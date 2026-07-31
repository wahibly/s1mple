#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lb.h"
#include "logger.h"
#include <limits.h>


lb_t g_lb;

void lb_init(void) {
    memset(&g_lb, 0, sizeof(g_lb));
    log_info("lb", "load balancer initialized");
}

lb_route_t *lb_add_route(const char *prefix, const char *algo) {
    if (g_lb.route_count >= LB_MAX_ROUTES) {
        log_error("lb", "max routes reached");
        return NULL;
    }

    lb_route_t *r = &g_lb.routes[g_lb.route_count++];
    memset(r, 0, sizeof(lb_route_t));
    strncpy(r->prefix, prefix, sizeof(r->prefix) - 1);

    if (strcmp(algo, "least_conn") == 0) {
        r->algo = LB_ALGO_LC;
    } else {
        r->algo = LB_ALGO_RR; // default
    }

    pthread_mutex_init(&r->lock, NULL);

    log_info("lb", "route '%s' added with algo=%s", prefix, algo);
    return r;
}

int lb_add_backend(const char *prefix, const char *host, int port) {
    lb_route_t *r = lb_find_route(prefix);
    if (!r) {
        log_error("lb", "route '%s' not found", prefix);
        return -1;
    }

    if (r->backend_count >= LB_MAX_BACKENDS) {
        log_error("lb", "max backends reached for route '%s'", prefix);
        return -1;
    }

    lb_backend_t *b = &r->backends[r->backend_count++];
    memset(b, 0, sizeof(lb_backend_t));
    strncpy(b->host, host, sizeof(b->host) - 1);
    b->port    = port;
    b->healthy = 1;

    log_info("lb", "backend %s:%d added to route '%s'",
             host, port, prefix);
    return 0;
}

lb_route_t *lb_find_route(const char *path) {
    for (int i = 0; i < g_lb.route_count; i++) {
        if (strncmp(path, g_lb.routes[i].prefix,
                    strlen(g_lb.routes[i].prefix)) == 0) {
            return &g_lb.routes[i];
        }
    }
    return NULL;
}

static lb_backend_t *pick_round_robin(lb_route_t *r) {
    int start = r->rr_index;
    int n     = r->backend_count;

    for (int i = 0; i < n; i++) {
        int idx = (start + i) % n;
        if (r->backends[idx].healthy) {
            r->rr_index = (idx + 1) % n;  
            return &r->backends[idx];
        }
    }
    return NULL;  
}

static lb_backend_t *pick_least_conn(lb_route_t *r) {
    lb_backend_t *best     = NULL;
    int           best_cnt = INT_MAX;

    for (int i = 0; i < r->backend_count; i++) {
        lb_backend_t *b = &r->backends[i];
        if (!b->healthy) continue;
        if (b->active_conns < best_cnt) {
            best     = b;
            best_cnt = b->active_conns;
        }
    }
    return best;
}

lb_backend_t *lb_pick(const char *path) {
    lb_route_t *r = lb_find_route(path);
    if (!r) return NULL;

    pthread_mutex_lock(&r->lock);

    lb_backend_t *b = (r->algo == LB_ALGO_LC)
                      ? pick_least_conn(r)
                      : pick_round_robin(r);

    if (b) {
        b->active_conns++;
        b->total_requests++;
        log_debug("lb", "picked %s:%d for '%s' (conns=%d)",
                  b->host, b->port, path, b->active_conns);
    } else {
        log_warn("lb", "all backends down for route '%s'", path);
    }

    pthread_mutex_unlock(&r->lock);
    return b;
}


void lb_release(lb_backend_t *backend) {
    if (!backend) return;
    if (backend->active_conns > 0) backend->active_conns--;
}


void lb_set_health(lb_backend_t *backend, int healthy) {
    if (!backend) return;
    if (backend->healthy != healthy) {
        log_warn("lb", "backend %s:%d marked %s",
                 backend->host, backend->port,
                 healthy ? "healthy" : "unhealthy");
        backend->healthy    = healthy;
        backend->last_check = time(NULL);
    }
}

void lb_print_stats(char *buf, int buf_size) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos,
                    "{\n  \"routes\": [\n");

    for (int i = 0; i < g_lb.route_count; i++) {
        lb_route_t *r = &g_lb.routes[i];
        const char *algo = (r->algo == LB_ALGO_LC)
                           ? "least_conn" : "round_robin";

        pos += snprintf(buf + pos, buf_size - pos,
            "    {\n"
            "      \"prefix\": \"%s\",\n"
            "      \"algo\": \"%s\",\n"
            "      \"backends\": [\n",
            r->prefix, algo);

        for (int j = 0; j < r->backend_count; j++) {
            lb_backend_t *b = &r->backends[j];
            pos += snprintf(buf + pos, buf_size - pos,
                "        {"
                "\"host\": \"%s\", "
                "\"port\": %d, "
                "\"healthy\": %s, "
                "\"active_conns\": %d, "
                "\"total_requests\": %d"
                "}%s\n",
                b->host, b->port,
                b->healthy ? "true" : "false",
                b->active_conns,
                b->total_requests,
                j < r->backend_count - 1 ? "," : "");
        }

        pos += snprintf(buf + pos, buf_size - pos,
            "      ]\n"
            "    }%s\n",
            i < g_lb.route_count - 1 ? "," : "");
    }

    pos += snprintf(buf + pos, buf_size - pos,
                    "  ]\n}");
}