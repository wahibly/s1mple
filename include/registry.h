#ifndef REGISTRY_H
#define REGISTRY_H

#include <sys/types.h>
#include <pthread.h>

#define MAX_APPS 16
#define BASE_PORT 5001
#define MAX_APP_NAME 64
#define MAX_PREFIX_LEN 64

 typedef struct {
    int  active;                  // 1 =slot is in use 0 = free
    char name[MAX_APP_NAME];     
    char prefix[MAX_PREFIX_LEN]; 
    char filepath[256];        
    int  port;        
    pid_t pid;  
} app_entry_t;


//shared across all threads
typedef struct {
    app_entry_t apps[MAX_APPS];
    int count;            // number of active apps
    int next_port;  
    pthread_mutex_t lock;         // semaphore to protect concurrent access yo registry
} app_registry_t;

void registry_init(void);


app_entry_t *registry_deploy(const char *name, const char *filepath);
app_entry_t *registry_find_by_prefix(const char *prefix);
void registry_list(app_entry_t *out, int *count); //list all active apps
int registry_undeploy(const char *name);

extern app_registry_t g_registry; //global registry

#endif