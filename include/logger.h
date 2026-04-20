#pragma once

#include <stddef.h>

int logger_init(const char *error_log_path, const char *access_log_path);
void logger_close(void);

void log_error(const char *level, const char *fmt, ...);
void log_access(
    const char *client_ip,
    const char *method,
    const char *uri,
    const char *version,
    int status,
    size_t bytes
);
