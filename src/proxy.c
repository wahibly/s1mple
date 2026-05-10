#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/proxy.h"
#include "http.h"
#include "logger.h"
#include "../include/server_tcp.h"



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

    log_info("proxy", "relayed %d bytes to client", total_relayed);

    close(backend_fd);
    return 0;
}