#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "logger.h"
#include "parse.h"

#define LISO_PORT 9999
#define MAX_CONNECTIONS 1024
#define RECV_CHUNK 4096
#define MAX_HEADER_SIZE 8192
#define MAX_REQUEST_SIZE (1024 * 1024)
#define CGI_PREFIX "/cgi/"
#define SERVER_SOFTWARE "Liso/1.0"

static int listen_sock = -1;

typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    char *buf;
    size_t len;
    size_t cap;
} ClientState;

static int close_socket_if_needed(int fd)
{
    if (fd >= 0 && close(fd) != 0) {
        log_error("error", "close failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void handle_signal(int sig)
{
    log_error("notice", "received signal %d, shutting down", sig);
    close_socket_if_needed(listen_sock);
    logger_close();
    exit(0);
}

static int ensure_logs_dir(void)
{
    if (mkdir("logs", 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    fprintf(stderr, "cannot create logs directory: %s\n", strerror(errno));
    return -1;
}

static const char *status_text(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 505:
        return "HTTP Version not supported";
    default:
        return "Internal Server Error";
    }
}

static const char *content_type_for_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (strcasecmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcasecmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, ".txt") == 0) {
        return "text/plain";
    }
    return "application/octet-stream";
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static int send_simple_error(int fd, int status, bool keep_alive)
{
    char response[256];
    int response_len = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status,
        status_text(status),
        keep_alive ? "keep-alive" : "close"
    );

    if (response_len <= 0 || response_len >= (int)sizeof(response)) {
        return -1;
    }
    return send_all(fd, response, (size_t)response_len);
}

static int send_response(
    int client_fd,
    int status,
    const char *content_type,
    const void *body,
    size_t body_len,
    bool send_body,
    bool keep_alive
)
{
    char header[1024];
    int header_len = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status,
        status_text(status),
        body_len,
        (content_type != NULL) ? content_type : "text/plain",
        keep_alive ? "keep-alive" : "close"
    );

    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        return -1;
    }
    if (send_all(client_fd, header, (size_t)header_len) != 0) {
        return -1;
    }
    if (send_body && body_len > 0 && body != NULL) {
        if (send_all(client_fd, body, body_len) != 0) {
            return -1;
        }
    }
    return 0;
}

static char *make_env_pair(const char *name, const char *value)
{
    size_t name_len = strlen(name);
    size_t value_len = value == NULL ? 0 : strlen(value);
    char *entry = (char *)malloc(name_len + value_len + 2);

    if (entry == NULL) {
        return NULL;
    }
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    if (value_len > 0) {
        memcpy(entry + name_len + 1, value, value_len);
    }
    entry[name_len + value_len + 1] = '\0';
    return entry;
}

static ssize_t find_header_end(const char *buf, size_t len)
{
    size_t i;
    if (len < 4) {
        return -1;
    }
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (ssize_t)(i + 4);
        }
    }
    return -1;
}

static const char *request_get_header(const Request *req, const char *name)
{
    int i;
    for (i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].header_name, name) == 0) {
            return req->headers[i].header_value;
        }
    }
    return NULL;
}

static void discard_buffer_prefix(char *buf, size_t *len, size_t count)
{
    if (count >= *len) {
        *len = 0;
        return;
    }
    memmove(buf, buf + count, *len - count);
    *len -= count;
}

static bool looks_like_request_line_at(const char *buf, size_t len, size_t pos)
{
    size_t i = pos;

    if (pos >= len || buf[pos] == ' ' || buf[pos] == '\r' || buf[pos] == '\n') {
        return false;
    }

    while (i < len && buf[i] != ' ') {
        if (buf[i] < 'A' || buf[i] > 'Z') {
            return false;
        }
        i++;
    }
    if (i == pos || i >= len || buf[i] != ' ') {
        return false;
    }

    i++;
    if (i >= len || buf[i] == ' ') {
        return false;
    }
    while (i < len && buf[i] != ' ') {
        if (buf[i] == '\r' || buf[i] == '\n') {
            return false;
        }
        i++;
    }
    if (i >= len || buf[i] != ' ') {
        return false;
    }

    i++;
    if (i + 7 >= len || memcmp(buf + i, "HTTP/", 5) != 0) {
        return false;
    }
    while (i < len && buf[i] != '\r') {
        if (buf[i] == '\n') {
            return false;
        }
        i++;
    }
    return i + 1 < len && buf[i] == '\r' && buf[i + 1] == '\n';
}

