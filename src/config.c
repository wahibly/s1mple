#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "logger.h"
#include "lb.h"
#include "cache.h"

server_config_t g_config;

//default

static void config_set_defaults(void) {
    g_config.port             = 8080;
    g_config.max_connections  = 10;
    g_config.max_request_bytes = 4096;
    g_config.max_apps         = 16;
    g_config.base_port        = 5001;
    g_config.log_level        = LOG_DEBUG;
    g_config.proxy_timeout = 30;
    strncpy(g_config.static_dir, "static",      sizeof(g_config.static_dir) - 1);
    strncpy(g_config.log_file,   "s1mple.log",  sizeof(g_config.log_file)   - 1);
    strncpy(g_config.concurrency, "thread", sizeof(g_config.concurrency) - 1);
    g_config.cache_rule_count = 0;
}

//remove leading and trailing whitespace in place
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;

    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    return s;
}
static log_level_t parse_log_level(const char *val) {
    if (strcasecmp(val, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(val, "info")  == 0) return LOG_INFO;
    if (strcasecmp(val, "warn")  == 0) return LOG_WARN;
    if (strcasecmp(val, "error") == 0) return LOG_ERROR;
    if (strcasecmp(val, "fatal") == 0) return LOG_FATAL;

    fprintf(stderr, "[config] unknown log level '%s', using 'debug'\n", val);
    return LOG_DEBUG;
}


static void config_apply_pair(const char *key, const char *value) {
    if (strcmp(key, "port") == 0) {
        g_config.port = atoi(value);

    } else if (strcmp(key, "static_dir") == 0) {
        strncpy(g_config.static_dir, value,
                sizeof(g_config.static_dir) - 1);

    } else if (strcmp(key, "log_file") == 0) {
        strncpy(g_config.log_file, value,
                sizeof(g_config.log_file) - 1);

    } else if (strcmp(key, "log_level") == 0) {
        g_config.log_level = parse_log_level(value);

    } else if (strcmp(key, "max_connections") == 0) {
        g_config.max_connections = atoi(value);

    } else if (strcmp(key, "max_request_kb") == 0) {
        g_config.max_request_bytes = atoi(value) * 1024;

    } else if (strcmp(key, "max_apps") == 0) {
        g_config.max_apps = atoi(value);

    } else if (strcmp(key, "base_port") == 0) {
        g_config.base_port = atoi(value);

    }else if (strcmp(key, "proxy_timeout") == 0){
        g_config.proxy_timeout= atoi(value);

    } else if (strcmp(key, "concurrency") == 0) {
        strncpy(g_config.concurrency, value, sizeof(g_config.concurrency) - 1);
    
    } else if (strcmp(key, "lb_route") == 0) {
        
        char prefix[64], algo[32];
        if (sscanf(value, "%63s %31s", prefix, algo) == 2) {
            lb_add_route(prefix, algo);
        } else {
            fprintf(stderr, "[config] invalid lb_route: '%s'\n", value);
        }

    } else if (strcmp(key, "lb_backend") == 0) {
        // Format: lb_backend = /prefix host:port
        // e.g.:   lb_backend = /api localhost:5001
        char prefix[64], hostport[128];
        if (sscanf(value, "%63s %127s", prefix, hostport) == 2) {
            char host[64];
            int  port;
            if (sscanf(hostport, "%63[^:]:%d", host, &port) == 2) {
                lb_add_backend(prefix, host, port);
            } else {
                fprintf(stderr,
                        "[config] invalid lb_backend host:port: '%s'\n",
                        hostport);
            }
        } else {
            fprintf(stderr, "[config] invalid lb_backend: '%s'\n", value);
        }
    
    } else if (strcmp(key, "cache_route") == 0) {
        // Format: cache_route = /prefix TTL_seconds
        char   prefix[128];
        int    ttl;
        if (sscanf(value, "%127s %d", prefix, &ttl) == 2) {
            if (g_config.cache_rule_count < MAX_CACHE_RULES) {
                cache_rule_t *r =
                    &g_config.cache_rules[g_config.cache_rule_count++];
                strncpy(r->prefix, prefix, sizeof(r->prefix) - 1);
                r->ttl = ttl;
                fprintf(stderr, "[config] cache_route: %s TTL=%ds\n",
                        prefix, ttl);
            }
        } else {
            fprintf(stderr, "[config] invalid cache_route: '%s'\n", value);
        }
    } else {
        fprintf(stderr, "[config] unknown key '%s' — ignored\n", key);
    }
}


void config_load(const char *filepath) {
    config_set_defaults();

    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr,
                "[config] '%s' not found — using defaults\n", filepath);
        return;
    }

    char line[512];
    int  line_num = 0;

    while (fgets(line, sizeof(line), f)) {
        line_num++;

        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        char *trimmed = trim(line);

        // Skip empty lines
        if (strlen(trimmed) == 0) continue;

        // Split on '='
        char *eq = strchr(trimmed, '=');
        if (!eq) {
            fprintf(stderr,
                    "[config] line %d: no '=' found, skipping: '%s'\n",
                    line_num, trimmed);
            continue;
        }

        // Split into key and value
        *eq = '\0';
        char *key   = trim(trimmed);
        char *value = trim(eq + 1);

        if (strlen(key) == 0 || strlen(value) == 0) {
            fprintf(stderr,
                    "[config] line %d: empty key or value, skipping\n",
                    line_num);
            continue;
        }

        config_apply_pair(key, value);
    }

    fclose(f);
}

void config_print(void) {
    static const char *level_names[] = {
        "debug", "info", "warn", "error", "fatal"
    };

    log_info("config", "port            = %d", g_config.port);
    log_info("config", "static_dir      = %s", g_config.static_dir);
    log_info("config", "log_file        = %s", g_config.log_file);
    log_info("config", "log_level       = %s", level_names[g_config.log_level]);
    log_info("config", "max_connections = %d", g_config.max_connections);
    log_info("config", "max_request_kb  = %d", g_config.max_request_bytes / 1024);
    log_info("config", "max_apps        = %d", g_config.max_apps);
    log_info("config", "base_port       = %d", g_config.base_port);
    log_info("config", "proxy_timeout   = %d", g_config.proxy_timeout);
    log_info("config", "concurrency     = %s", g_config.concurrency);
    log_info("config", "cache_rules     = %d", g_config.cache_rule_count);
}