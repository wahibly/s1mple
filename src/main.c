#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      
#include <sys/socket.h> 
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

    
    log_info("server", "s1mple listening on http://localhost:%d", g_config.port);

    router_set_start_time();
    while(1){
        conn_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue; //not kill server en cas bad accept
        }

        char client_ip[INET_ADDRSTRLEN];
        strncpy(client_ip, inet_ntoa(client_addr.sin_addr), INET_ADDRSTRLEN - 1);

        if (!security_conn_allow(client_ip)) {
            const char *resp =
                "HTTP/1.1 429 Too Many Requests\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 23\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Too many connections.\r\n";
            write(conn_fd, resp, strlen(resp));
            close(conn_fd);
            continue;
        }
        log_info("server", "connection from %s:%d", client_ip, ntohs(client_addr.sin_port));

        thread_args_t *args = malloc(sizeof(thread_args_t));
        if (!args) { close(conn_fd); continue; }
        args->conn_fd = conn_fd;

        strncpy(args->client_ip, client_ip, INET_ADDRSTRLEN - 1);

        pthread_t thread;
        if (pthread_create(&thread, NULL, connection_thread, args) != 0) {
            log_error("server", "pthread_create failed");
            free(args);
            close(conn_fd);
            continue;
        }

        pthread_detach(thread);

    }
    close(server_fd);
    logger_close();
    return 0;

}

static void handle_connection(int conn_fd, const char *client_ip) {
    char raw[BUFFER_SIZE];
    int total = 0;
    ssize_t n;

    while (total < (int)sizeof(raw) - 1) {
        n = read(conn_fd, raw + total, sizeof(raw) - 1 - total);
        if (n <= 0) break;        
        total += n;
        raw[total] = '\0';

        if (!security_request_size_ok(total)) {
            http_respond_error(conn_fd, "413 Payload Too Large",
                               "Request exceeds maximum allowed size.");
            return;
        }
        // stop once we have the full headers
        if (strstr(raw, "\r\n\r\n")) break;
    }

    if (total == 0) return;

    http_request_t req;
    if (http_parse(raw, total, &req) < 0) {
            log_warn("server", "malformed request from client");
            http_respond_error(conn_fd, "400 Bad Request", "Malformed request.");
            return;
    }

    printf("[%s] %s\n", req.method, req.path);

    if (strcmp(req.method, "GET") != 0) { //for now just GET
        
        log_warn("server", "method not allowed: %s", req.method);
        http_respond_error(conn_fd, "405 Method Not Allowed",
                           "Only GET is supported.");
        return;
    }

    if (router_dispatch(conn_fd, &req, routes)) {
        return;
    }

    //check if pth start with a known app
    app_entry_t *app = registry_find_by_prefix(req.path);
    if (app) {
            log_info("proxy", "routing %s -> %s (port %d)", req.path, app->name, app->port);
            proxy_forward(conn_fd, &req, app->port, client_ip);
            return;
    }

    //serve static files
    char filepath[MAX_PATH_LEN];
    const char *req_path = req.path;

    if (strcmp(req_path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", g_config.static_dir); //to-do: secure critical path!!!
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s",g_config.static_dir, req_path);
    }


    if (!security_path_is_safe(filepath, g_config.static_dir)) {
        http_respond_error(conn_fd, "403 Forbidden", "Access denied.");
        return;
    }

    char *body = NULL;
    int   body_len = read_file(filepath, &body);

    if (body_len < 0) {
        log_warn("server", "404 not found: %s", filepath);
        http_respond_error(conn_fd, "404 Not Found",
                           "The requested file was not found!");
        return;
    }

    const char *content_type = "text/plain";
    if (strstr(filepath, ".html")) content_type = "text/html";
    else if (strstr(filepath, ".css"))  content_type = "text/css";
    else if (strstr(filepath, ".js"))   content_type = "application/javascript";
    else if (strstr(filepath, ".json")) content_type = "application/json";

    http_respond(conn_fd, "200 OK", content_type, body, body_len);
    free(body); 

}