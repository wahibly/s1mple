#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} log_level_t;

void logger_init(const char *log_file_path, log_level_t min_level);

void logger_close(void);


void log_write(log_level_t level, const char *component, const char *fmt, ...)

__attribute__((format(printf, 3, 4)));  

#define log_debug(comp, ...) log_write(LOG_DEBUG, comp, __VA_ARGS__)
#define log_info(comp, ...)  log_write(LOG_INFO,  comp, __VA_ARGS__)
#define log_warn(comp, ...)  log_write(LOG_WARN,  comp, __VA_ARGS__)
#define log_error(comp, ...) log_write(LOG_ERROR, comp, __VA_ARGS__)
#define log_fatal(comp, ...) log_write(LOG_FATAL, comp, __VA_ARGS__)

#endif