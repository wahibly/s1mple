#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/http.h"
#include "../include/server_tcp.h"

int http_parse(const char *raw, int raw_len, http_request_t *req) {
    memset(req, 0, sizeof(*req));


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