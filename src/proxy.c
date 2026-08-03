#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "proxy.h"
#include "http.h"
#include "logger.h"
#include "server_tcp.h"
#include <errno.h>
#include "config.h"
#include <sys/time.h> 
#include "lb.h"
#include "cache.h"
#include <fcntl.h>


//proxy connect: open tcp connection to localhost:port
static int proxy_connect(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("proxy: socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if(connect(sock, (struct sockaddr *)&addr, sizeof(addr))<0) {
        log_error("proxy", "could not connect to backend port %d", port);
        close(sock);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec  = g_config.proxy_timeout;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        log_warn("proxy", "could not set read timeout");
    }
    return sock;
}


//build the proxy to backend + ip of client
static int proxy_build_request(const http_request_t *req, const char *client_ip, char *out, int out_size){

    int pos=0;

    pos += snprintf(out + pos, out_size - pos,"%s %s %s\r\n", req->method, req->path, req->version);
    
    //forwad original headers
    for (int i = 0; i< req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Connection") == 0) continue;

        pos += snprintf(out + pos, out_size - pos,"%s: %s\r\n", req->headers[i].name, req->headers[i].value);
    }

    //add proxy header
    pos += snprintf(out + pos, out_size - pos, "X-Forwarded-For: %s\r\n", client_ip);
    pos += snprintf(out + pos, out_size - pos, "X-Forwarded-Host: localhost:%d\r\n", SERVER_PORT);
    pos += snprintf(out + pos, out_size - pos, "Connection: close\r\n");

    pos += snprintf(out + pos, out_size - pos, "\r\n");

    if (req->body && req->body_len > 0) {
        int copy = req->body_len;
        if (copy > out_size - pos - 1) copy = out_size - pos - 1;
        memcpy(out + pos, req->body, copy);
        pos += copy;
    }

    return pos;
}

int proxy_forward(int conn_fd, http_request_t *req, int backend_port, const char *client_ip) {

    int backend_fd = proxy_connect(backend_port);
    if(backend_fd < 0) {
        http_respond_error(conn_fd, "502 Bad Gateway",  "Could not connect to backend app.");
        return -1;
    }

    log_info("proxy", "connected to backend port %d", backend_port);

    char request_buf[BUFFER_SIZE * 2];
    int request_len = proxy_build_request(req, client_ip, request_buf, sizeof(request_buf));

    if(write(backend_fd, request_buf, request_len) < 0) {
        log_error("proxy", "write to backend failed");
        close(backend_fd);
        http_respond_error(conn_fd, "502 Bad Gateway", "Failed to send request to backend.");
        return -1;
    }

    log_info("proxy", "forwarded %d bytes to backend port %d", request_len, backend_port);

    char relay_buf[BUFFER_SIZE];
    ssize_t n;
    int total_relayed = 0;

    while ((n = read(backend_fd, relay_buf, sizeof(relay_buf))) > 0) {
        ssize_t writen = write(conn_fd, relay_buf, n);
        if (writen < 0) {
            log_error("proxy", "write to client failed");
            break;
        }
        total_relayed += writen;
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            log_warn("proxy", "backend port %d timed out after %ds", backend_port, g_config.proxy_timeout);
            if (total_relayed == 0) {
                http_respond_error(conn_fd, "504 Gateway Timeout",
                                   "Backend app did not respond in time.");
            }
        } else {
            log_error("proxy", "read from backend failed: %s",
                      strerror(errno));
        }
    }

    log_info("proxy", "relayed %d bytes to client", total_relayed);

    close(backend_fd);
    return 0;
}


