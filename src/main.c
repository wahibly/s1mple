#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      
#include <sys/socket.h> 
#include <netinet/in.h>  
#include <arpa/inet.h> //IP->String (inet_ntoa())
#include "../include/server_tcp.h"
#include "../include/http.h"
#include "../include/router.h"
#include <pthread.h>
#include "../include/registry.h"
#include "../include/proxy.h"



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
    return NULL;
}


int main(void) {
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
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if(listen(server_fd, SERVER_BACKLOG)<0){
        perror("listern");
    }

    printf("Server listening on port %d...\n", SERVER_PORT);
    printf("Connect with: nc localhost %d\n\n", SERVER_PORT); //nc: Netcat, a command-line tool used to create TCP/UDP connections

    registry_init();

    while(1){
        conn_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue; //not kill server en cas bad accept
        }

        char client_ip[INET_ADDRSTRLEN];
        strncpy(client_ip, inet_ntoa(client_addr.sin_addr), INET_ADDRSTRLEN - 1);

        printf("New connection from %s:%d (fd=%d)\n",
               client_ip,
               ntohs(client_addr.sin_port),
               conn_fd);

        thread_args_t *args = malloc(sizeof(thread_args_t));
        if (!args) { close(conn_fd); continue; }
        args->conn_fd = conn_fd;

        strncpy(args->client_ip, client_ip, INET_ADDRSTRLEN - 1);

        pthread_t thread;
        if (pthread_create(&thread, NULL, connection_thread, args) != 0) {
            perror("pthread_create");
            free(args);
            close(conn_fd);
            continue;
        }

        pthread_detach(thread);
        printf("Connection closed.\n\n");

    }
    close(server_fd);
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

        // stop once we have the full headers
        if (strstr(raw, "\r\n\r\n")) break;
    }

    if (total == 0) return;

    http_request_t req;
    if (http_parse(raw, total, &req) < 0) {
            http_respond_error(conn_fd, "400 Bad Request", "Malformed request.");
            return;
    }

    printf("[%s] %s\n", req.method, req.path);

    if (strcmp(req.method, "GET") != 0) { //for now just GET    
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
            printf("[proxy] routing %s -> %s (port %d)\n", req.path, app->name, app->port);
            proxy_forward(conn_fd, &req, app->port, client_ip);
            return;
    }

    //serve static files
    char filepath[MAX_PATH_LEN];
    const char *req_path = req.path;

    if (strcmp(req_path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", STATIC_DIR); //to-do: secure critical path!!!
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", STATIC_DIR, req_path);
    }

    char *body = NULL;
    int   body_len = read_file(filepath, &body);

    if (body_len < 0) {
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