static ssize_t find_next_request_start(const char *buf, size_t len, size_t start)
{
    size_t i;
    for (i = start; i < len; i++) {
        if (looks_like_request_line_at(buf, len, i)) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static bool only_crlf_bytes(const char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] != '\r' && buf[i] != '\n') {
            return false;
        }
    }
    return true;
}

static bool request_should_keep_alive(const Request *req)
{
    bool keep_alive = (strcmp(req->http_version, "HTTP/1.1") == 0);
    const char *connection = request_get_header(req, "Connection");
    if (connection != NULL) {
        if (strcasecmp(connection, "close") == 0) {
            keep_alive = false;
        } else if (strcasecmp(connection, "keep-alive") == 0) {
            keep_alive = true;
        }
    }
    return keep_alive;
}

static bool request_version_supported(const Request *req)
{
    return strcmp(req->http_version, "HTTP/1.1") == 0;
}

static bool request_method_implemented(const Request *req)
{
    return strcmp(req->http_method, "GET") == 0 ||
           strcmp(req->http_method, "HEAD") == 0 ||
           strcmp(req->http_method, "POST") == 0;
}

static int request_content_length(const Request *req)
{
    char *endptr;
    long value;
    const char *header_value = request_get_header(req, "Content-Length");
    if (header_value == NULL) {
        return 0;
    }
    errno = 0;
    value = strtol(header_value, &endptr, 10);
    if (errno != 0 || endptr == header_value || *endptr != '\0' || value < 0 || value > INT_MAX) {
        return -1;
    }
    return (int)value;
}

static bool is_cgi_request_uri(const char *uri)
{
    return uri != NULL && strncmp(uri, CGI_PREFIX, strlen(CGI_PREFIX)) == 0;
}

static int split_uri_query(const char *uri, char *path, size_t path_size, char *query, size_t query_size)
{
    const char *qmark;
    size_t path_len;

    if (uri == NULL || uri[0] != '/' || strstr(uri, "..") != NULL) {
        return -1;
    }

    qmark = strchr(uri, '?');
    path_len = qmark == NULL ? strlen(uri) : (size_t)(qmark - uri);
    if (path_len == 0 || path_len >= path_size) {
        return -1;
    }

    memcpy(path, uri, path_len);
    path[path_len] = '\0';

    if (query_size > 0) {
        if (qmark == NULL) {
            query[0] = '\0';
        } else if (strlen(qmark + 1) >= query_size) {
            return -1;
        } else {
            strcpy(query, qmark + 1);
        }
    }
    return 0;
}

static int resolve_cgi_path(
    const char *uri,
    char *script_name,
    size_t script_name_size,
    char *script_path,
    size_t script_path_size,
    char *path_info,
    size_t path_info_size,
    char *query_string,
    size_t query_string_size
)
{
    char clean_path[4096];
    const char *script_start;
    const char *slash_after_script;
    size_t script_len;

    if (split_uri_query(uri, clean_path, sizeof(clean_path), query_string, query_string_size) != 0 ||
        !is_cgi_request_uri(clean_path)) {
        return -1;
    }

    script_start = clean_path + strlen(CGI_PREFIX);
    if (*script_start == '\0') {
        return -1;
    }

    slash_after_script = strchr(script_start, '/');
    script_len = slash_after_script == NULL ? strlen(script_start) : (size_t)(slash_after_script - script_start);
    if (script_len == 0 || script_len >= 256) {
        return -1;
    }

    if (snprintf(script_name, script_name_size, "%s%.*s", CGI_PREFIX, (int)script_len, script_start) >= (int)script_name_size ||
        snprintf(script_path, script_path_size, "cgi/%.*s", (int)script_len, script_start) >= (int)script_path_size) {
        return -1;
    }

    if (slash_after_script == NULL) {
        path_info[0] = '\0';
    } else if (strlen(slash_after_script) >= path_info_size) {
        return -1;
    } else {
        strcpy(path_info, slash_after_script);
    }

    return 0;
}

