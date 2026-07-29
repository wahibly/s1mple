#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "event.h"
#include "http.h"
#include "router.h"
#include "registry.h"
#include "proxy.h"
#include "static.h"
#include "security.h"
#include "config.h"
#include "logger.h"
#include "server_tcp.h"

#define MAX_EVENTS       64      // max events per epoll_wait() call
#define MAX_CONNS        4096    // max  connections simultany
#define KEEPALIVE_SECS   15

// Connection table (infex by fd)
static conn_t *conn_table[MAX_CONNS];

static conn_t *conn_new(int fd, const char *ip) {
    if (fd < 0 || fd >= MAX_CONNS) return NULL;

    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) return NULL;

    c->fd    = fd;
    c->state = CONN_READING_HEADERS;
    strncpy(c->client_ip, ip, sizeof(c->client_ip) - 1);

    conn_table[fd] = c;
    return c;
}

static void conn_free(conn_t *c) {
    if (!c) return;
    if (c->fd >= 0 && c->fd < MAX_CONNS)
        conn_table[c->fd] = NULL;
    if (c->wbuf) free(c->wbuf);
    free(c);
}

//  set_nonblocking
// Every fd in the epoll loop must be non-blocking.
// If read() would block, it returns -1 with errno=EAGAIN instead.
// This is what lets one thread handle thousands of connections —
// we never actually wait inside a syscall.
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//  epoll_add / epoll_mod/ epol_del
static int epoll_add(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events   = events;
    ev.data.fd  = fd;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events   = events;
    ev.data.fd  = fd;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void epoll_del(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}


static void conn_close(int epfd, conn_t *c) {
    log_debug("event", "closing fd=%d ip=%s after %d requests", c->fd, c->client_ip, c->requests);
    security_conn_release(c->client_ip);
    epoll_del(epfd, c->fd);
    close(c->fd);
    conn_free(c);
}

// Store a response to be written asynchronously.
// The event loop will call write() when EPOLLOUT fires
static void conn_set_response(conn_t *c, char *buf, int len) {
    if (c->wbuf) free(c->wbuf);
    c->wbuf      = buf;
    c->wbuf_len  = len;
    c->wbuf_sent = 0;
    c->state     = CONN_WRITING;
}


// NOTE: For simplicity we build the entire response in memory here
// A production epoll server would stream large responses in chunks
static char *build_response(conn_t *c, http_request_t *req, int *out_len) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return NULL;

    // ── Make the READ end non-blocking ────────────────────────────────
    // The write end (fake_fd) stays blocking so handlers work normally.
    // The read end we drain in a loop.
    int flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    // ── Increase socket buffer size to handle large responses ─────────
    int bufsize = 4 * 1024 * 1024;  // 4MB send buffer
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    int fake_fd = sv[0];

    // Dispatch
    int handled = router_dispatch(fake_fd, req, routes);

    if (!handled) {
        app_entry_t *app = registry_find_by_prefix(req->path);
        if (app) {
            proxy_forward(fake_fd, req, app->port, c->client_ip);
            handled = 1;
        }
    }

    if (!handled && strcmp(req->method, "GET") == 0) {
        char filepath[MAX_PATH_LEN];
        if (strcmp(req->path, "/") == 0) {
            snprintf(filepath, sizeof(filepath),
                     "%s/index.html", g_config.static_dir);
        } else {
            snprintf(filepath, sizeof(filepath),
                     "%s%s", g_config.static_dir, req->path);
        }

        if (!security_path_is_safe(filepath, g_config.static_dir)) {
            http_respond_error(fake_fd, "403 Forbidden", "Access denied.");
        } else {
            static_serve(fake_fd, req, filepath);
        }
        handled = 1;
    }

    if (!handled) {
        http_respond_error(fake_fd, "404 Not Found", "No route matched.");
    }

    // ── Close write end so read side sees EOF ─────────────────────────
    close(sv[0]);

    // ── Drain read end into a dynamic buffer ──────────────────────────
    // Start with 64KB, grow as needed
    int    capacity = 65536;
    int    total    = 0;
    char  *buf      = malloc(capacity);

    if (!buf) { close(sv[1]); return NULL; }

    char   tmp[65536];
    ssize_t n;

    // Keep reading until EOF (n==0) or EAGAIN (no more data right now)
    while (1) {
        n = read(sv[1], tmp, sizeof(tmp));

        if (n > 0) {
            // Grow buffer if needed
            if (total + n > capacity) {
                capacity = (total + n) * 2;
                char *newbuf = realloc(buf, capacity);
                if (!newbuf) {
                    free(buf);
                    close(sv[1]);
                    return NULL;
                }
                buf = newbuf;
            }
            memcpy(buf + total, tmp, n);
            total += n;

        } else if (n == 0) {
            // EOF — done
            break;

        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data available right now
                // Since we closed the write end, this means we got everything
                break;
            }
            // Real error
            free(buf);
            close(sv[1]);
            return NULL;
        }
    }

    close(sv[1]);

    *out_len = total;
    return buf;
}

