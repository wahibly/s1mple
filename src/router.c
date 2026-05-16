#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "router.h"
#include "http.h"
#include "server_tcp.h"
#include "process.h"
#include "config.h"
#include "registry.h"
#include <sys/stat.h>
#include <unistd.h>
#include "logger.h"


static void handler_home(int conn_fd, http_request_t *req) {
    (void)req;
    char filepath[256];
    snprintf(filepath, sizeof(filepath),
             "%s/index.html", g_config.static_dir); 

    char *body     = NULL;
    int   body_len = read_file(filepath, &body);

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

static void handler_run_python(int conn_fd, http_request_t *req) {
    (void)req;
    char *const argv[] = {"python3", "apps/messi.py", NULL};
    char *output = NULL;

    int len = process_exec_capture(argv, &output);
    if (len < 0){
        http_respond_error(conn_fd, "500 Internal Server Error",
                           "Failed to execute script.");
        return;   
    }

    http_respond(conn_fd, "200 OK", "text/html", output, len);
    free(output);
}

static void handler_run_node(int conn_fd, http_request_t *req) {
    (void)req;
    char *const argv[] = {"node", "apps/messi.js", NULL};
    char *output = NULL;

    int len = process_exec_capture(argv, &output);
    if (len < 0){
        http_respond_error(conn_fd, "500 Internal Server Error",
                           "Failed to execute script.");
        return;   
    }

    http_respond(conn_fd, "200 OK", "text/html", output, len);
    free(output);
}


//GET /deploy?app=messi_serevr.py
static void handler_deploy(int conn_fd, http_request_t *req) {

    const char *query = strchr(req->path, '?');
    if (!query) {
        http_respond_error(conn_fd, "400 Bad Request","Missing query param: ?app=filename");
        return;
    }

    char app_name[MAX_APP_NAME];
    if (sscanf(query, "?app=%63s", app_name) != 1) {
        http_respond_error(conn_fd, "400 Bad Request", "Invalid app parameter.");
        return;
    }


    //build filepath
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "apps/%s", app_name);

    if (access(filepath, F_OK) != 0) {
        http_respond_error(conn_fd, "404 Not Found",
                           "App file not found in apps/ directory.");
        return;
    }

    app_entry_t *entry = registry_deploy(app_name, filepath);
    if (!entry) {
        http_respond_error(conn_fd, "500 Internal Server Error",
                           "Deployment failed. Check server logs.");
        return;      
    }

    usleep(500000);

    char body[512];
    int  body_len = snprintf(body, sizeof(body),
        "{\n"
        "  \"status\": \"deployed\",\n"
        "  \"name\": \"%s\",\n"
        "  \"prefix\": \"%s\",\n"
        "  \"port\": %d,\n"
        "  \"pid\": %d\n"
        "}",
        entry->name, entry->prefix, entry->port, entry->pid);

    http_respond(conn_fd, "200 OK", "application/json", body, body_len);
    log_info("router", "deploy request for '%s'", app_name);


}

static void handler_undeploy(int conn_fd, http_request_t *req) {
    const char *query = strchr(req->path, '?');
    if (!query) {
        http_respond_error(conn_fd, "400 Bad Request",
                           "Missing query param: ?app=filename");
        return;
    }

    char app_name[MAX_APP_NAME];
    if (sscanf(query, "?app=%63s", app_name) != 1) {
        http_respond_error(conn_fd, "400 Bad Request",
                           "Invalid app parameter.");
        return;
    }

    if (registry_undeploy(app_name) < 0) {
        http_respond_error(conn_fd, "404 Not Found", "App not found.");
        return;
    }

    const char *body = "{\"status\": \"undeployed\"}";
    http_respond(conn_fd, "200 OK", "application/json",body, (int)strlen(body));
    log_info("router", "undeploy request for '%s'", app_name);
}


static void handler_api_apps(int conn_fd, http_request_t *req) {
    (void)req;

    app_entry_t apps[MAX_APPS];
    int count = 0;
    registry_list(apps, &count);

    char body[4096];
    int  pos = 0;
    pos += snprintf(body + pos, sizeof(body) - pos, "{\n  \"apps\": [\n");

    for (int i = 0; i < count; i++) {
        pos += snprintf(body + pos, sizeof(body) - pos,
            "    {\"name\": \"%s\", \"prefix\": \"%s\","
            " \"port\": %d, \"pid\": %d}%s\n",
            apps[i].name, apps[i].prefix,
            apps[i].port, apps[i].pid,
            i < count - 1 ? "," : "");
    }

    pos += snprintf(body + pos, sizeof(body) - pos,
                    "  ],\n  \"count\": %d\n}", count);

    http_respond(conn_fd, "200 OK", "application/json", body, pos);   
}




route_t routes[] = {
    { "/", handler_home},
    { "/api/status", handler_api_status},
    {"/api/apps", handler_api_apps},
    { "/about", handler_about},
    { "/run/messi.py", handler_run_python},
    { "/run/messi.js", handler_run_node},
    {"/deploy", handler_deploy},
    {"/undeploy", handler_undeploy},

    { NULL, NULL}
};

int router_dispatch(int conn_fd, http_request_t *req, route_t *table) {

    char path_only[MAX_PATH_LEN];
    strncpy(path_only, req->path, MAX_PATH_LEN - 1);
    char *q = strchr(path_only, '?');
    if (q) *q = '\0'; // null-terminate at the '?'

    for (int i = 0; table[i].path != NULL; i++) {
        if (strcmp(path_only, table[i].path) == 0) {
            log_debug("router", "matched '%s'", table[i].path);
            table[i].handler(conn_fd, req);
            return 1;
        }
    }
    return 0;
}