static int resolve_path(const char *uri, char *out, size_t out_size)
{
    const char *mapped_uri = uri;
    char clean_uri[4096];
    const char *query;
    size_t clean_len;

    if (uri == NULL || uri[0] != '/') {
        return -1;
    }
    if (strstr(uri, "..") != NULL) {
        return -1;
    }

    query = strchr(uri, '?');
    clean_len = (query != NULL) ? (size_t)(query - uri) : strlen(uri);
    if (clean_len == 0 || clean_len >= sizeof(clean_uri)) {
        return -1;
    }
    memcpy(clean_uri, uri, clean_len);
    clean_uri[clean_len] = '\0';

    if (strcmp(clean_uri, "/") == 0) {
        mapped_uri = "/index.html";
    } else {
        mapped_uri = clean_uri;
    }
    if (snprintf(out, out_size, "static_site%s", mapped_uri) >= (int)out_size) {
        return -1;
    }
    return 0;
}

static int load_file(const char *path, char **data, size_t *size)
{
    int fd;
    struct stat st;
    char *buf;
    ssize_t n;
    size_t off = 0;

    *data = NULL;
    *size = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 404;
    }
    if (fstat(fd, &st) != 0) {
        close(fd);
        return 404;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return 404;
    }

    if (st.st_size < 0 || st.st_size > MAX_REQUEST_SIZE) {
        close(fd);
        return 404;
    }

    *size = (size_t)st.st_size;
    if (*size == 0) {
        close(fd);
        return 200;
    }

    buf = (char *)malloc(*size);
    if (buf == NULL) {
        close(fd);
        return 404;
    }

    while (off < *size) {
        n = read(fd, buf + off, *size - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return 404;
        }
        if (n == 0) {
            break;
        }
        off += (size_t)n;
    }

    close(fd);
    *data = buf;
    *size = off;
    return 200;
}

static void free_request(Request *request)
{
    if (request == NULL) {
        return;
    }
    free(request->headers);
    free(request);
}

static int handle_request(
    int client_fd,
    const char *client_ip,
    const Request *request,
    const char *raw_request,
    size_t raw_request_len,
    bool *keep_alive_out,
    size_t *bytes_out
);

static void init_client(ClientState *client)
{
    client->fd = -1;
    client->ip[0] = '\0';
    client->buf = NULL;
    client->len = 0;
    client->cap = 0;
}

static void close_client(ClientState *client)
{
    if (client->fd >= 0) {
        close_socket_if_needed(client->fd);
    }
    free(client->buf);
    init_client(client);
}

static int ensure_client_capacity(ClientState *client, size_t additional)
{
    size_t needed = client->len + additional;

    if (needed > MAX_REQUEST_SIZE) {
        return -1;
    }
    if (needed <= client->cap) {
        return 0;
    }

    size_t new_cap = client->cap == 0 ? 8192 : client->cap * 2;
    char *new_buf;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    new_buf = realloc(client->buf, new_cap);
    if (new_buf == NULL) {
        return -1;
    }
    client->buf = new_buf;
    client->cap = new_cap;
    return 0;
}

static int append_client_data(ClientState *client, const char *data, size_t len)
{
    if (ensure_client_capacity(client, len) != 0) {
        return -1;
    }
    memcpy(client->buf + client->len, data, len);
    client->len += len;
    return 0;
}

