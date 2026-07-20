#ifndef STATIC_H
#define STATIC_H

#include "http.h"

void static_serve(int conn_fd, http_request_t *req, const char *filepath);

const char *static_mime_type(const char *filepath);

#endif