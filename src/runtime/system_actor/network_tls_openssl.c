#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "network_tls.h"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <stdlib.h>

struct Om_Tls_Session {
    SSL_CTX *context;
    SSL *ssl;
};

static const char *verify_error_code(long verify_result)
{
    return verify_result == X509_V_ERR_HOSTNAME_MISMATCH
            || verify_result == X509_V_ERR_IP_ADDRESS_MISMATCH
        ? "hostname_mismatch" : "certificate_invalid";
}

static bool cancelled(void *context, Om_Tls_Cancelled_Fn is_cancelled,
                      const char **error_code)
{
    if (is_cancelled != NULL && is_cancelled(context)) {
        if (error_code != NULL) *error_code = "closed";
        return true;
    }
    return false;
}

static bool wait_for_ssl(int ssl_error, void *context,
                         Om_Tls_Cancelled_Fn is_cancelled,
                         Om_Tls_Wait_Fn wait_socket,
                         const char **error_code)
{
    const bool writable = ssl_error == SSL_ERROR_WANT_WRITE;
    if (ssl_error != SSL_ERROR_WANT_READ && !writable) {
        if (error_code != NULL) *error_code = "handshake_failed";
        return false;
    }
    while (!cancelled(context, is_cancelled, error_code)) {
        if (wait_socket == NULL || !wait_socket(context, writable)) {
            if (error_code != NULL && *error_code == NULL) {
                *error_code = "closed";
            }
            return false;
        }
        return true;
    }
    return false;
}

bool om_tls_client_start(intptr_t native_socket, const char *server_name,
    void *wait_context, Om_Tls_Cancelled_Fn is_cancelled,
    Om_Tls_Wait_Fn wait_socket, Om_Tls_Session **session,
    const char **error_code)
{
    if (session != NULL) *session = NULL;
    if (error_code != NULL) *error_code = "handshake_failed";
    if (server_name == NULL || server_name[0] == '\0' || session == NULL
        || wait_socket == NULL) {
        if (error_code != NULL) *error_code = "invalid_argument";
        return false;
    }

    Om_Tls_Session *created = calloc(1, sizeof(*created));
    if (created == NULL) {
        if (error_code != NULL) *error_code = "resource_exhausted";
        return false;
    }
    created->context = SSL_CTX_new(TLS_client_method());
    if (created->context == NULL) goto failed;
    if (SSL_CTX_set_min_proto_version(created->context, TLS1_2_VERSION) != 1) {
        goto failed;
    }
    SSL_CTX_set_verify(created->context, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(created->context) != 1) {
        if (error_code != NULL) *error_code = "unsupported";
        goto failed;
    }
    created->ssl = SSL_new(created->context);
    if (created->ssl == NULL) goto failed;
    if (SSL_set_fd(created->ssl, (int)native_socket) != 1) goto failed;

    struct in_addr ipv4 = {0};
    struct in6_addr ipv6 = {0};
    const bool ip_literal = inet_pton(AF_INET, server_name, &ipv4) == 1
        || inet_pton(AF_INET6, server_name, &ipv6) == 1;
    if (ip_literal) {
        X509_VERIFY_PARAM *verify = SSL_get0_param(created->ssl);
        if (verify == NULL
            || X509_VERIFY_PARAM_set1_ip_asc(verify, server_name) != 1) {
            goto failed;
        }
    } else {
        if (SSL_set_tlsext_host_name(created->ssl, server_name) != 1
            || SSL_set1_host(created->ssl, server_name) != 1) {
            goto failed;
        }
    }

    while (!cancelled(wait_context, is_cancelled, error_code)) {
        ERR_clear_error();
        const int result = SSL_connect(created->ssl);
        if (result == 1) break;
        const int ssl_error = SSL_get_error(created->ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!wait_for_ssl(ssl_error, wait_context,
                              is_cancelled, wait_socket, error_code)) {
                goto failed;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            if (error_code != NULL) *error_code = "closed";
        } else if (ssl_error == SSL_ERROR_SYSCALL) {
            if (error_code != NULL) *error_code = "io_error";
        } else {
            const long verify_result = SSL_get_verify_result(created->ssl);
            if (verify_result != X509_V_OK && error_code != NULL) {
                *error_code = verify_error_code(verify_result);
            } else if (error_code != NULL) {
                *error_code = "handshake_failed";
            }
        }
        goto failed;
    }
    if (cancelled(wait_context, is_cancelled, error_code)) goto failed;
    const long verify_result = SSL_get_verify_result(created->ssl);
    if (verify_result != X509_V_OK) {
        if (error_code != NULL) *error_code = verify_error_code(verify_result);
        goto failed;
    }
    *session = created;
    if (error_code != NULL) *error_code = NULL;
    return true;

failed:
    om_tls_close(created);
    return false;
}

