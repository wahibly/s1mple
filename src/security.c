#include <stdio.h>
#include <stdlib.h>
#include <string.h>   
#include <pthread.h>
#include "security.h"
#include "server_tcp.h"
#include "logger.h"
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int  count;
} conn_entry_t;

static conn_entry_t  conn_table[MAX_CONN_TRACKED];
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

int security_conn_allow(const char *ip) {
    pthread_mutex_lock(&conn_lock);

    int  found    = -1;   // index of existing entry for this IP
    int  empty    = -1;   // index of first empty slot

    for (int i = 0; i < MAX_CONN_TRACKED; i++) {
        if (conn_table[i].count > 0 && strcmp(conn_table[i].ip, ip) == 0) {
            found = i;
            break;
        }
        if (empty < 0 && conn_table[i].count == 0) {
            empty = i;
        }
    }

    int allowed = 1;

    if (found >= 0) {
        // IP in table
        if (conn_table[found].count >= MAX_CONN_PER_IP) {
            log_warn("security", "connection limit hit for IP %s (%d/%d)",
                     ip, conn_table[found].count, MAX_CONN_PER_IP);
            allowed = 0;
        } else {
            conn_table[found].count++;
            log_debug("security", "IP %s connections: %d",
                      ip, conn_table[found].count);
        }
    } else if (empty >= 0) {
        // new IP —> use empty slot
        strncpy(conn_table[empty].ip, ip, INET_ADDRSTRLEN - 1);
        conn_table[empty].count = 1;
        log_debug("security", "new IP %s tracked", ip);
    } else {
        log_warn("security", "connection table full, allowing %s", ip);
    }

    pthread_mutex_unlock(&conn_lock);
    return allowed;
}


void security_conn_release(const char *ip) {
    pthread_mutex_lock(&conn_lock);

    for (int i = 0; i < MAX_CONN_TRACKED; i++) {
        if (conn_table[i].count > 0 && strcmp(conn_table[i].ip, ip) == 0) {
            conn_table[i].count--;
            if (conn_table[i].count == 0) {
                memset(&conn_table[i], 0, sizeof(conn_entry_t));
            }
            break;
        }
    }

    pthread_mutex_unlock(&conn_lock);
}


int security_path_is_safe(const char *filepath, const char *allowed_dir) {
    // Resolve allowed_dir to absolute path 
    char real_allowed[PATH_MAX];
    if (!realpath(allowed_dir, real_allowed)) {
        log_error("security", "could not resolve allowed_dir '%s'", allowed_dir);
        return 0;
    }


    if (strstr(filepath, "..") != NULL) {
        log_warn("security",
                 "path traversal attempt blocked: '%s'", filepath);
        return 0;
    }

    // ── Second line of defense: resolve and verify prefix ─────────────
    // Only reached if no '..' in path. Still check resolved path
    // to catch symlink-based escapes.
    char real_path[PATH_MAX];
    if (!realpath(filepath, real_path)) {
        // File doesn't exist and no '..' — safe, caller gets 404
        return 1;
    }



    char allowed_with_slash[PATH_MAX];
    snprintf(allowed_with_slash, sizeof(allowed_with_slash),
             "%s/", real_allowed);


    if (strncmp(real_path, allowed_with_slash,
                strlen(allowed_with_slash)) != 0 &&
        strcmp(real_path, real_allowed) != 0) {
        log_warn("security",
                 "path traversal attempt blocked: '%s' → '%s'",
                 filepath, real_path);
        return 0;
    }

    return 1;
}

int security_request_size_ok(int size) {
    if (size > MAX_REQUEST_SIZE) {
        log_warn("security",
                 "request too large: %d bytes (max %d) — rejecting",
                 size, MAX_REQUEST_SIZE);
        return 0;
    }
    return 1;
}