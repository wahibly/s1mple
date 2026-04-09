#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/http.h"
#include "../include/server_tcp.h"

//parse raw hhtp string to http_request_t struct
int http_parse(const char *raw, int raw_len, http_request_t *req) {
    (void)raw_len;
    memset(req, 0, sizeof(*req));

    char *buf = strdup(raw); //copy raw
    if (!buf) return -1;

    char *line = strtok(buf, "\r\n"); //get first line
    if (!line) { free(buf); return -1; }

    if (sscanf(line, "%15s %511s %15s",
        req->method, req->path, req->version) != 3) {
        free(buf);
        return -1;
    }

    //parse headers: (Name: Value)
    while ((line = strtok(NULL, "\r\n")) != NULL) {
        if (strlen(line) == 0) break; 

        if (req->header_count >= MAX_HEADERS) break;

        char *colon = strchr(line, ':');
        if (!colon) continue; 

        *colon = '\0';
        char *name  = line;
        char *value = colon + 1;

        while (*value == ' ') value++;

        strncpy(req->headers[req->header_count].name,
                name, MAX_HEADER_LEN - 1);
        strncpy(req->headers[req->header_count].value,
                value, MAX_HEADER_LEN - 1);
        req->header_count++;
    }
    //locate body -> start after \r\b\r\n
    const char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        req->body = (char *)(body_start + 4);
        req->body_len = (int)(raw + raw_len - req->body);
        if (req->body_len < 0) req->body_len = 0;
    }

    free(buf);
    return 0;

}

//http response
void http_respond(int conn_fd,
                  const char *status_text,
                  const char *content_type,
                  const char *body,
                  int body_len) {

    char headers[BUFFER_SIZE];
    int header_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"     
        "\r\n",
        status_text, content_type, body_len);

    write(conn_fd, headers, header_len);
    if (body && body_len > 0) {
        write(conn_fd, body, body_len);
    }
}

void http_respond_error(int conn_fd,
                         const char *status_text,
                         const char *message) {
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%s</h1><p>%s</p></body></html>",
        status_text, message);

    http_respond(conn_fd, status_text, "text/html", body, body_len);
}

//read file
int read_file(const char *path, char **out_buf) {
    FILE *f = fopen(path, "rb");  
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    *out_buf = malloc(size + 1);
    if (!*out_buf) { fclose(f); return -1; }

    long bytes_read = fread(*out_buf, 1, size, f);
    (*out_buf)[bytes_read] = '\0';

    fclose(f);
    return (int)bytes_read;
}