// called when epoll says fd is readable.
static void handle_read(int epfd, conn_t *c) {
    // Read available bytes into the connection's read buffer
    ssize_t n = read(c->fd,
                     c->rbuf + c->rbuf_len,
                     sizeof(c->rbuf) - 1 - c->rbuf_len);

    if (n == 0) {
        conn_close(epfd, c);
        return;
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn_close(epfd, c);
        return;
    }

    c->rbuf_len += n;
    c->rbuf[c->rbuf_len] = '\0';

    // Size limit check
    if (!security_request_size_ok(c->rbuf_len)) {
        const char *resp =
            "HTTP/1.1 413 Payload Too Large\r\n"
            "Content-Length: 28\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Request exceeds size limit.\n";
        int   len = strlen(resp);
        char *buf = malloc(len);
        if (buf) {
            memcpy(buf, resp, len);
            conn_set_response(c, buf, len);
            c->keep_alive = 0;
            epoll_mod(epfd, c->fd, EPOLLOUT);
        } else {
            conn_close(epfd, c);
        }
        return;
    }

    if (c->state == CONN_READING_HEADERS) {
        // Wait until we have the full headers
        if (!strstr(c->rbuf, "\r\n\r\n")) return;

        // Parse the request
        http_request_t req;
        if (http_parse(c->rbuf, c->rbuf_len, &req) < 0) {
            const char *resp =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 17\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Malformed request";
            int   len = strlen(resp);
            char *buf = malloc(len);
            if (buf) {
                memcpy(buf, resp, len);
                conn_set_response(c, buf, len);
                c->keep_alive = 0;
                epoll_mod(epfd, c->fd, EPOLLOUT);
            } else {
                conn_close(epfd, c);
            }
            return;
        }

        c->keep_alive = req.keep_alive;

        // If POST body not fully received yet switch to body reading
        if (req.content_length > 0 &&
            req.body_len < req.content_length) {
            c->state = CONN_READING_BODY;
            return;
        }

        // Full request received — dispatch it
        log_info("event", "[%s] %s %s",
                 c->client_ip, req.method, req.path);

        // ── Check if route table matches ──────────────────────────────
        int router_matched = 0;
        {
            char path_only[MAX_PATH_LEN];
            strncpy(path_only, req.path, MAX_PATH_LEN - 1);
            path_only[MAX_PATH_LEN - 1] = '\0';
            char *q = strchr(path_only, '?');
            if (q) *q = '\0';

            for (int ri = 0; routes[ri].path != NULL; ri++) {
                if (strcmp(path_only, routes[ri].path) == 0) {
                    router_matched = 1;
                    break;
                }
            }
        }

        // ── Check if a deployed app owns this prefix ──────────────────
        app_entry_t *app = registry_find_by_prefix(req.path);

        if (!router_matched && !app && strcmp(req.method, "GET") == 0) {
            // ── Static file — serve directly to avoid socketpair limits ──
            char filepath[MAX_PATH_LEN];
            if (strcmp(req.path, "/") == 0) {
                snprintf(filepath, sizeof(filepath),
                         "%s/index.html", g_config.static_dir);
            } else {
                snprintf(filepath, sizeof(filepath),
                         "%s%s", g_config.static_dir, req.path);
            }

            if (!security_path_is_safe(filepath, g_config.static_dir)) {
                http_respond_error(c->fd, "403 Forbidden", "Access denied.");
            } else {
                static_serve(c->fd, &req, filepath);
            }

            // Handle keep-alive after direct send
            c->requests++;
            if (!c->keep_alive) {
                conn_close(epfd, c);
            } else {
                c->state    = CONN_READING_HEADERS;
                c->rbuf_len = 0;
                memset(c->rbuf, 0, sizeof(c->rbuf));
                epoll_mod(epfd, c->fd, EPOLLIN);
            }
            return;
        }

        // ── Router or proxy — capture via socketpair ──────────────────
        // These responses are always small (JSON, HTML snippets)
        // so the socketpair buffer is never a bottleneck here
        int   resp_len = 0;
        char *resp_buf = build_response(c, &req, &resp_len);

        if (!resp_buf) {
            conn_close(epfd, c);
            return;
        }

        conn_set_response(c, resp_buf, resp_len);
        epoll_mod(epfd, c->fd, EPOLLOUT);

    } else if (c->state == CONN_READING_BODY) {
        // Keep accumulating until we have the full body
        http_request_t req;
        if (http_parse(c->rbuf, c->rbuf_len, &req) < 0) {
            conn_close(epfd, c);
            return;
        }

        if (req.content_length > 0 &&
            req.body_len < req.content_length) {
            return; // still need more body bytes
        }

        // Full body received
        log_info("event", "[%s] %s %s (body complete)",
                 c->client_ip, req.method, req.path);

        int   resp_len = 0;
        char *resp_buf = build_response(c, &req, &resp_len);

        if (!resp_buf) {
            conn_close(epfd, c);
            return;
        }

        conn_set_response(c, resp_buf, resp_len);
        epoll_mod(epfd, c->fd, EPOLLOUT);
    }
}

