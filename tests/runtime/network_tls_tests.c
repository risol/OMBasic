#define _POSIX_C_SOURCE 200809L

#include "network_tls.h"

#include <openssl/ssl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#define TLS_ASSERT(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "TLS assertion failed at %s:%d: %s\n", \
                      __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct Test_Server {
    int socket;
    SSL_CTX *context;
    const char *certificate_path;
    const char *key_path;
    bool accepted;
} Test_Server;

static void *test_server_run(void *opaque)
{
    Test_Server *server = opaque;
    server->context = SSL_CTX_new(TLS_server_method());
    if (server->context == NULL
        || SSL_CTX_use_certificate_file(server->context,
             server->certificate_path, SSL_FILETYPE_PEM) != 1
        || SSL_CTX_use_PrivateKey_file(server->context,
             server->key_path, SSL_FILETYPE_PEM) != 1) {
        return NULL;
    }
    SSL *ssl = SSL_new(server->context);
    if (ssl == NULL || SSL_set_fd(ssl, server->socket) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    server->accepted = SSL_accept(ssl) == 1;
    if (server->accepted) {
        char input[8] = {0};
        const int count = SSL_read(ssl, input, (int)sizeof(input));
        if (count == 5 && memcmp(input, "hello", 5) == 0) {
            (void)SSL_write(ssl, "world", 5);
        }
    }
    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
    return NULL;
}

static bool test_cancelled(void *context)
{
    (void)context;
    return false;
}

static bool test_wait_socket(void *context, bool writable)
{
    const int socket = *(const int *)context;
    for (;;) {
        struct pollfd descriptor = {
            .fd = socket,
            .events = writable ? POLLOUT : POLLIN,
        };
        const int ready = poll(&descriptor, 1, 100);
        if (ready > 0) return (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
        if (ready < 0) return false;
    }
}

static bool make_socket_pair(int sockets[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
    const int flags = fcntl(sockets[0], F_GETFL, 0);
    return flags >= 0 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool run_echo_case(const char *directory)
{
    char ca_path[1024];
    char certificate_path[1024];
    char key_path[1024];
    if (snprintf(ca_path, sizeof(ca_path), "%s/ca.pem", directory) < 0
        || snprintf(certificate_path, sizeof(certificate_path),
                    "%s/server.pem", directory) < 0
        || snprintf(key_path, sizeof(key_path), "%s/server.key", directory) < 0) {
        return false;
    }
    if (setenv("SSL_CERT_FILE", ca_path, 1) != 0) return false;

    int sockets[2] = {-1, -1};
    if (!make_socket_pair(sockets)) return false;
    Test_Server server = {
        .socket = sockets[1],
        .certificate_path = certificate_path,
        .key_path = key_path,
    };
    pthread_t thread;
    if (pthread_create(&thread, NULL, test_server_run, &server) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return false;
    }

    const char *error_code = NULL;
    Om_Tls_Session *session = NULL;
    TLS_ASSERT(om_tls_client_start((intptr_t)sockets[0], "localhost",
        &sockets[0], test_cancelled, test_wait_socket, &session,
        &error_code));
    TLS_ASSERT(session != NULL && error_code == NULL);
    size_t sent = 0;
    TLS_ASSERT(om_tls_send(session, (const uint8_t *)"hello", 5, &sockets[0],
        test_cancelled, test_wait_socket, &sent, &error_code));
    TLS_ASSERT(sent == 5);
    uint8_t received[8] = {0};
    size_t received_length = 0;
    TLS_ASSERT(om_tls_receive(session, received, sizeof(received), &sockets[0],
        test_cancelled, test_wait_socket, &received_length, &error_code));
    TLS_ASSERT(received_length == 5 && memcmp(received, "world", 5) == 0);
    om_tls_close(session);
    (void)pthread_join(thread, NULL);
    TLS_ASSERT(server.accepted);
    SSL_CTX_free(server.context);
    close(sockets[0]);
    close(sockets[1]);
    return true;
}

static bool run_hostname_rejection_case(const char *directory)
{
    char ca_path[1024];
    char certificate_path[1024];
    char key_path[1024];
    (void)snprintf(ca_path, sizeof(ca_path), "%s/ca.pem", directory);
    (void)snprintf(certificate_path, sizeof(certificate_path),
                   "%s/server.pem", directory);
    (void)snprintf(key_path, sizeof(key_path), "%s/server.key", directory);
    if (setenv("SSL_CERT_FILE", ca_path, 1) != 0) return false;
    int sockets[2] = {-1, -1};
    if (!make_socket_pair(sockets)) return false;
    Test_Server server = {
        .socket = sockets[1],
        .certificate_path = certificate_path,
        .key_path = key_path,
    };
    pthread_t thread;
    if (pthread_create(&thread, NULL, test_server_run, &server) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return false;
    }
    const char *error_code = NULL;
    Om_Tls_Session *session = NULL;
    const bool started = om_tls_client_start((intptr_t)sockets[0],
        "wrong.example", &sockets[0], test_cancelled, test_wait_socket,
        &session, &error_code);
    TLS_ASSERT(!started && session == NULL);
    TLS_ASSERT(error_code != NULL && strcmp(error_code, "hostname_mismatch") == 0);
    (void)pthread_join(thread, NULL);
    SSL_CTX_free(server.context);
    close(sockets[0]);
    close(sockets[1]);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!run_echo_case(argv[1])) return 1;
    if (!run_hostname_rejection_case(argv[1])) return 1;
    return 0;
}