static bool process_client_buffer(ClientState *client)
{
    while (1) {
        ssize_t header_end;
        Request *request;
        int content_length = 0;
        bool content_length_present = false;
        size_t total_needed;
        bool keep_alive = false;
        size_t bytes_sent = 0;
        int handle_status;

        while (client->len >= 2 && client->buf[0] == '\r' && client->buf[1] == '\n') {
            memmove(client->buf, client->buf + 2, client->len - 2);
            client->len -= 2;
        }

        header_end = find_header_end(client->buf, client->len);
        if (header_end < 0) {
            if (client->len > MAX_HEADER_SIZE) {
                log_error("error", "request header too large from %s", client->ip);
                send_simple_error(client->fd, 400, false);
                log_access(client->ip, NULL, NULL, NULL, 400, 0);
                return false;
            }
            return true;
        }

        request = parse(client->buf, (int)header_end, client->fd);
        if (request == NULL) {
            bool can_continue = false;
            ssize_t next_start = find_next_request_start(client->buf, client->len, (size_t)header_end);

            log_error("error", "parse failed from %s", client->ip);
            if (next_start >= 0) {
                can_continue = true;
                discard_buffer_prefix(client->buf, &client->len, (size_t)next_start);
            } else {
                discard_buffer_prefix(client->buf, &client->len, client->len);
            }
            send_simple_error(client->fd, 400, can_continue);
            log_access(client->ip, NULL, NULL, NULL, 400, 0);
            if (!can_continue) {
                return false;
            }
            continue;
        }

        content_length_present = request_get_header(request, "Content-Length") != NULL;
        content_length = request_content_length(request);
        if (content_length < 0) {
            bool can_continue = false;
            ssize_t next_start = find_next_request_start(client->buf, client->len, (size_t)header_end);

            log_error("error", "invalid Content-Length from %s", client->ip);
            if (next_start >= 0) {
                discard_buffer_prefix(client->buf, &client->len, (size_t)next_start);
                can_continue = true;
            } else {
                discard_buffer_prefix(client->buf, &client->len, client->len);
            }
            send_simple_error(client->fd, 400, can_continue);
            log_access(client->ip, request->http_method, request->http_uri, request->http_version, 400, 0);
            free_request(request);
            if (!can_continue) {
                return false;
            }
            continue;
        }

        total_needed = (size_t)header_end + (size_t)content_length;
        if (total_needed > MAX_REQUEST_SIZE) {
            log_error("error", "request too large from %s", client->ip);
            send_simple_error(client->fd, 400, false);
            log_access(client->ip, request->http_method, request->http_uri, request->http_version, 400, 0);
            free_request(request);
            return false;
        }
        if (client->len < total_needed) {
            free_request(request);
            return true;
        }

        if (strcmp(request->http_method, "POST") == 0 &&
            !content_length_present &&
            client->len > total_needed &&
            only_crlf_bytes(client->buf + total_needed, client->len - total_needed)) {
            total_needed = client->len;
        }

        handle_status = handle_request(
            client->fd,
            client->ip,
            request,
            client->buf,
            total_needed,
            &keep_alive,
            &bytes_sent);

        if (handle_status < 0) {
            log_error("error", "send failed to %s: %s", client->ip, strerror(errno));
            free_request(request);
            return false;
        }

        discard_buffer_prefix(client->buf, &client->len, total_needed);
        free_request(request);

        if (!keep_alive && client->len == 0) {
            return false;
        }
        if (client->len == 0) {
            return true;
        }
    }
}

static void free_env(char **envp, size_t env_count)
{
    size_t i;
    if (envp == NULL) {
        return;
    }
    for (i = 0; i < env_count; i++) {
        free(envp[i]);
    }
    free(envp);
}

static int add_env(char **envp, size_t env_cap, size_t *env_count, const char *name, const char *value)
{
    if (*env_count + 1 >= env_cap) {
        return -1;
    }
    envp[*env_count] = make_env_pair(name, value);
    if (envp[*env_count] == NULL) {
        return -1;
    }
    (*env_count)++;
    envp[*env_count] = NULL;
    return 0;
}

static int add_header_env(
    char **envp,
    size_t env_cap,
    size_t *env_count,
    const Request *request,
    const char *env_name,
    const char *header_name
)
{
    return add_env(envp, env_cap, env_count, env_name, request_get_header(request, header_name));
}