// called when epoll says fd is writable.
static void handle_write(int epfd, conn_t *c) {
    // Write as many bytes as the socket will accept right now
    ssize_t n = write(c->fd, c->wbuf + c->wbuf_sent, c->wbuf_len - c->wbuf_sent);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // buffer full, try later
        conn_close(epfd, c);
        return;
    }

    c->wbuf_sent += n;

    if (c->wbuf_sent < c->wbuf_len) {
        // Not done yet, stay in EPOLLOUT and write more next time
        return;
    }

    // Response fully sent
    c->requests++;
    free(c->wbuf);
    c->wbuf     = NULL;
    c->wbuf_len = 0;
    c->wbuf_sent = 0;

    if (!c->keep_alive) {
        conn_close(epfd, c);
        return;
    }

    // Keep-alive: reset to reading state for the next request
    c->state       = CONN_READING_HEADERS;
    c->rbuf_len    = 0;
    memset(c->rbuf, 0, sizeof(c->rbuf));

    // Switch back to watching for reads
    epoll_mod(epfd, c->fd, EPOLLIN);
}

void event_loop_run(int server_fd) {
    // Create the epoll instance
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        log_fatal("event", "epoll_create1 failed");
    }

    // Add the listening socket to epoll
    // When server_fd becomes readable, a new connection is ready to accept
    set_nonblocking(server_fd);
    epoll_add(epfd, server_fd, EPOLLIN);

    log_info("event", "epoll event loop started");

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        // Block until at least one fd is ready.
        // Timeout of 15s, used to check for idle connections
        int nready = epoll_wait(epfd, events, MAX_EVENTS, KEEPALIVE_SECS * 1000);

        if (nready < 0) {
            if (errno == EINTR) continue; 
            log_error("event", "epoll_wait failed");
            break;
        }

        if (nready == 0) {
            // Timeout, no events in a full implementation we had
            // walk the conn_table and close idle connections here
            // For now we just note it
            log_debug("event", "epoll timeout, no events");
            continue;
        }

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // New connection 
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                int conn_fd = accept(server_fd,(struct sockaddr *)&client_addr, &client_len);
                if (conn_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        log_error("event", "accept failed");
                    }
                    continue;
                }

                char ip[46];
                strncpy(ip, inet_ntoa(client_addr.sin_addr),
                        sizeof(ip) - 1);

                // Check connection limit
                if (!security_conn_allow(ip)) {
                    const char *resp =
                        "HTTP/1.1 429 Too Many Requests\r\n"
                        "Content-Length: 22\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Too many connections.\n";
                    write(conn_fd, resp, strlen(resp));
                    close(conn_fd);
                    continue;
                }

                // Make non-blocking and add to epoll
                set_nonblocking(conn_fd);

                conn_t *c = conn_new(conn_fd, ip);
                if (!c) {
                    log_error("event", "conn_table full");
                    close(conn_fd);
                    continue;
                }

                epoll_add(epfd, conn_fd, EPOLLIN);
                log_info("event", "new connection fd=%d ip=%s", conn_fd, ip);

            } else {
                // Existing connection event 
                conn_t *c = (fd >= 0 && fd < MAX_CONNS)? conn_table[fd] : NULL;

                if (!c) {
                    // Stale event, fd was already closed
                    epoll_del(epfd, fd);
                    continue;
                }

                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    // Error or hangup on this connection
                    conn_close(epfd, c);
                    continue;
                }

                if (events[i].events & EPOLLIN) {
                    handle_read(epfd, c);
                }

                if (events[i].events & EPOLLOUT) {
                    handle_write(epfd, c);
                }
            }
        }
    }

    close(epfd);
}