int proxy_forward_lb(int conn_fd,
                     http_request_t *req,
                     const char *client_ip) {

    // pick a backend
    lb_backend_t *backend = lb_pick(req->path);
    if (!backend) {
        http_respond_error(conn_fd, "503 Service Unavailable", "No healthy backends available.");
        return -1;
    }

    log_info("proxy", "lb → %s:%d for %s", backend->host, backend->port, req->path);

    // try to connect
    int backend_fd = proxy_connect(backend->port);
    if (backend_fd < 0) {
        // connection failed —> mark unhealthy and return 502
        lb_set_health(backend, 0);
        lb_release(backend);
        http_respond_error(conn_fd, "502 Bad Gateway","Backend unavailable.");
        return -1;
    }
    lb_set_health(backend, 1);

    // build and send request
    char request_buf[BUFFER_SIZE * 2];
    int  request_len = proxy_build_request(req, client_ip,
                                           request_buf,
                                           sizeof(request_buf));
    if (write(backend_fd, request_buf, request_len) < 0) {
        log_error("proxy", "write to backend failed");
        close(backend_fd);
        lb_release(backend);
        http_respond_error(conn_fd, "502 Bad Gateway", "Failed to send request to backend.");
        return -1;
    }

    char    relay_buf[BUFFER_SIZE];
    ssize_t n;
    int     total_relayed = 0;

    while ((n = read(backend_fd, relay_buf, sizeof(relay_buf))) > 0) {
        if (write(conn_fd, relay_buf, n) < 0) break;
        total_relayed += n;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        log_warn("proxy", "lb backend %s:%d timed out",
                 backend->host, backend->port);
        if (total_relayed == 0) {
            http_respond_error(conn_fd, "504 Gateway Timeout", "Backend did not respond in time.");
        }
    }

    log_info("proxy", "lb relayed %d bytes from %s:%d", total_relayed, backend->host, backend->port);

    close(backend_fd);
    lb_release(backend);
    return 0;
}


int proxy_forward_cached(int conn_fd,
                          http_request_t *req,
                          int backend_port,
                          const char *client_ip) {

    int ttl = cache_get_ttl(req->path);

    if (ttl > 0 && strcmp(req->method, "GET") == 0) {
        int   cached_len = 0;
        int   cached_age = 0;
        char *cached     = cache_get(req->path, &cached_len, &cached_age);

        if (cached) {
            const char *header_end = strstr(cached, "\r\n");
            if (header_end) {
                write(conn_fd, cached,
                      (int)(header_end - cached));

                char cache_headers[128];
                int  ch_len = snprintf(cache_headers,
                                       sizeof(cache_headers),
                    "\r\nX-Cache: HIT\r\nX-Cache-Age: %d",
                    cached_age);
                write(conn_fd, cache_headers, ch_len);

                write(conn_fd,
                      header_end,
                      cached_len - (int)(header_end - cached));
            } else {
                write(conn_fd, cached, cached_len);
            }

            free(cached);
            log_info("cache", "served from cache: %s (age=%ds)",
                     req->path, cached_age);
            return 0;
        }
    }

    // cache MISS

    if (ttl > 0 && strcmp(req->method, "GET") == 0) {
        // Capture via socketpair
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            return proxy_forward(conn_fd, req, backend_port, client_ip);
        }

        // Forward to backend, capture into sv[0]
        proxy_forward(sv[0], req, backend_port, client_ip);
        close(sv[0]);

        // Read captured response
        int    cap_size = CACHE_MAX_VAL_SIZE;
        char  *cap_buf  = malloc(cap_size);
        int    cap_total = 0;
        ssize_t n;

        int flags = fcntl(sv[1], F_GETFL, 0);
        fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

        while (cap_total < cap_size - 1) {
            n = read(sv[1], cap_buf + cap_total,
                     cap_size - 1 - cap_total);
            if (n <= 0) break;
            cap_total += n;
        }
        close(sv[1]);

        if (cap_total > 0) {
            // Store in cache
            cache_set(req->path, cap_buf, cap_total, ttl);

            // Send to client with X-Cache: MISS header
            const char *header_end = strstr(cap_buf, "\r\n");
            if (header_end) {
                write(conn_fd, cap_buf,
                      (int)(header_end - cap_buf));
                char ch[64];
                int  ch_len = snprintf(ch, sizeof(ch),
                                       "\r\nX-Cache: MISS");
                write(conn_fd, ch, ch_len);
                write(conn_fd, header_end,
                      cap_total - (int)(header_end - cap_buf));
            } else {
                write(conn_fd, cap_buf, cap_total);
            }
            log_info("cache", "stored: %s (%d bytes, TTL=%ds)",
                     req->path, cap_total, ttl);
        }

        free(cap_buf);
        return 0;
    }

    // Not cacheable —> forward directly
    return proxy_forward(conn_fd, req, backend_port, client_ip);
}