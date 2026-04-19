#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/router.h"
#include "../include/http.h"
#include "../include/server_tcp.h"

static void handler_home(int conn_fd, http_request_t *req) {
    (void)req;

    char *body    = NULL;
    int   body_len = read_file(STATIC_DIR "/index.html", &body);

    if (body_len < 0) {
        http_respond_error(conn_fd, "500 Internal Server Error",
                           "Could not read index.html.");
        return;
    }

    http_respond(conn_fd, "200 OK", "text/html", body, body_len);
    free(body);
}

static void handler_api_status(int conn_fd, http_request_t *req) {
    (void)req;
    const char *body =
        "{\n"
        "  \"status\": \"ok\",\n"
        "  \"server\": \"s1mple\",\n"
        "  \"version\": \"0.1.0\"\n"
        "}";
    http_respond(conn_fd, "200 OK", "application/json",
                 body, (int)strlen(body));
}

static void handler_about(int conn_fd, http_request_t *req) {
    (void)req;
    const char *body =
        "<html><body>"
        "<h1>About s1mple</h1>"
        "<p>A minimal HTTP server written in C.</p>"
        "</body></html>";
    http_respond(conn_fd, "200 OK", "text/html",
                 body, (int)strlen(body));
}

route_t routes[] = {
    { "/", handler_home},
    { "/api/status", handler_api_status},
    { "/about", handler_about},
    { NULL, NULL}
};

int router_dispatch(int conn_fd, http_request_t *req, route_t *table) {

    for (int i = 0; table[i].path != NULL; i++) {

        if (strcmp(req->path, table[i].path) == 0) {
            table[i].handler(conn_fd, req);
            return 1;
        }
    }
    return 0;
}