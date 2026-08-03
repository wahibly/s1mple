#ifndef LB_H
#define LB_H

#include <pthread.h>
#include <time.h>

#define LB_MAX_ROUTES    16
#define LB_MAX_BACKENDS  16
#define LB_ALGO_RR       0    // round robin
#define LB_ALGO_LC       1    // least connections

// one backend server in the pool
typedef struct {
    char  host[64];
    int   port;
    int   healthy;        
    int   active_conns;     
    int   total_requests;    
    time_t last_check;       // last health check timestamp
} lb_backend_t;

typedef struct {
    char          prefix[64];
    int           algo;
    lb_backend_t  backends[LB_MAX_BACKENDS];
    int           backend_count;
    int           rr_index;          // current round robin position
    pthread_mutex_t lock;
} lb_route_t;

// The load balancer
typedef struct {
    lb_route_t routes[LB_MAX_ROUTES];
    int        route_count;
} lb_t;

extern lb_t g_lb;

void lb_init(void);

lb_route_t *lb_add_route(const char *prefix, const char *algo);

int lb_add_backend(const char *prefix, const char *host, int port);

// Pick the best backend for this request
// Returns the backend to use, or NULL if all backends are down
lb_backend_t *lb_pick(const char *path);

void lb_release(lb_backend_t *backend);

void lb_set_health(lb_backend_t *backend, int healthy);

// find which route owns this path prefix
lb_route_t *lb_find_route(const char *path);

void lb_print_stats(char *buf, int buf_size);

#endif