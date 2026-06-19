#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "../include/http.h"
#include "../include/server_tcp.h"

int http_parse(const char *raw, int raw_len, http_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
    req->keep_alive = 1; // HTTP/1.1 keeps connections open by default (persistent connections)

    const char *line_end = strstr(raw, "\r\n");
    if (!line_end) return -1;

    // Copy just the first line into a temp buffer
    char first_line[512];
    int  line_len = (int)(line_end - raw);
    if (line_len >= (int)sizeof(first_line)) return -1;

    memcpy(first_line, raw, line_len);
    first_line[line_len] = '\0';

    if (sscanf(first_line, "%15s %511s %15s",
               req->method, req->path, req->version) != 3) {
        return -1;
    }

    // HTTP/1.0 opens a new TCP connection for each request
    if(strcmp(req->version, "HTTP/1.0")==0){
        req->keep_alive = 0;
    }

    const char *cursor = line_end + 2; // skip past first \r\n

    while (cursor < raw + raw_len) {
        const char *end = strstr(cursor, "\r\n");
        if (!end) break;

        int len = (int)(end - cursor);
        if (len == 0) {
            cursor = end + 2;
            break;
        }

        // parse "Name: Value"
        if (req->header_count < MAX_HEADERS) {
            const char *colon = memchr(cursor, ':', len);
            if (colon) {
                int name_len  = (int)(colon - cursor);
                int value_off = 1;
                while (colon[value_off] == ' ') value_off++;
                int value_len = len - name_len - value_off;

                if (name_len  >= MAX_HEADER_LEN) name_len  = MAX_HEADER_LEN - 1;
                if (value_len >= MAX_HEADER_LEN) value_len = MAX_HEADER_LEN - 1;
                if (value_len < 0) value_len = 0;

                memcpy(req->headers[req->header_count].name,
                       cursor, name_len);
                req->headers[req->header_count].name[name_len] = '\0';

                memcpy(req->headers[req->header_count].value,
                       colon + value_off, value_len);
                req->headers[req->header_count].value[value_len] = '\0';

                req->header_count++;
            }
        }

        cursor = end + 2;
    }


    req->body     = (char *)cursor;
    req->body_len = (int)(raw + raw_len - cursor);
    if (req->body_len < 0) req->body_len = 0;

    const char *c1 = http_get_header(req, "Content-Length");
    if(c1) {
        req->content_length = atoi(c1);
    }

    const char *conn = http_get_header(req, "Connection");
    if(conn) {
        if(strcasecmp(conn, "close") == 0)  req->keep_alive = 0;
        else if (strcasecmp(conn, "keep-alive") == 0) req->keep_alive = 1;
    }

    return 0;
}

const char *http_get_header(const http_request_t *req, const char *name) {
    for (int i = 0; i<req->header_count; i++){
        if(strcasecmp(req->headers[i].name, name) == 0){
            return req->headers[i].value;
        } 
    }
    return NULL;
}

//http response
void http_respond(int conn_fd,
                  const char *status_text,
                  const char *content_type,
                  const char *body,
                  int body_len){
    http_respond_ex(conn_fd, status_text, content_type, body, body_len, 0);
}
//http response
void http_respond_ex(int conn_fd,
                  const char *status_text,
                  const char *content_type,
                  const char *body,
                  int body_len,
                  int keep_alive) {

    char headers[BUFFER_SIZE];
    int header_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"     
        "\r\n",
        status_text, content_type, body_len,
        keep_alive ? "keep-alive" : "close");

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

    //errors close the connection (safe)
    http_respond_ex(conn_fd, status_text, "text/html", body, body_len, 0);
}

void http_chunked_start(int conn_fd, const char *status_text, const char *content_type, int keep_alive) {
    char headers[512];
    int len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status_text, content_type,
        keep_alive ? "keep-alive" : "close");

    write(conn_fd, headers, len);
}

// each chunk: <hex-length>\r\n<data>\r\n
void http_chunk_send(int conn_fd, const char *data, int len) {
    if (len <= 0) return;

    char size_line[32];
    int  size_len = snprintf(size_line, sizeof(size_line), "%x\r\n", len);

    write(conn_fd, size_line, size_len);
    write(conn_fd, data, len);
    write(conn_fd, "\r\n", 2);
}
// terminating chunk: 0\r\n\r\n
void http_chunk_end(int conn_fd) {
    write(conn_fd, "0\r\n\r\n", 5);
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