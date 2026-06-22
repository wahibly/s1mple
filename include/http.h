#ifndef HTTP_H
#define HTTP_H

#define MAX_HEADERS     32
#define MAX_HEADER_LEN  256
#define MAX_PATH_LEN    512
#define MAX_METHOD_LEN  16


typedef struct{
    char name[MAX_HEADER_LEN];
    char value[MAX_HEADER_LEN];

} http_header_t;

typedef struct {
    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    char version[16];
    http_header_t headers[MAX_HEADERS];
    int header_count;
    char *body;
    int body_len;
    int content_length; //-1 if absent
    int keep_alive;
} http_request_t;

int http_parse(const char *raw ,int raw_len, http_request_t *req);

const char *http_get_header(const http_request_t *req, const char *name);

void http_respond(int conn_fd,
                   const char *status_text,
                   const char *content_type,
                   const char *body,
                   int body_len);

//same as hhtp respond but let caller control connection header
void http_respond_ex(int conn_fd,
                     const char *status_text,
                     const char *content_type,
                     const char *body,
                     int body_len,
                     int keep_alive);

void http_respond_error(int conn_fd, const char *status_text, const char *message);


//send a response using chunked transfer encoding
void http_chunked_start(int conn_fd, const char *status_text, const char *content_type, int keep_alive);
void http_chunk_send(int conn_fd, const char *data, int len);
void http_chunk_end(int conn_fd);


int read_file(const char *path, char **out_buf);

#endif
