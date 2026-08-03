// src/cache.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cache.h"
#include "config.h"
#include "logger.h"

cache_t g_cache;


void cache_init(void) {
    memset(&g_cache, 0, sizeof(g_cache));
    pthread_mutex_init(&g_cache.lock, NULL);
    log_info("cache", "initialized — max %d entries, max %dKB per entry", CACHE_MAX_ENTRIES, CACHE_MAX_VAL_SIZE / 1024);
}

char *cache_get(const char *key, int *out_len, int *out_age) {
    pthread_mutex_lock(&g_cache.lock);

    time_t now = time(NULL);

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        cache_entry_t *e = &g_cache.entries[i];
        if (!e->active) continue;
        if (strcmp(e->key, key) != 0) continue;

        // Check TTL
        int age = (int)(now - e->created_at);
        if (age > e->ttl) {

            log_debug("cache", "expired: key='%s' age=%ds ttl=%ds",
                      key, age, e->ttl);
            free(e->value);
            memset(e, 0, sizeof(cache_entry_t));
            g_cache.count--;
            pthread_mutex_unlock(&g_cache.lock);
            return NULL;
        }

        char *copy = malloc(e->value_len);
        if (!copy) {
            pthread_mutex_unlock(&g_cache.lock);
            return NULL;
        }
        memcpy(copy, e->value, e->value_len);
        *out_len = e->value_len;
        *out_age = age;
        e->hits++;

        log_debug("cache", "HIT key='%s' age=%ds hits=%d",
                  key, age, e->hits);

        pthread_mutex_unlock(&g_cache.lock);
        return copy;
    }

    pthread_mutex_unlock(&g_cache.lock);
    return NULL;

}
void cache_set(const char *key, const char *value, int value_len, int ttl) {
    if (value_len > CACHE_MAX_VAL_SIZE) {
        log_warn("cache", "response too large to cache: %d bytes (max %d)", value_len, CACHE_MAX_VAL_SIZE);
        return;
    }

    pthread_mutex_lock(&g_cache.lock);

    // Find existing entry for this key (update) or a free slot (insert)
    int slot = -1;

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        cache_entry_t *e = &g_cache.entries[i];

        if (e->active && strcmp(e->key, key) == 0) {
            free(e->value);
            slot = i;
            break;
        }

        if (!e->active && slot < 0) {
            slot = i;  
        }
    }

        if (slot < 0) {
        // Cache full — evict the oldest entry
        time_t oldest_time = time(NULL);
        int    oldest_idx  = 0;

        for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
            if (g_cache.entries[i].created_at < oldest_time) {
                oldest_time = g_cache.entries[i].created_at;
                oldest_idx  = i;
            }
        }

        log_warn("cache", "cache full — evicting oldest entry '%s'",
                 g_cache.entries[oldest_idx].key);
        free(g_cache.entries[oldest_idx].value);
        memset(&g_cache.entries[oldest_idx], 0, sizeof(cache_entry_t));
        g_cache.count--;
        slot = oldest_idx;
    }

    // Store the entry
    cache_entry_t *e = &g_cache.entries[slot];
    e->value = malloc(value_len);
    if (!e->value) {
        pthread_mutex_unlock(&g_cache.lock);
        return;
    }

    memcpy(e->value, value, value_len);
    strncpy(e->key, key, CACHE_MAX_KEY_LEN - 1);
    e->value_len  = value_len;
    e->created_at = time(NULL);
    e->ttl        = ttl;
    e->hits       = 0;
    e->active     = 1;

    if (slot >= g_cache.count) g_cache.count++;

    log_debug("cache", "STORE key='%s' size=%d ttl=%ds",
              key, value_len, ttl);

    pthread_mutex_unlock(&g_cache.lock);
}

int cache_purge(const char *key) {
    pthread_mutex_lock(&g_cache.lock);

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        cache_entry_t *e = &g_cache.entries[i];
        if (e->active && strcmp(e->key, key) == 0) {
            free(e->value);
            memset(e, 0, sizeof(cache_entry_t));
            g_cache.count--;
            log_info("cache", "purged key='%s'", key);
            pthread_mutex_unlock(&g_cache.lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&g_cache.lock);
    return -1;
}


void cache_evict_expired(void) {
    pthread_mutex_lock(&g_cache.lock);

    time_t now     = time(NULL);
    int    evicted = 0;

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        cache_entry_t *e = &g_cache.entries[i];
        if (!e->active) continue;

        int age = (int)(now - e->created_at);
        if (age > e->ttl) {
            free(e->value);
            memset(e, 0, sizeof(cache_entry_t));
            g_cache.count--;
            evicted++;
        }
    }

    if (evicted > 0) {
        log_debug("cache", "evicted %d expired entries", evicted);
    }

    pthread_mutex_unlock(&g_cache.lock);
}

void cache_clear(void) {
    pthread_mutex_lock(&g_cache.lock);

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (g_cache.entries[i].active) {
            free(g_cache.entries[i].value);
            memset(&g_cache.entries[i], 0, sizeof(cache_entry_t));
        }
    }
    g_cache.count = 0;
    log_info("cache", "cleared all entries");

    pthread_mutex_unlock(&g_cache.lock);
}

int cache_get_ttl(const char *path) {
    for (int i = 0; i < g_config.cache_rule_count; i++) {
        if (strncmp(path,g_config.cache_rules[i].prefix, strlen(g_config.cache_rules[i].prefix)) == 0) {
            return g_config.cache_rules[i].ttl;
        }
    }
    return 0;
}

void cache_stats(char *buf, int buf_size) {
    pthread_mutex_lock(&g_cache.lock);

    time_t now = time(NULL);
    int    pos = 0;

    pos += snprintf(buf + pos, buf_size - pos,
        "{\n"
        "  \"count\": %d,\n"
        "  \"max\": %d,\n"
        "  \"entries\": [\n",
        g_cache.count, CACHE_MAX_ENTRIES);

    int first = 1;
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        cache_entry_t *e = &g_cache.entries[i];
        if (!e->active) continue;

        int age        = (int)(now - e->created_at);
        int remaining  = e->ttl - age;

        pos += snprintf(buf + pos, buf_size - pos,
            "%s    {"
            "\"key\": \"%s\", "
            "\"size\": %d, "
            "\"age\": %d, "
            "\"ttl\": %d, "
            "\"remaining\": %d, "
            "\"hits\": %d"
            "}",
            first ? "" : ",\n",
            e->key, e->value_len,
            age, e->ttl, remaining > 0 ? remaining : 0,
            e->hits);
        first = 0;
    }

    pos += snprintf(buf + pos, buf_size - pos, "\n  ]\n}");

    pthread_mutex_unlock(&g_cache.lock);
}