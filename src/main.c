#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>  
#include <arpa/inet.h> //IP->String (inet_ntoa())
#include "server_tcp.h"
#include "http.h"
#include "router.h"
#include <pthread.h>
#include "registry.h"
#include "proxy.h"
#include "logger.h"
#include "security.h"
#include "config.h"
#include <time.h>
#include <static.h>
#include "event.h"
#include "lb.h"
#include "cache.h"

#define KEEPALIVE_TIMEOUT_SEC  15
#define MAX_REQUESTS_PER_CONN  100

void router_set_start_time(void);
static void handle_connection(int conn_fd, const char *client_ip);

typedef struct {
    int conn_fd;
    char client_ip[INET_ADDRSTRLEN];
} thread_args_t;

static void *connection_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    int  conn_fd   = args->conn_fd;
    char client_ip[INET_ADDRSTRLEN];
    strncpy(client_ip, args->client_ip, INET_ADDRSTRLEN - 1);
    free(args);

    handle_connection(conn_fd, client_ip);
    close(conn_fd);
    security_conn_release(client_ip);
    return NULL;
}


int main(void) {
    lb_init();
    cache_init();
    config_load("s1mple.conf");
    logger_init(g_config.log_file, g_config.log_level);
    config_print();

    registry_init();

    int server_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);


    server_fd = socket(AF_INET, SOCK_STREAM, 0); //TCP
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    //without this if you restart the server quickly bind() fails with
    // "Address already in use" because the old socket is in TIME_WAIT state
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(g_config.port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if(listen(server_fd, SERVER_BACKLOG)<0){
        perror("listern");
    }

    
    log_info("server", "s1mple listening on http://localhost:%d [%s]", g_config.port, g_config.concurrency);

    if (strcmp(g_config.concurrency, "epoll") == 0) {
        event_loop_run(server_fd);
    } else {
        // original pthread model
        while (1) {
            conn_fd = accept(server_fd,
                             (struct sockaddr *)&client_addr,
                             &client_len);
            if (conn_fd < 0) { perror("accept"); continue; }

            char client_ip[INET_ADDRSTRLEN];
            strncpy(client_ip, inet_ntoa(client_addr.sin_addr),
                    INET_ADDRSTRLEN - 1);

            if (!security_conn_allow(client_ip)) {
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

            log_info("server", "connection from %s:%d",
                     client_ip, ntohs(client_addr.sin_port));

            thread_args_t *args = malloc(sizeof(thread_args_t));
            if (!args) {
                security_conn_release(client_ip);
                close(conn_fd);
                continue;
            }
            args->conn_fd = conn_fd;
            strncpy(args->client_ip, client_ip, INET_ADDRSTRLEN - 1);

            pthread_t thread;
            if (pthread_create(&thread, NULL,
                               connection_thread, args) != 0) {
                log_error("server", "pthread_create failed");
                security_conn_release(client_ip);
                free(args);
                close(conn_fd);
                continue;
            }
            pthread_detach(thread);
        }
    }
    close(server_fd);
    logger_close();
    return 0;

}

static void handle_connection(int conn_fd, const char *client_ip) {
    int requests_handled = 0;

    while (requests_handled < MAX_REQUESTS_PER_CONN) {

        // set a read timeout so an idle keep-alive connection doesnt
        // hold the thread forever
        struct timeval tv;
        tv.tv_sec = KEEPALIVE_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char raw[BUFFER_SIZE];
        int total = 0;
        ssize_t n;
        int headers_complete = 0;

        while (total < (int)sizeof(raw) - 1) {
            n = read(conn_fd, raw + total, sizeof(raw) - 1 - total);
            if (n <= 0) return;        
            total += n;
            raw[total] = '\0';

            if (!security_request_size_ok(total)) {
                http_respond_error(conn_fd, "413 Payload Too Large",
                                "Request exceeds maximum allowed size.");
                return;
            }
            // stop once we have the full headers
            if (strstr(raw, "\r\n\r\n")){
                headers_complete = 1;
                break;
            }
        }

        if (!headers_complete) return;

        http_request_t req;
        if (http_parse(raw, total, &req) < 0) {
                log_warn("server", "malformed request from client");
                http_respond_error(conn_fd, "400 Bad Request", "Malformed request.");
                return;
        }
        char *body_buf       = NULL;
        int   body_total     = req.body_len;
        int   need_more_body = req.content_length > 0 && req.content_length > req.body_len;

        if (need_more_body) {

            body_buf = malloc(req.content_length + 1);
            if (!body_buf) {
                http_respond_error(conn_fd, "500 Internal Server Error","Out of memory.");
                return;
            }
            memcpy(body_buf, req.body, req.body_len);
            body_total = req.body_len;

            while (body_total < req.content_length) {
                n = read(conn_fd, body_buf + body_total,
                        req.content_length - body_total);
                if (n <= 0) { free(body_buf); return; }
                body_total += n;
            }
            body_buf[body_total] = '\0';

            req.body     = body_buf;
            req.body_len = body_total;
        }

        log_info("server", "[%s] %s %s (keep-alive=%d)",
                 client_ip, req.method, req.path, req.keep_alive);
        printf("[%s] %s\n", req.method, req.path);

        if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "POST") != 0) { //for now just GET
            
            log_warn("server", "method not allowed: %s", req.method);
            http_respond_error(conn_fd, "405 Method Not Allowed",
                            "Only GET & POST is supported.");
            if(body_buf) free(body_buf);
            return;
        }

        log_debug("server", "dispatch: path='%s' lb_routes=%d",
          req.path, g_lb.route_count);

        lb_route_t *test = lb_find_route(req.path);
        log_debug("server", "lb_find_route result: %s",
                test ? test->prefix : "NULL");

        int handled = router_dispatch(conn_fd, &req, routes);
        
        if (!handled) {
            lb_route_t *lb_route = lb_find_route(req.path);
            if (lb_route) {
                log_info("server", "lb routing %s", req.path);
                proxy_forward_lb(conn_fd, &req, client_ip);
                handled = 1;
            }
        }
        if (!handled) {
            app_entry_t *app = registry_find_by_prefix(req.path);
            if (app) {
                log_info("server", "proxying %s → %s:%d", req.path, app->name, app->port);
                proxy_forward_cached(conn_fd, &req, app->port, client_ip);
                handled = 1;
            }
        }
        

        if (!handled && strcmp(req.method, "GET") == 0) {
            char filepath[MAX_PATH_LEN];
            if (strcmp(req.path, "/") == 0) {
                snprintf(filepath, sizeof(filepath), "%s/index.html", g_config.static_dir);
            } else {
                snprintf(filepath, sizeof(filepath), "%s%s", g_config.static_dir, req.path);
            }

            if (!security_path_is_safe(filepath, g_config.static_dir)) {
                http_respond_error(conn_fd, "403 Forbidden", "Access denied.");
                if (body_buf) free(body_buf);
                return;
            }

            static_serve(conn_fd, &req, filepath);
            handled = 1;
        } else if (!handled) {
            http_respond_error(conn_fd, "404 Not Found", "No route matched.");
        }

        if(body_buf) free(body_buf);

        if(!req.keep_alive) {
            return;
        }

        requests_handled++;
    }

    log_debug("server", "[%s] reached max requests per connection, closing",
             client_ip);
}