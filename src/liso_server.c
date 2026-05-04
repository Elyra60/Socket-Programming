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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger.h"
#include "parse.h"

#define LISO_PORT 9999
#define RECV_CHUNK 4096
#define MAX_HEADER_SIZE 8192
#define MAX_REQUEST_SIZE (1024 * 1024)

#define RESPONSE_400 "HTTP/1.1 400 Bad request\r\n\r\n"
#define RESPONSE_404 "HTTP/1.1 404 Not Found\r\n\r\n"
#define RESPONSE_501 "HTTP/1.1 501 Not Implemented\r\n\r\n"
#define RESPONSE_505 "HTTP/1.1 505 HTTP Version not supported\r\n\r\n"

static int listen_sock = -1;

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

static int send_simple_error(int fd, int status)
{
    const char *response;

    switch (status) {
    case 400:
        response = RESPONSE_400;
        break;
    case 404:
        response = RESPONSE_404;
        break;
    case 501:
        response = RESPONSE_501;
        break;
    case 505:
        response = RESPONSE_505;
        break;
    default:
        response = RESPONSE_400;
        break;
    }

    return send_all(fd, response, strlen(response));
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
)
{
    int status = 500;
    bool keep_alive = request_should_keep_alive(request);

    *keep_alive_out = keep_alive;
    *bytes_out = 0;

    if (!request_version_supported(request)) {
        status = 505;
        *keep_alive_out = false;
        if (send_simple_error(client_fd, status) != 0) {
            return -1;
        }
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
        return status;
    }

    if (!request_method_implemented(request)) {
        status = 501;
        *keep_alive_out = false;
        if (send_simple_error(client_fd, status) != 0) {
            return -1;
        }
        log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
        return status;
    }

    if (strcmp(request->http_method, "GET") == 0 || strcmp(request->http_method, "HEAD") == 0) {
        char path[8192];
        char *file_data = NULL;
        size_t file_size = 0;
        int file_status;
        bool send_body = strcmp(request->http_method, "GET") == 0;

        if (resolve_path(request->http_uri, path, sizeof(path)) != 0) {
            status = 400;
            *keep_alive_out = false;
            if (send_simple_error(client_fd, status) != 0) {
                return -1;
            }
            log_access(client_ip, request->http_method, request->http_uri, request->http_version, status, 0);
            return status;
        }

        file_status = load_file(path, &file_data, &file_size);
        if (file_status != 200) {
            status = 404;
            *keep_alive_out = false;
            log_error("error", "file access failed for %s: errno=%s", path, strerror(errno));
            if (send_simple_error(client_fd, status) != 0) {
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
        if (send_all(client_fd, raw_request, raw_request_len) != 0) {
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

    if (listen(listen_sock, 8) != 0) {
        log_error("error", "listen failed: %s", strerror(errno));
        close_socket_if_needed(listen_sock);
        logger_close();
        return EXIT_FAILURE;
    }

    log_error("notice", "liso_server started on port %d", LISO_PORT);

    while (1) {
        int client_fd;
        struct sockaddr_in cli_addr;
        socklen_t cli_size = sizeof(cli_addr);
        char client_ip[INET_ADDRSTRLEN] = "-";
        char *conn_buf = NULL;
        size_t conn_len = 0;
        size_t conn_cap = 0;
        bool keep_connection = true;

        client_fd = accept(listen_sock, (struct sockaddr *)&cli_addr, &cli_size);
        if (client_fd < 0) {
            log_error("error", "accept failed: %s", strerror(errno));
            continue;
        }

        inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, sizeof(client_ip));

        while (keep_connection) {
            ssize_t header_end;
            Request *request;
            int content_length = 0;
            bool content_length_present = false;
            size_t total_needed;
            bool keep_alive = false;
            size_t bytes_sent = 0;
            int handle_status;

            while (conn_len >= 2 && conn_buf[0] == '\r' && conn_buf[1] == '\n') {
                memmove(conn_buf, conn_buf + 2, conn_len - 2);
                conn_len -= 2;
            }

            header_end = find_header_end(conn_buf, conn_len);
            while (header_end < 0) {
                char recv_buf[RECV_CHUNK];
                ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
                if (n <= 0) {
                    keep_connection = false;
                    break;
                }
                if (conn_len + (size_t)n > MAX_REQUEST_SIZE) {
                    log_error("error", "request too large from %s", client_ip);
                    send_simple_error(client_fd, 400);
                    keep_connection = false;
                    break;
                }
                if (conn_len + (size_t)n > conn_cap) {
                    size_t new_cap = conn_cap == 0 ? 8192 : conn_cap * 2;
                    char *new_buf;
                    while (new_cap < conn_len + (size_t)n) {
                        new_cap *= 2;
                    }
                    new_buf = realloc(conn_buf, new_cap);
                    if (new_buf == NULL) {
                        log_error("error", "memory allocation failed while reading request");
                        keep_connection = false;
                        break;
                    }
                    conn_buf = new_buf;
                    conn_cap = new_cap;
                }
                memcpy(conn_buf + conn_len, recv_buf, (size_t)n);
                conn_len += (size_t)n;
                header_end = find_header_end(conn_buf, conn_len);
                if (header_end < 0 && conn_len > MAX_HEADER_SIZE) {
                    log_error("error", "request header too large from %s", client_ip);
                    send_simple_error(client_fd, 400);
                    keep_connection = false;
                    break;
                }
            }

            if (!keep_connection) {
                break;
            }

            request = parse(conn_buf, (int)header_end, client_fd);
            if (request == NULL) {
                log_error("error", "parse failed from %s", client_ip);
                send_simple_error(client_fd, 400);
                log_access(client_ip, NULL, NULL, NULL, 400, 0);
                keep_connection = false;
                break;
            }

            if (strcmp(request->http_method, "POST") == 0) {
                content_length_present = request_get_header(request, "Content-Length") != NULL;
                content_length = request_content_length(request);
                if (content_length < 0) {
                    log_error("error", "invalid Content-Length from %s", client_ip);
                    send_simple_error(client_fd, 400);
                    log_access(client_ip, request->http_method, request->http_uri, request->http_version, 400, 0);
                    free_request(request);
                    keep_connection = false;
                    break;
                }
            }

            total_needed = (size_t)header_end + (size_t)content_length;
            while (conn_len < total_needed) {
                char recv_buf[RECV_CHUNK];
                ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
                if (n <= 0) {
                    keep_connection = false;
                    break;
                }
                if (conn_len + (size_t)n > MAX_REQUEST_SIZE) {
                    log_error("error", "request body too large from %s", client_ip);
                    send_simple_error(client_fd, 400);
                    log_access(client_ip, request->http_method, request->http_uri, request->http_version, 400, 0);
                    keep_connection = false;
                    break;
                }
                if (conn_len + (size_t)n > conn_cap) {
                    size_t new_cap = conn_cap == 0 ? 8192 : conn_cap * 2;
                    char *new_buf;
                    while (new_cap < conn_len + (size_t)n) {
                        new_cap *= 2;
                    }
                    new_buf = realloc(conn_buf, new_cap);
                    if (new_buf == NULL) {
                        log_error("error", "memory allocation failed while reading body");
                        keep_connection = false;
                        break;
                    }
                    conn_buf = new_buf;
                    conn_cap = new_cap;
                }
                memcpy(conn_buf + conn_len, recv_buf, (size_t)n);
                conn_len += (size_t)n;
            }

            if (!keep_connection) {
                free_request(request);
                break;
            }

            if (strcmp(request->http_method, "POST") == 0 &&
                !content_length_present &&
                conn_len > total_needed &&
                only_crlf_bytes(conn_buf + total_needed, conn_len - total_needed)) {
                total_needed = conn_len;
            }

            handle_status = handle_request(
                client_fd,
                client_ip,
                request,
                conn_buf,
                total_needed,
                &keep_alive,
                &bytes_sent);

            if (handle_status < 0) {
                log_error("error", "send failed to %s: %s", client_ip, strerror(errno));
                keep_connection = false;
                free_request(request);
                break;
            }

            if (conn_len > total_needed) {
                memmove(conn_buf, conn_buf + total_needed, conn_len - total_needed);
            }
            conn_len -= total_needed;

            free_request(request);
            keep_connection = keep_alive || conn_len > 0;
        }

        free(conn_buf);
        close_socket_if_needed(client_fd);
    }

    close_socket_if_needed(listen_sock);
    logger_close();
    return EXIT_SUCCESS;
}
