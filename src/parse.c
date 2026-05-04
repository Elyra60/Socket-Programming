#include "parse.h"

#include <ctype.h>

static char *trim_ows(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t') {
        s++;
    }

    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return s;
}

static int copy_token(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len == 0 || len >= dst_size) {
        return -1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int append_header(Request *request, const char *name, size_t name_len, const char *value)
{
    Request_header *new_headers;

    if (name_len == 0 || name_len >= sizeof(request->headers[0].header_name) ||
        strlen(value) >= sizeof(request->headers[0].header_value)) {
        return -1;
    }

    new_headers = (Request_header *)realloc(
        request->headers,
        sizeof(Request_header) * (size_t)(request->header_count + 1));
    if (new_headers == NULL) {
        return -1;
    }
    request->headers = new_headers;

    memcpy(request->headers[request->header_count].header_name, name, name_len);
    request->headers[request->header_count].header_name[name_len] = '\0';
    strcpy(request->headers[request->header_count].header_value, value);
    request->header_count++;
    return 0;
}

Request *parse(const char *buffer, const int size, int socketFd)
{
    Request *request;
    char *work;
    char *line;
    char *next;
    char *sp1;
    char *sp2;
    char *colon;
    (void)socketFd;

    if (buffer == NULL || size <= 0 || size > 8192) {
        return NULL;
    }

    work = (char *)malloc((size_t)size + 1);
    if (work == NULL) {
        return NULL;
    }
    memcpy(work, buffer, (size_t)size);
    work[size] = '\0';

    request = (Request *)calloc(1, sizeof(Request));
    if (request == NULL) {
        free(work);
        return NULL;
    }

    line = work;
    next = strstr(line, "\r\n");
    if (next == NULL) {
        free_request:
        free(request->headers);
        free(request);
        free(work);
        return NULL;
    }
    *next = '\0';

    sp1 = strchr(line, ' ');
    if (sp1 == NULL) {
        goto free_request;
    }
    sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL || strchr(sp2 + 1, ' ') != NULL) {
        goto free_request;
    }

    if (copy_token(request->http_method, sizeof(request->http_method), line, (size_t)(sp1 - line)) != 0 ||
        copy_token(request->http_uri, sizeof(request->http_uri), sp1 + 1, (size_t)(sp2 - sp1 - 1)) != 0 ||
        copy_token(request->http_version, sizeof(request->http_version), sp2 + 1, strlen(sp2 + 1)) != 0) {
        goto free_request;
    }

    line = next + 2;
    while (line[0] != '\0') {
        next = strstr(line, "\r\n");
        if (next == NULL) {
            goto free_request;
        }
        if (next == line) {
            break;
        }
        *next = '\0';

        colon = strchr(line, ':');
        if (colon == NULL) {
            goto free_request;
        }
        *colon = '\0';
        if (append_header(request, line, strlen(line), trim_ows(colon + 1)) != 0) {
            goto free_request;
        }

        line = next + 2;
    }

    free(work);
    return request;
}
