#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "logger.h"

static struct {
    FILE *file;
    log_level_t min_level; //messages below this are dropped
    pthread_mutex_t lock;
    int initialized;
} g_logger = {NULL, LOG_DEBUG, PTHREAD_MUTEX_INITIALIZER, 0};

static const char *level_names[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

static const char *level_colors[] = {
    "\033[36m",   // DEBUG — cyan
    "\033[32m",   // INFO  — green
    "\033[33m",   // WARN  — yellow
    "\033[31m",   // ERROR — red
    "\033[35m",   // FATAL — magenta
};
static const char *color_reset = "\033[0m";

void logger_init(const char *log_file_path, log_level_t min_level){
    pthread_mutex_lock(&g_logger.lock);

    g_logger.min_level = min_level;
    g_logger.initialized =1;

    if(log_file_path) {
        g_logger.file = fopen(log_file_path, "a");
        if(!g_logger.file) {
            fprintf(stderr, "[logger] warning: could not open log file '%s'\n", log_file_path);
        }
    }

    pthread_mutex_unlock(&g_logger.lock);
    
}

void logger_close(void){
    pthread_mutex_lock(&g_logger.lock);

    if(g_logger.file) {
        fclose(g_logger.file);
        g_logger.file = NULL;
    }

    pthread_mutex_unlock(&g_logger.lock); 
    
}

void log_write(log_level_t level, const char *component, const char *fmt, ...){

    if(level < g_logger.min_level)
        return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_logger.lock); 
    fprintf(stdout, "[%s] %s%s%s [%-8s] %s\n",
            timestamp,
            level_colors[level],
            level_names[level],
            color_reset,
            component,
            message);
    fflush(stdout);
    
    //logs without colors
    if (g_logger.file) {
        fprintf(g_logger.file, "[%s] [%s] [%-8s] %s\n",
                timestamp,
                level_names[level],
                component,
                message);
        fflush(g_logger.file); 
    }

    pthread_mutex_unlock(&g_logger.lock); 

    if (level == LOG_FATAL) {
        logger_close();
        exit(EXIT_FAILURE);
    }
}


