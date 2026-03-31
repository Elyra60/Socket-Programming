#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include "parse.h"
#define ECHO_PORT 9999
#define BUF_SIZE 4096

int sock = -1, client_sock = -1;
char buf[BUF_SIZE];

int close_socket(int sock) {
    if (close(sock)) {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}
void handle_signal(const int sig) {
    if (sock != -1) {
        fprintf(stderr, "\nReceived signal %d. Closing socket.\n", sig);
        close_socket(sock);
    }
    exit(0);
}
void handle_sigpipe(const int sig) 
{
    if (sock != -1) {
        return;
    }
    exit(0);
}
int main(int argc, char *argv[]) {
    /* register signal handler */
    /* process termination signals */
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGSEGV, handle_signal);
    signal(SIGABRT, handle_signal);
    signal(SIGQUIT, handle_signal);
    signal(SIGTSTP, handle_signal);
    signal(SIGFPE, handle_signal);
    signal(SIGHUP, handle_signal);
    /* normal I/O event */
    signal(SIGPIPE, handle_sigpipe);
    socklen_t cli_size;
    struct sockaddr_in addr, cli_addr;
    fprintf(stdout, "----- Echo Server -----\n");

    /* all networked programs must create a socket */
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }
    /* set socket SO_REUSEADDR | SO_REUSEPORT */
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        fprintf(stderr, "Failed setting socket options.\n");
        return EXIT_FAILURE;
    }

    addr.sin_family = AF_INET; // ipv4
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    /* servers bind sockets to ports---notify the OS they accept connections */
    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr))) {
        close_socket(sock);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }

    if (listen(sock, 5)) {
        close_socket(sock);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }

    /* finally, loop waiting for input and then write it back */

    while (1) {
        /* listen for new connection */
        cli_size = sizeof(cli_addr);
        fprintf(stdout,"Waiting for connection...\n");
        client_sock = accept(sock, (struct sockaddr *) &cli_addr, &cli_size);
        if (client_sock == -1)
        {
            fprintf(stderr, "Error accepting connection.\n");
            close_socket(sock);
            return EXIT_FAILURE;
        }
        fprintf(stdout,"New connection from %s:%d\n",inet_ntoa(cli_addr.sin_addr),ntohs(cli_addr.sin_port));
        while(1){
            /* receive HTTP request from client */
            memset(buf, 0, BUF_SIZE);
            int readret = recv(client_sock, buf, BUF_SIZE, 0);
            if (readret <= 0) {
                break;
            }

            fprintf(stdout,"Received (total %d bytes):\n%.*s\n", readret, readret, buf);

            /* parse HTTP request */
            Request *request = parse(buf, readret, client_sock);

            if (request == NULL) {
                /* format error */
                const char *bad_req = "HTTP/1.1 400 Bad request\r\n\r\n";
                send(client_sock, bad_req, strlen(bad_req), 0);
                fprintf(stdout, "Sent 400 Bad request\n");
                break;
            }

            /* check HTTP method */
            int is_supported =
                strcmp(request->http_method, "GET") == 0 ||
                strcmp(request->http_method, "HEAD") == 0 ||
                strcmp(request->http_method, "POST") == 0;

            if (!is_supported) {
                const char *not_impl = "HTTP/1.1 501 Not Implemented\r\n\r\n";
                send(client_sock, not_impl, strlen(not_impl), 0);
                fprintf(stdout, "Sent 501 Not Implemented for method %s\n", request->http_method);
                free(request->headers);
                free(request);
                break;
            }

            /* supported methods: echo back the original message in the body */
            char response[BUF_SIZE * 2];
            int body_len = readret;

            int header_len = snprintf(
                response,
                sizeof(response),
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: %d\r\n"
                "Content-Type: text/plain\r\n"
                "\r\n",
                body_len
            );

            if (header_len < 0 || header_len >= (int)sizeof(response)) {
                free(request->headers);
                free(request);
                break;
            }

            if (header_len + body_len > (int)sizeof(response)) {
                body_len = (int)sizeof(response) - header_len;
            }

            memcpy(response + header_len, buf, body_len);

            int total_len = header_len + body_len;
            if (send(client_sock, response, total_len, 0) < 0) {
                free(request->headers);
                free(request);
                break;
            }

            fprintf(stdout,"Echoed HTTP request back to client\n");

            free(request->headers);
            free(request);
        }
        /* client closes the connection. server free resources and listen again */
        if (close_socket(client_sock))
        {
            close_socket(sock);
            fprintf(stderr, "Error closing client socket.\n");
            return EXIT_FAILURE;
        }
        fprintf(stdout,"Closed connection from %s:%d\n",inet_ntoa(cli_addr.sin_addr),ntohs(cli_addr.sin_port));
    }
    close_socket(sock);
    return EXIT_SUCCESS;
}
