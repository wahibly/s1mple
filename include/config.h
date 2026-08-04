#ifndef CONFIG_H
#define CONFIG_H
#define MAX_CACHE_RULES 16

#include "logger.h"

typedef struct {
    char prefix[128];
    int  ttl;          
} cache_rule_t;

typedef struct {
    int  port;

    char static_dir[256];

    char log_file[256];
    log_level_t log_level;

    int  max_connections;   
    int  max_request_bytes;  

    int  max_apps;
    int  base_port;
    int proxy_timeout;

    char concurrency[16];
    
    cache_rule_t cache_rules[MAX_CACHE_RULES];
    int cache_rule_count;
} server_config_t;

// Load config from file. Missing keys use defaults.
void config_load(const char *filepath);

void config_print(void);

extern server_config_t g_config;



#endif

