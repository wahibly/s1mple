#ifndef PROXY_H
#define PROXY_H

#include "http.h"


int proxy_forward(int conn_fd,
                  http_request_t *req,
                  int backend_port,
                  const char *client_ip);
                  
int proxy_forward_lb(int conn_fd,
                     http_request_t *req,
                     const char *client_ip);

int proxy_forward_cached(int conn_fd,
                          http_request_t *req,
                          int backend_port,
                          const char *client_ip);
#endif