static char **build_cgi_env(
    const Request *request,
    const char *client_ip,
    const char *script_name,
    const char *path_info,
    const char *query_string,
    size_t *env_count_out
)
{
    char **envp;
    size_t env_count = 0;
    const size_t env_cap = 32;
    char server_port[16];

    envp = (char **)calloc(env_cap, sizeof(char *));
    if (envp == NULL) {
        return NULL;
    }

    snprintf(server_port, sizeof(server_port), "%d", LISO_PORT);

    if (add_header_env(envp, env_cap, &env_count, request, "CONTENT_LENGTH", "Content-Length") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "CONTENT_TYPE", "Content-Type") != 0 ||
        add_env(envp, env_cap, &env_count, "GATEWAY_INTERFACE", "CGI/1.1") != 0 ||
        add_env(envp, env_cap, &env_count, "PATH_INFO", path_info) != 0 ||
        add_env(envp, env_cap, &env_count, "QUERY_STRING", query_string) != 0 ||
        add_env(envp, env_cap, &env_count, "REMOTE_ADDR", client_ip) != 0 ||
        add_env(envp, env_cap, &env_count, "REQUEST_METHOD", request->http_method) != 0 ||
        add_env(envp, env_cap, &env_count, "REQUEST_URI", request->http_uri) != 0 ||
        add_env(envp, env_cap, &env_count, "SCRIPT_NAME", script_name) != 0 ||
        add_env(envp, env_cap, &env_count, "SERVER_PORT", server_port) != 0 ||
        add_env(envp, env_cap, &env_count, "SERVER_PROTOCOL", "HTTP/1.1") != 0 ||
        add_env(envp, env_cap, &env_count, "SERVER_SOFTWARE", SERVER_SOFTWARE) != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_ACCEPT", "Accept") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_REFERER", "Referer") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_ACCEPT_ENCODING", "Accept-Encoding") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_ACCEPT_LANGUAGE", "Accept-Language") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_ACCEPT_CHARSET", "Accept-Charset") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_HOST", "Host") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_COOKIE", "Cookie") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_USER_AGENT", "User-Agent") != 0 ||
        add_header_env(envp, env_cap, &env_count, request, "HTTP_CONNECTION", "Connection") != 0) {
        free_env(envp, env_count);
        return NULL;
    }

    *env_count_out = env_count;
    return envp;
}

static int append_output(char **buf, size_t *len, size_t *cap, const char *data, size_t data_len)
{
    size_t needed = *len + data_len;
    char *new_buf;
    size_t new_cap;

    if (needed > MAX_REQUEST_SIZE) {
        return -1;
    }
    if (needed <= *cap) {
        memcpy(*buf + *len, data, data_len);
        *len = needed;
        return 0;
    }

    new_cap = *cap == 0 ? 8192 : *cap * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    new_buf = (char *)realloc(*buf, new_cap);
    if (new_buf == NULL) {
        return -1;
    }
    *buf = new_buf;
    *cap = new_cap;
    memcpy(*buf + *len, data, data_len);
    *len = needed;
    return 0;
}

static int send_cgi_output(int client_fd, const char *output, size_t output_len)
{
    const char *prefix = "HTTP/1.1 200 OK\r\nConnection: close\r\n";

    if (output_len >= 5 && memcmp(output, "HTTP/", 5) == 0) {
        return send_all(client_fd, output, output_len);
    }
    if (send_all(client_fd, prefix, strlen(prefix)) != 0) {
        return -1;
    }
    return send_all(client_fd, output, output_len);
}

static int run_cgi(
    int client_fd,
    const char *client_ip,
    const Request *request,
    const char *body,
    size_t body_len,
    bool *keep_alive_out,
    size_t *bytes_out
)
{
    char script_name[512];
    char script_path[1024];
    char path_info[4096];
    char query_string[4096];
    char *argv[2];
    char **envp = NULL;
    size_t env_count = 0;
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    pid_t pid;
    int status;
    char *cgi_output = NULL;
    size_t output_len = 0;
    size_t output_cap = 0;
    int result = -1;

    *keep_alive_out = false;
    *bytes_out = 0;

    if (resolve_cgi_path(
            request->http_uri,
            script_name,
            sizeof(script_name),
            script_path,
            sizeof(script_path),
            path_info,
            sizeof(path_info),
            query_string,
            sizeof(query_string)) != 0) {
        send_simple_error(client_fd, 404, false);
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, 404, 0);
        return 404;
    }

    if (access(script_path, X_OK) != 0) {
        send_simple_error(client_fd, 404, false);
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, 404, 0);
        return 404;
    }

    envp = build_cgi_env(request, client_ip, script_name, path_info, query_string, &env_count);
    if (envp == NULL || pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        send_simple_error(client_fd, 500, false);
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, 500, 0);
        goto cleanup;
    }

    pid = fork();
    if (pid < 0) {
        send_simple_error(client_fd, 500, false);
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, 500, 0);
        goto cleanup;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        argv[0] = script_path;
        argv[1] = NULL;
        execve(script_path, argv, envp);
        _exit(127);
    }

    close(stdin_pipe[0]);
    stdin_pipe[0] = -1;
    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;

    if (body_len > 0 && write_all(stdin_pipe[1], body, body_len) != 0) {
        close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
        waitpid(pid, &status, 0);
        send_simple_error(client_fd, 500, false);
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, 500, 0);
        goto cleanup;
    }
    close(stdin_pipe[1]);
    stdin_pipe[1] = -1;

    while (1) {
        char buf[RECV_CHUNK];
        ssize_t n = read(stdout_pipe[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            waitpid(pid, &status, 0);
            send_simple_error(client_fd, 500, false);
            log_access(client_ip, request->http_method, request->http_uri, request->http_version, 500, 0);
            goto cleanup;
        }
        if (n == 0) {
            break;
        }
        if (append_output(&cgi_output, &output_len, &output_cap, buf, (size_t)n) != 0) {
            waitpid(pid, &status, 0);
            send_simple_error(client_fd, 500, false);
            log_access(client_ip, request->http_method, request->http_uri, request->http_version, 500, 0);
            goto cleanup;
        }
    }

    close(stdout_pipe[0]);
    stdout_pipe[0] = -1;

    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        send_simple_error(client_fd, 500, false);
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, 500, 0);
        goto cleanup;
    }

    if (send_cgi_output(client_fd, cgi_output == NULL ? "" : cgi_output, output_len) != 0) {
        goto cleanup;
    }

    *bytes_out = output_len;
    log_access(client_ip, request->http_method, request->http_uri, request->http_version, 200, *bytes_out);
    result = 200;

