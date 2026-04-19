#ifndef ROUTER_H
#define ROUTER_H

#include "./http.h"

typedef void (*route_handler_t)(int conn_fd, http_request_t *req);

typedef struct {
    const char     *path;
    route_handler_t handler;
} route_t;

extern route_t routes[];

int router_dispatch(int conn_fd, http_request_t *req, route_t *routes);

#endif