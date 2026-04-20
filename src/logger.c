#include "logger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *g_error_log;
static FILE *g_access_log;

static void format_time_error(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, size, "%a %b %d %H:%M:%S %Y", &tm_now);
}

static void format_time_access(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, size, "%d/%b/%Y:%H:%M:%S %z", &tm_now);
}

int logger_init(const char *error_log_path, const char *access_log_path)
{
    g_error_log = fopen(error_log_path, "a");
    g_access_log = fopen(access_log_path, "a");

    if (g_error_log == NULL || g_access_log == NULL) {
        if (g_error_log != NULL) {
            fclose(g_error_log);
            g_error_log = NULL;
        }
        if (g_access_log != NULL) {
            fclose(g_access_log);
            g_access_log = NULL;
        }
        return -1;
    }

    setvbuf(g_error_log, NULL, _IOLBF, 0);
    setvbuf(g_access_log, NULL, _IOLBF, 0);
    return 0;
}

void logger_close(void)
{
    if (g_error_log != NULL) {
        fclose(g_error_log);
        g_error_log = NULL;
    }
    if (g_access_log != NULL) {
        fclose(g_access_log);
        g_access_log = NULL;
    }
}

void log_error(const char *level, const char *fmt, ...)
{
    FILE *out = (g_error_log != NULL) ? g_error_log : stderr;
    char time_buf[64];
    format_time_error(time_buf, sizeof(time_buf));

    fprintf(out, "[%s] [%s] ", time_buf, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    if (fmt[0] == '\0' || fmt[strlen(fmt) - 1] != '\n') {
        fputc('\n', out);
    }

    if (out != stderr) {
        fflush(out);
    }
}

void log_access(
    const char *client_ip,
    const char *method,
    const char *uri,
    const char *version,
    int status,
    size_t bytes
)
{
    FILE *out = (g_access_log != NULL) ? g_access_log : stdout;
    char time_buf[64];
    format_time_access(time_buf, sizeof(time_buf));

    fprintf(
        out,
        "%s - - [%s] \"%s %s %s\" %d %zu\n",
        (client_ip != NULL) ? client_ip : "-",
        time_buf,
        (method != NULL) ? method : "-",
        (uri != NULL) ? uri : "-",
        (version != NULL) ? version : "-",
        status,
        bytes
    );

    if (out != stdout) {
        fflush(out);
    }
}
