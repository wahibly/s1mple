// include/cache.h
#ifndef CACHE_H
#define CACHE_H

#include <time.h>
#include <pthread.h>

#define CACHE_MAX_ENTRIES   256
#define CACHE_MAX_KEY_LEN   512
#define CACHE_MAX_VAL_SIZE  (1024 * 1024)  

// one cache entry
typedef struct {
    int    active;
    char   key[CACHE_MAX_KEY_LEN];    // the request path
    char  *value;                      
    int    value_len;
    time_t created_at;                 
    int    ttl;                        
    int    hits;                       // how many times served from cache
} cache_entry_t;

//cache store
typedef struct {
    cache_entry_t   entries[CACHE_MAX_ENTRIES];
    int             count;
    pthread_mutex_t lock;
} cache_t;

extern cache_t g_cache;


void cache_init(void);


char *cache_get(const char *key, int *out_len, int *out_age);

void cache_set(const char *key, const char *value, int value_len, int ttl);


int cache_purge(const char *key);


void cache_evict_expired(void);

void cache_clear(void);


int cache_get_ttl(const char *path);


void cache_stats(char *buf, int buf_size);

#endif