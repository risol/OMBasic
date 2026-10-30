#ifndef OMBASIC_RUNTIME_NETWORK_TLS_H
#define OMBASIC_RUNTIME_NETWORK_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Om_Tls_Session Om_Tls_Session;
typedef bool (*Om_Tls_Cancelled_Fn)(void *context);
typedef bool (*Om_Tls_Wait_Fn)(void *context, bool writable);

bool om_tls_client_start(intptr_t native_socket, const char *server_name,
    void *wait_context, Om_Tls_Cancelled_Fn is_cancelled,
    Om_Tls_Wait_Fn wait_socket, Om_Tls_Session **session,
    const char **error_code);
bool om_tls_send(Om_Tls_Session *session, const uint8_t *bytes,
    size_t length, void *wait_context, Om_Tls_Cancelled_Fn is_cancelled,
    Om_Tls_Wait_Fn wait_socket, size_t *sent, const char **error_code);
bool om_tls_receive(Om_Tls_Session *session, uint8_t *bytes,
    size_t capacity, void *wait_context, Om_Tls_Cancelled_Fn is_cancelled,
    Om_Tls_Wait_Fn wait_socket, size_t *received, const char **error_code);
void om_tls_close(Om_Tls_Session *session);

#endif
