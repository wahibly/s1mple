#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "registry.h"
#include "process.h"
#include "logger.h"
#include "config.h"


app_registry_t g_registry;

void registry_init(void) {
    memset(&g_registry, 0 ,sizeof(g_registry));
    g_registry.next_port = g_config.base_port;
    g_registry.next_id = 1;

    pthread_mutex_init(&g_registry.lock, NULL);
    log_info("registry", "initialized. base port: %d", g_config.base_port);
}

// exec app in backgournd -> return pid of process or -1 if failed
static pid_t spawn_app_process(const char *filepath, int port){
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0){
        //convert port to string 
        char port_str[16];
        snprintf(port_str, sizeof(port_str),"%d", port);

        const char *ext = strrchr(filepath, '.');
        if(ext && strcmp(ext, ".py")==0) {

            char port_env[32];
            snprintf(port_env, sizeof(port_env), "PORT=%d", port);
            putenv(port_env); //make var env

            char *const argv[] = { "python3", (char *)filepath, NULL};
            execvp("python3", argv);

        }else if (ext && strcmp(ext, ".js") == 0) {
            char port_env[32];
            snprintf(port_env, sizeof(port_env), "PORT=%d", port);
            putenv(port_env);

            char *const argv[] = { "node", (char *)filepath, NULL};
            execvp("node", argv);
        }else {
            //try to excute directly
            char *const argv[] = {(char *)filepath, NULL};
            execvp(filepath, argv);
            
        }
        log_error("registry", "exec failed for %s", filepath);
        _exit(1);
        
    }

    //parent
    log_info("registry", "spawned '%s' as pid %d on port %d", filepath, pid, port);

    return pid;
}


app_entry_t *registry_deploy(const char *name, const char *filepath) {
    pthread_mutex_lock(&g_registry.lock);

    if (g_registry.count >= g_config.max_apps) {
        pthread_mutex_unlock(&g_registry.lock);
        log_error("registry", "max apps reached (%d) — deploy rejected for '%s'",
          g_config.max_apps, name);
        return NULL;
        
    }

    for(int i =0; i < g_config.max_apps; i++) {
        if (g_registry.apps[i].active && strcmp(g_registry.apps[i].name, name) == 0) {
            pthread_mutex_unlock(&g_registry.lock);
            log_warn("registry", "app '%s' already deployed", name);
            return NULL;
        }        

    }

    //find a free slot
    int slot = -1;
    for (int i=0; i < g_config.max_apps; i++){
        if(!g_registry.apps[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&g_registry.lock);
        return NULL;
    }

    int port = g_registry.next_port++;

    //build the url
    char prefix[MAX_PREFIX_LEN];
    snprintf(prefix, sizeof(prefix), "/app%d", g_registry.next_id++);

    pid_t pid = spawn_app_process(filepath, port);
    if(pid < 0) {
        pthread_mutex_unlock(&g_registry.lock);
        return NULL;
    }

    //save entry

    app_entry_t *entry = &g_registry.apps[slot];
    entry->active =1;
    strncpy(entry->name,     name,     MAX_APP_NAME - 1);
    strncpy(entry->prefix,   prefix,   MAX_PREFIX_LEN - 1);
    strncpy(entry->filepath, filepath, 255);
    entry->port = port;
    entry->pid = pid;
    g_registry.count++;


    pthread_mutex_unlock(&g_registry.lock);

    log_info("registry", "deployed '%s' → prefix=%s port=%d", name, prefix, port);   

    return entry;
}


app_entry_t *registry_find_by_prefix(const char *prefix) {
    pthread_mutex_lock(&g_registry.lock);

    app_entry_t *found = NULL;
    for(int i=0; i < g_config.max_apps; i++) {
        if (g_registry.apps[i].active && 
            strncmp(prefix, g_registry.apps[i].prefix, strlen(g_registry.apps[i].prefix))==0) {
                found = &g_registry.apps[i];
                break;
            }
 
        }
    pthread_mutex_unlock(&g_registry.lock);
    return found;
}

void registry_list(app_entry_t *out, int *count){
    pthread_mutex_lock(&g_registry.lock);

    *count = 0;
    for (int i = 0; i < g_config.max_apps; i++) {
        if (g_registry.apps[i].active) {
            out[*count] = g_registry.apps[i];
            (*count)++;
        }
    }
    pthread_mutex_unlock(&g_registry.lock);

}

int registry_undeploy(const char *name){
    pthread_mutex_lock(&g_registry.lock);

    for (int i=0; i < g_config.max_apps; i++) {
        if(g_registry.apps[i].active &&
           strcmp(g_registry.apps[i].name, name) == 0) {
                
           kill(g_registry.apps[i].pid, SIGTERM);
           
           sleep(1);

           waitpid(g_registry.apps[i].pid, NULL, WNOHANG);

           log_info("registry", "undeployed '%s' (pid %d)", name, g_registry.apps[i].pid);

           memset(&g_registry.apps[i], 0, sizeof(app_entry_t));
           g_registry.count--;

           pthread_mutex_unlock(&g_registry.lock);
           return 0;
        }

    }

    pthread_mutex_unlock(&g_registry.lock);
    return -1; //not found
}