cleanup:
    if (stdin_pipe[0] >= 0) {
        close(stdin_pipe[0]);
    }
    if (stdin_pipe[1] >= 0) {
        close(stdin_pipe[1]);
    }
    if (stdout_pipe[0] >= 0) {
        close(stdout_pipe[0]);
    }
    if (stdout_pipe[1] >= 0) {
        close(stdout_pipe[1]);
    }
    free(cgi_output);
    free_env(envp, env_count);
    return result;
}

static int handle_request(
    int client_fd,
    const char *client_ip,
    const Request *request,
    const char *raw_request,
    size_t raw_request_len,
    bool *keep_alive_out,
    size_t *bytes_out
)
{
    int status = 500;
    bool keep_alive = request_should_keep_alive(request);

    *keep_alive_out = keep_alive;
    *bytes_out = 0;

    if (!request_version_supported(request)) {
        status = 505;
        *keep_alive_out = keep_alive;
        if (send_simple_error(client_fd, status, keep_alive) != 0) {
            return -1;
        }
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
        return status;
    }

    if (!request_method_implemented(request)) {
        status = 501;
        *keep_alive_out = keep_alive;
        if (send_simple_error(client_fd, status, keep_alive) != 0) {
            return -1;
        }
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
        return status;
    }

    if (is_cgi_request_uri(request->http_uri)) {
        ssize_t header_end = find_header_end(raw_request, raw_request_len);
        const char *body = "";
        size_t body_len = 0;

        if (header_end >= 0 && raw_request_len > (size_t)header_end) {
            body = raw_request + header_end;
            body_len = raw_request_len - (size_t)header_end;
        }
        return run_cgi(client_fd, client_ip, request, body, body_len, keep_alive_out, bytes_out);
    }

    if (strcmp(request->http_method, "GET") == 0 || strcmp(request->http_method, "HEAD") == 0) {
        char path[8192];
        char *file_data = NULL;
        size_t file_size = 0;
        int file_status;
        bool send_body = strcmp(request->http_method, "GET") == 0;

        if (resolve_path(request->http_uri, path, sizeof(path)) != 0) {
            status = 400;
            *keep_alive_out = keep_alive;
            if (send_simple_error(client_fd, status, keep_alive) != 0) {
                return -1;
            }
            log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
            return status;
        }

        file_status = load_file(path, &file_data, &file_size);
        if (file_status != 200) {
            status = 404;
            *keep_alive_out = keep_alive;
            log_error("error", "file access failed for %s: errno=%s", path, strerror(errno));
            if (send_simple_error(client_fd, status, keep_alive) != 0) {
                free(file_data);
                return -1;
            }
            log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
            free(file_data);
            return status;
        }

        status = 200;
        if (send_response(
                client_fd,
                status,
                content_type_for_path(path),
                file_data,
                file_size,
                send_body,
                keep_alive) != 0) {
            free(file_data);
            return -1;
        }
        *bytes_out = send_body ? file_size : 0;
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, *bytes_out);
        free(file_data);
        return status;
    }

    if (strcmp(request->http_method, "POST") == 0) {
        status = 200;
        if (send_response(
                client_fd,
                status,
                "message/http",
                raw_request,
                raw_request_len,
                true,
                keep_alive) != 0) {
            return -1;
        }
        *bytes_out = raw_request_len;
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, *bytes_out);
        return status;
    }

    return -1;
}

