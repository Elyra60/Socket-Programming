#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void html_print(const char *s)
{
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        switch (*s) {
        case '&':
            fputs("&amp;", stdout);
            break;
        case '<':
            fputs("&lt;", stdout);
            break;
        case '>':
            fputs("&gt;", stdout);
            break;
        case '"':
            fputs("&quot;", stdout);
            break;
        default:
            fputc(*s, stdout);
            break;
        }
        s++;
    }
}

static char *read_stdin_body(void)
{
    const char *len_s = getenv("CONTENT_LENGTH");
    char *endptr;
    long len;
    char *body;
    size_t got = 0;

    if (len_s == NULL || *len_s == '\0') {
        return strdup("");
    }

    len = strtol(len_s, &endptr, 10);
    if (*endptr != '\0' || len < 0 || len > 1024 * 1024) {
        return strdup("");
    }

    body = (char *)malloc((size_t)len + 1);
    if (body == NULL) {
        return NULL;
    }

    while (got < (size_t)len) {
        size_t n = fread(body + got, 1, (size_t)len - got, stdin);
        if (n == 0) {
            break;
        }
        got += n;
    }
    body[got] = '\0';
    return body;
}

static char *field_value(const char *data, const char *name)
{
    size_t name_len = strlen(name);
    char *copy = strdup(data == NULL ? "" : data);
    char *tok;
    char *save = NULL;

    if (copy == NULL) {
        return NULL;
    }

    for (tok = strtok_r(copy, "&", &save); tok != NULL; tok = strtok_r(NULL, "&", &save)) {
        if (strncmp(tok, name, name_len) == 0 && tok[name_len] == '=') {
            char *value = strdup(tok + name_len + 1);
            free(copy);
            if (value != NULL) {
                url_decode(value);
            }
            return value;
        }
    }

    free(copy);
    return strdup("");
}

int main(void)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *query = getenv("QUERY_STRING");
    char *post_body = NULL;
    const char *form_data;
    char *username = NULL;
    char *password = NULL;

    if (method != NULL && strcmp(method, "POST") == 0) {
        post_body = read_stdin_body();
        form_data = post_body == NULL ? "" : post_body;
    } else {
        form_data = query == NULL ? "" : query;
    }

    username = field_value(form_data, "username");
    password = field_value(form_data, "password");

    printf("Content-Type: text/html; charset=utf-8\r\n\r\n");
    printf("<!doctype html><html><head><meta charset=\"utf-8\"><title>CGI Result</title>");
    printf("<style>body{font-family:Arial,sans-serif;max-width:760px;margin:40px auto;line-height:1.6}"
           "table{border-collapse:collapse;width:100%%}td,th{border:1px solid #ccc;padding:6px 8px}"
           "code{background:#f3f3f3;padding:2px 4px}</style></head><body>");
    printf("<h1>CGI form result</h1>");
    printf("<p>Submitted username: <strong>");
    html_print(username);
    printf("</strong></p><p>Submitted password: <strong>");
    html_print(password);
    printf("</strong></p>");

    printf("<h2>CGI environment</h2><table><tr><th>Name</th><th>Value</th></tr>");
    {
        const char *names[] = {
            "CONTENT_LENGTH", "CONTENT_TYPE", "GATEWAY_INTERFACE", "PATH_INFO",
            "QUERY_STRING", "REMOTE_ADDR", "REQUEST_METHOD", "REQUEST_URI",
            "SCRIPT_NAME", "SERVER_PORT", "SERVER_PROTOCOL", "SERVER_SOFTWARE",
            "HTTP_ACCEPT", "HTTP_REFERER", "HTTP_ACCEPT_ENCODING",
            "HTTP_ACCEPT_LANGUAGE", "HTTP_ACCEPT_CHARSET", "HTTP_HOST",
            "HTTP_COOKIE", "HTTP_USER_AGENT", "HTTP_CONNECTION", NULL
        };
        int i;
        for (i = 0; names[i] != NULL; i++) {
            printf("<tr><td><code>%s</code></td><td>", names[i]);
            html_print(getenv(names[i]));
            printf("</td></tr>");
        }
    }
    printf("</table><p><a href=\"/cgi_form.html\">Back to form</a></p></body></html>");

    free(post_body);
    free(username);
    free(password);
    return 0;
}