bool om_tls_send(Om_Tls_Session *session, const uint8_t *bytes,
    size_t length, void *wait_context, Om_Tls_Cancelled_Fn is_cancelled,
    Om_Tls_Wait_Fn wait_socket, size_t *sent, const char **error_code)
{
    if (sent != NULL) *sent = 0;
    if (error_code != NULL) *error_code = "io_error";
    if (session == NULL || session->ssl == NULL || (length != 0 && bytes == NULL)
        || sent == NULL || wait_socket == NULL) {
        if (error_code != NULL) *error_code = "invalid_argument";
        return false;
    }
    while (!cancelled(wait_context, is_cancelled, error_code)) {
        size_t written = 0;
        const int result = SSL_write_ex(session->ssl, bytes, length, &written);
        if (result == 1) {
            *sent = written;
            if (error_code != NULL) *error_code = NULL;
            return true;
        }
        const int ssl_error = SSL_get_error(session->ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            if (wait_for_ssl(ssl_error, wait_context,
                             is_cancelled, wait_socket, error_code)) continue;
            return false;
        }
        if (error_code != NULL) {
            *error_code = ssl_error == SSL_ERROR_ZERO_RETURN ? "closed"
                : ssl_error == SSL_ERROR_SYSCALL ? "io_error" : "protocol_error";
        }
        return false;
    }
    if (error_code != NULL) *error_code = "closed";
    return false;
}

bool om_tls_receive(Om_Tls_Session *session, uint8_t *bytes,
    size_t capacity, void *wait_context, Om_Tls_Cancelled_Fn is_cancelled,
    Om_Tls_Wait_Fn wait_socket, size_t *received, const char **error_code)
{
    if (received != NULL) *received = 0;
    if (error_code != NULL) *error_code = "io_error";
    if (session == NULL || session->ssl == NULL || (capacity != 0 && bytes == NULL)
        || received == NULL || wait_socket == NULL) {
        if (error_code != NULL) *error_code = "invalid_argument";
        return false;
    }
    if (capacity == 0) {
        if (error_code != NULL) *error_code = NULL;
        return true;
    }
    while (!cancelled(wait_context, is_cancelled, error_code)) {
        size_t count = 0;
        const int result = SSL_read_ex(session->ssl, bytes, capacity, &count);
        if (result == 1) {
            *received = count;
            if (error_code != NULL) *error_code = NULL;
            return true;
        }
        const int ssl_error = SSL_get_error(session->ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            if (wait_for_ssl(ssl_error, wait_context,
                             is_cancelled, wait_socket, error_code)) continue;
            return false;
        }
        if (error_code != NULL) {
            *error_code = ssl_error == SSL_ERROR_ZERO_RETURN ? "end_of_stream"
                : ssl_error == SSL_ERROR_SYSCALL ? "io_error" : "protocol_error";
        }
        return false;
    }
    if (error_code != NULL) *error_code = "closed";
    return false;
}

void om_tls_close(Om_Tls_Session *session)
{
    if (session == NULL) return;
    if (session->ssl != NULL) {
        (void)SSL_shutdown(session->ssl);
        SSL_free(session->ssl);
    }
    if (session->context != NULL) SSL_CTX_free(session->context);
    free(session);
}