int main(void)
{
    struct sockaddr_in addr;
    int opt = 1;
    ClientState clients[MAX_CONNECTIONS];
    int i;

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGSEGV, handle_signal);
    signal(SIGABRT, handle_signal);
    signal(SIGQUIT, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    if (ensure_logs_dir() != 0) {
        return EXIT_FAILURE;
    }
    if (logger_init("logs/error.log", "logs/access.log") != 0) {
        fprintf(stderr, "logger initialization failed\n");
        return EXIT_FAILURE;
    }

    listen_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_sock == -1) {
        log_error("error", "socket creation failed: %s", strerror(errno));
        logger_close();
        return EXIT_FAILURE;
    }

    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        log_error("error", "setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close_socket_if_needed(listen_sock);
        logger_close();
        return EXIT_FAILURE;
    }
#ifdef SO_REUSEPORT
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) != 0) {
        log_error("warn", "setsockopt SO_REUSEPORT failed: %s", strerror(errno));
    }
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("error", "bind failed: %s", strerror(errno));
        close_socket_if_needed(listen_sock);
        logger_close();
        return EXIT_FAILURE;
    }

    if (listen(listen_sock, MAX_CONNECTIONS) != 0) {
        log_error("error", "listen failed: %s", strerror(errno));
        close_socket_if_needed(listen_sock);
        logger_close();
        return EXIT_FAILURE;
    }

    for (i = 0; i < MAX_CONNECTIONS; i++) {
        init_client(&clients[i]);
    }

    log_error("notice", "liso_server started on port %d", LISO_PORT);

    while (1) {
        fd_set readfds;
        int max_fd = listen_sock;
        int ready;

        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &readfds);
                if (clients[i].fd > max_fd) {
                    max_fd = clients[i].fd;
                }
            }
        }

        ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("error", "select failed: %s", strerror(errno));
            continue;
        }

        if (FD_ISSET(listen_sock, &readfds)) {
            int client_fd;
            int slot = -1;
            struct sockaddr_in cli_addr;
            socklen_t cli_size = sizeof(cli_addr);

            for (i = 0; i < MAX_CONNECTIONS; i++) {
                if (clients[i].fd < 0) {
                    slot = i;
                    break;
                }
            }

            client_fd = accept(listen_sock, (struct sockaddr *)&cli_addr, &cli_size);
            if (client_fd < 0) {
                log_error("error", "accept failed: %s", strerror(errno));
            } else if (slot < 0 || client_fd >= FD_SETSIZE) {
                log_error("warn", "too many clients, rejecting fd %d", client_fd);
                close_socket_if_needed(client_fd);
            } else {
                clients[slot].fd = client_fd;
                if (inet_ntop(AF_INET, &cli_addr.sin_addr, clients[slot].ip, sizeof(clients[slot].ip)) == NULL) {
                    strcpy(clients[slot].ip, "-");
                }
                log_error("notice", "accepted connection from %s on fd %d", clients[slot].ip, client_fd);
            }
        }

        for (i = 0; i < MAX_CONNECTIONS; i++) {
            char recv_buf[RECV_CHUNK];
            ssize_t n;

            if (clients[i].fd < 0 || !FD_ISSET(clients[i].fd, &readfds)) {
                continue;
            }

            n = recv(clients[i].fd, recv_buf, sizeof(recv_buf), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n < 0) {
                    log_error("error", "recv failed from %s: %s", clients[i].ip, strerror(errno));
                }
                close_client(&clients[i]);
                continue;
            }

            if (append_client_data(&clients[i], recv_buf, (size_t)n) != 0) {
                log_error("error", "request too large or memory allocation failed from %s", clients[i].ip);
                send_simple_error(clients[i].fd, 400, false);
                log_access(clients[i].ip, NULL, NULL, NULL, 400, 0);
                close_client(&clients[i]);
                continue;
            }

            if (!process_client_buffer(&clients[i])) {
                close_client(&clients[i]);
            }
        }
    }

    close_socket_if_needed(listen_sock);
    logger_close();
    return EXIT_SUCCESS;
}
