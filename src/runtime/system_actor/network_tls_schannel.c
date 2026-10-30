#include "network_tls.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct Om_Tls_Session {
    CredHandle credentials;
    CtxtHandle context;
    bool has_credentials;
    bool has_context;
    SOCKET socket;
    SecPkgContext_StreamSizes sizes;
    uint8_t *encrypted;
    size_t encrypted_length;
    size_t encrypted_capacity;
    uint8_t *plaintext;
    size_t plaintext_length;
    size_t plaintext_offset;
};

static bool wait_cancelled(void *context, Om_Tls_Cancelled_Fn cancelled,
                           const char **error_code)
{
    if (cancelled != NULL && cancelled(context)) {
        if (error_code != NULL) *error_code = "closed";
        return true;
    }
    return false;
}

static bool grow_buffer(uint8_t **buffer, size_t *capacity, size_t needed)
{
    if (needed <= *capacity) return true;
    size_t next = *capacity == 0 ? 16384 : *capacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2) return false;
        next *= 2;
    }
    uint8_t *resized = realloc(*buffer, next);
    if (resized == NULL) return false;
    *buffer = resized;
    *capacity = next;
    return true;
}

static bool send_all(SOCKET socket, const uint8_t *bytes, size_t length,
    void *wait_context, Om_Tls_Cancelled_Fn cancelled,
    Om_Tls_Wait_Fn wait_socket, const char **error_code)
{
    size_t offset = 0;
    while (offset < length) {
        if (wait_cancelled(wait_context, cancelled, error_code)) return false;
        const size_t remaining = length - offset;
        const int chunk = remaining > (size_t)INT_MAX
            ? INT_MAX : (int)remaining;
        const int result = send(socket, (const char *)bytes + offset, chunk, 0);
        if (result > 0) {
            offset += (size_t)result;
            continue;
        }
        const int error = WSAGetLastError();
        if (result == SOCKET_ERROR
            && (error == WSAEWOULDBLOCK || error == WSAEINTR)) {
            if (wait_socket == NULL || !wait_socket(wait_context, true)) {
                if (error_code != NULL) *error_code = "closed";
                return false;
            }
            continue;
        }
        if (error_code != NULL) *error_code = "io_error";
        return false;
    }
    return true;
}

static bool receive_more(Om_Tls_Session *session, SOCKET socket,
    void *wait_context, Om_Tls_Cancelled_Fn cancelled,
    Om_Tls_Wait_Fn wait_socket, const char **error_code)
{
    if (session->encrypted_length > (size_t)(64 * 1024 * 1024) - 16384
        || !grow_buffer(&session->encrypted, &session->encrypted_capacity,
                        session->encrypted_length + 16384)) {
        if (error_code != NULL) *error_code = "resource_exhausted";
        return false;
    }
    for (;;) {
        if (wait_cancelled(wait_context, cancelled, error_code)) return false;
        if (wait_socket == NULL || !wait_socket(wait_context, false)) {
            if (error_code != NULL) *error_code = "closed";
            return false;
        }
        const int result = recv(socket,
            (char *)session->encrypted + session->encrypted_length, 16384, 0);
        if (result > 0) {
            session->encrypted_length += (size_t)result;
            return true;
        }
        if (result == 0) {
            if (error_code != NULL) *error_code = "end_of_stream";
            return false;
        }
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINTR) continue;
        if (error_code != NULL) *error_code = "io_error";
        return false;
    }
}

static bool send_handshake_token(SecBuffer *token, SOCKET socket,
    void *wait_context, Om_Tls_Cancelled_Fn cancelled,
    Om_Tls_Wait_Fn wait_socket, const char **error_code)
{
    if (token->cbBuffer == 0) return true;
    return send_all(socket, token->pvBuffer, token->cbBuffer, wait_context,
                    cancelled, wait_socket, error_code);
}

static const char *handshake_error_code(SECURITY_STATUS status)
{
    if (status == SEC_E_WRONG_PRINCIPAL) return "hostname_mismatch";
    if (status == SEC_E_UNTRUSTED_ROOT || status == SEC_E_CERT_EXPIRED
        || status == SEC_E_CERT_UNKNOWN) {
        return "certificate_invalid";
    }
    return "handshake_failed";
}

bool om_tls_client_start(intptr_t native_socket, const char *server_name,
    void *wait_context, Om_Tls_Cancelled_Fn cancelled,
    Om_Tls_Wait_Fn wait_socket, Om_Tls_Session **session,
    const char **error_code)
{
    if (session != NULL) *session = NULL;
    if (error_code != NULL) *error_code = "handshake_failed";
    if (session == NULL || server_name == NULL || server_name[0] == '\0'
        || wait_socket == NULL) {
        if (error_code != NULL) *error_code = "invalid_argument";
        return false;
    }

    Om_Tls_Session *created = calloc(1, sizeof(*created));
    if (created == NULL) {
        if (error_code != NULL) *error_code = "resource_exhausted";
        return false;
    }
    SCHANNEL_CRED credentials = {0};
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION
        | SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp expiry = {0};
    SECURITY_STATUS security_status = AcquireCredentialsHandleA(NULL,
        UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &credentials, NULL, NULL,
        &created->credentials, &expiry);
    if (security_status != SEC_E_OK) goto failed;
    created->has_credentials = true;
    created->socket = (SOCKET)native_socket;

    const ULONG request_flags = ISC_REQ_SEQUENCE_DETECT
        | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
        | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    ULONG attributes = 0;
    SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
    bool first_call = true;
    const SOCKET socket = created->socket;
    while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE) {
        if (wait_cancelled(wait_context, cancelled, error_code)) goto failed;
        SecBuffer input_buffers[2] = {0};
        SecBufferDesc input = {SECBUFFER_VERSION, 2, input_buffers};
        if (created->encrypted_length != 0) {
            input_buffers[0].BufferType = SECBUFFER_TOKEN;
            input_buffers[0].pvBuffer = created->encrypted;
            input_buffers[0].cbBuffer = (ULONG)created->encrypted_length;
            input_buffers[1].BufferType = SECBUFFER_EMPTY;
        }
        SecBuffer output_buffer = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc output = {SECBUFFER_VERSION, 1, &output_buffer};
        status = InitializeSecurityContextA(&created->credentials,
            first_call ? NULL : &created->context, (SEC_CHAR *)server_name,
            request_flags, 0, SECURITY_NATIVE_DREP,
            created->encrypted_length != 0 ? &input : NULL, 0,
            &created->context, &output, &attributes, &expiry);
        first_call = false;
        created->has_context = true;
        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED
            || status == SEC_E_INCOMPLETE_MESSAGE) {
            if (!send_handshake_token(&output_buffer, socket, wait_context,
                                      cancelled, wait_socket, error_code)) {
                if (output_buffer.pvBuffer != NULL) {
                    FreeContextBuffer(output_buffer.pvBuffer);
                }
                goto failed;
            }
            if (output_buffer.pvBuffer != NULL) {
                FreeContextBuffer(output_buffer.pvBuffer);
            }
        } else {
            if (output_buffer.pvBuffer != NULL) {
                FreeContextBuffer(output_buffer.pvBuffer);
            }
            if (error_code != NULL) *error_code = handshake_error_code(status);
            goto failed;
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (!receive_more(created, socket, wait_context, cancelled,
                              wait_socket, error_code)) goto failed;
            status = SEC_I_CONTINUE_NEEDED;
            continue;
        }
        bool has_extra = false;
        for (size_t index = 0; index < 2; ++index) {
            if (input_buffers[index].BufferType == SECBUFFER_EXTRA) {
                const size_t extra_length = input_buffers[index].cbBuffer;
                if (extra_length <= created->encrypted_length) {
                    memmove(created->encrypted,
                        created->encrypted + created->encrypted_length
                            - extra_length,
                        extra_length);
                    created->encrypted_length = extra_length;
                    has_extra = true;
                }
            }
        }
        if (!has_extra) created->encrypted_length = 0;
        if (status == SEC_E_OK) break;
        if (created->encrypted_length == 0
            && !receive_more(created, socket, wait_context, cancelled,
                             wait_socket, error_code)) goto failed;
    }
    security_status = QueryContextAttributesA(&created->context,
        SECPKG_ATTR_STREAM_SIZES, &created->sizes);
    if (security_status != SEC_E_OK) goto failed;
    /* Preserve any application data Schannel received with the final token. */
    if (created->encrypted_length != 0) {
        SecBuffer input_buffers[2] = {0};
        SecBufferDesc input = {SECBUFFER_VERSION, 2, input_buffers};
        input_buffers[0].BufferType = SECBUFFER_DATA;
        input_buffers[0].pvBuffer = created->encrypted;
        input_buffers[0].cbBuffer = (ULONG)created->encrypted_length;
        input_buffers[1].BufferType = SECBUFFER_EMPTY;
        if (DecryptMessage(&created->context, &input, 0, NULL) == SEC_E_OK) {
            bool has_extra = false;
            for (size_t index = 0; index < 2; ++index) {
                if (input_buffers[index].BufferType == SECBUFFER_DATA
                    && input_buffers[index].cbBuffer != 0) {
                    created->plaintext = malloc(input_buffers[index].cbBuffer);
                    if (created->plaintext == NULL) {
                        if (error_code != NULL) *error_code = "resource_exhausted";
                        goto failed;
                    }
                    memcpy(created->plaintext, input_buffers[index].pvBuffer,
                           input_buffers[index].cbBuffer);
                    created->plaintext_length = input_buffers[index].cbBuffer;
                }
                if (input_buffers[index].BufferType == SECBUFFER_EXTRA) {
                    const size_t extra = input_buffers[index].cbBuffer;
                    if (extra <= created->encrypted_length) {
                        memmove(created->encrypted,
                            created->encrypted + created->encrypted_length - extra,
                            extra);
                        created->encrypted_length = extra;
                        has_extra = true;
                    }
                }
            }
            if (!has_extra) created->encrypted_length = 0;
        }
    }
    *session = created;
    if (error_code != NULL) *error_code = NULL;
    return true;

failed:
    if (error_code != NULL && *error_code == NULL) {
        *error_code = handshake_error_code(security_status);
    }
    om_tls_close(created);
    return false;
}

bool om_tls_send(Om_Tls_Session *session, const uint8_t *bytes,
    size_t length, void *wait_context, Om_Tls_Cancelled_Fn cancelled,
    Om_Tls_Wait_Fn wait_socket, size_t *sent, const char **error_code)
{
    if (sent != NULL) *sent = 0;
    if (error_code != NULL) *error_code = "io_error";
    if (session == NULL || (length != 0 && bytes == NULL) || sent == NULL) {
        if (error_code != NULL) *error_code = "invalid_argument";
        return false;
    }
    if (length == 0) {
        if (error_code != NULL) *error_code = NULL;
        return true;
    }
    const size_t chunk = length < session->sizes.cbMaximumMessage
        ? length : session->sizes.cbMaximumMessage;
    const size_t total = (size_t)session->sizes.cbHeader + chunk
        + (size_t)session->sizes.cbTrailer;
    uint8_t *encrypted = malloc(total);
    if (encrypted == NULL) {
        if (error_code != NULL) *error_code = "resource_exhausted";
        return false;
    }
    (void)memcpy(encrypted + session->sizes.cbHeader, bytes, chunk);
    SecBuffer buffers[4] = {
        {session->sizes.cbHeader, SECBUFFER_STREAM_HEADER, encrypted},
        {(ULONG)chunk, SECBUFFER_DATA, encrypted + session->sizes.cbHeader},
        {session->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
            encrypted + session->sizes.cbHeader + chunk},
        {0, SECBUFFER_EMPTY, NULL},
    };
    SecBufferDesc message = {SECBUFFER_VERSION, 4, buffers};
    const SECURITY_STATUS status = EncryptMessage(&session->context, 0,
                                                   &message, 0);
    if (status != SEC_E_OK) {
        free(encrypted);
        if (error_code != NULL) *error_code = "protocol_error";
        return false;
    }
    bool succeeded = true;
    for (size_t index = 0; index < 3; ++index) {
        if (buffers[index].cbBuffer != 0
            && !send_all(session->socket, buffers[index].pvBuffer,
                buffers[index].cbBuffer, wait_context, cancelled,
                wait_socket, error_code)) {
            succeeded = false;
            break;
        }
    }
    if (succeeded) *sent = chunk;
    free(encrypted);
    if (succeeded && error_code != NULL) *error_code = NULL;
    return succeeded;
}

bool om_tls_receive(Om_Tls_Session *session, uint8_t *bytes,
    size_t capacity, void *wait_context, Om_Tls_Cancelled_Fn cancelled,
    Om_Tls_Wait_Fn wait_socket, size_t *received, const char **error_code)
{
    if (received != NULL) *received = 0;
    if (session == NULL || (capacity != 0 && bytes == NULL)
        || received == NULL) {
        if (error_code != NULL) *error_code = "invalid_argument";
        return false;
    }
    if (capacity == 0) {
        if (error_code != NULL) *error_code = NULL;
        return true;
    }
    for (;;) {
        if (session->plaintext_offset < session->plaintext_length) {
            const size_t available = session->plaintext_length
                - session->plaintext_offset;
            const size_t copied = available < capacity ? available : capacity;
            memcpy(bytes, session->plaintext + session->plaintext_offset, copied);
            session->plaintext_offset += copied;
            *received = copied;
            if (session->plaintext_offset == session->plaintext_length) {
                free(session->plaintext);
                session->plaintext = NULL;
                session->plaintext_length = 0;
                session->plaintext_offset = 0;
            }
            if (error_code != NULL) *error_code = NULL;
            return true;
        }
        SecBuffer buffers[4] = {0};
        SecBufferDesc message = {SECBUFFER_VERSION, 4, buffers};
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = session->encrypted;
        buffers[0].cbBuffer = (ULONG)session->encrypted_length;
        for (size_t index = 1; index < 4; ++index) {
            buffers[index].BufferType = SECBUFFER_EMPTY;
        }
        SECURITY_STATUS status = session->encrypted_length == 0
            ? SEC_E_INCOMPLETE_MESSAGE
            : DecryptMessage(&session->context, &message, 0, NULL);
        if (status == SEC_E_OK) {
            bool has_extra = false;
            for (size_t index = 0; index < 4; ++index) {
                if (buffers[index].BufferType == SECBUFFER_DATA
                    && buffers[index].cbBuffer != 0) {
                    uint8_t *copy = malloc(buffers[index].cbBuffer);
                    if (copy == NULL) {
                        if (error_code != NULL) *error_code = "resource_exhausted";
                        return false;
                    }
                    memcpy(copy, buffers[index].pvBuffer, buffers[index].cbBuffer);
                    free(session->plaintext);
                    session->plaintext = copy;
                    session->plaintext_length = buffers[index].cbBuffer;
                    session->plaintext_offset = 0;
                }
                if (buffers[index].BufferType == SECBUFFER_EXTRA) {
                    const size_t extra = buffers[index].cbBuffer;
                    if (extra <= session->encrypted_length) {
                        memmove(session->encrypted,
                            session->encrypted + session->encrypted_length - extra,
                            extra);
                        session->encrypted_length = extra;
                        has_extra = true;
                    }
                }
            }
            if (!has_extra) session->encrypted_length = 0;
            continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) {
            if (error_code != NULL) *error_code = "end_of_stream";
            return false;
        }
        if (status != SEC_E_INCOMPLETE_MESSAGE) {
            if (error_code != NULL) *error_code = "protocol_error";
            return false;
        }
        if (!receive_more(session, session->socket, wait_context, cancelled,
                          wait_socket, error_code)) return false;
    }
}

void om_tls_close(Om_Tls_Session *session)
{
    if (session == NULL) return;
    if (session->has_context) {
        DWORD shutdown_token = SCHANNEL_SHUTDOWN;
        SecBuffer buffer = {sizeof(shutdown_token), SECBUFFER_TOKEN,
                            &shutdown_token};
        SecBufferDesc descriptor = {SECBUFFER_VERSION, 1, &buffer};
        if (ApplyControlToken(&session->context, &descriptor) == SEC_E_OK) {
            SecBuffer output_buffer = {0, SECBUFFER_TOKEN, NULL};
            SecBufferDesc output = {SECBUFFER_VERSION, 1, &output_buffer};
            ULONG attributes = 0;
            TimeStamp expiry = {0};
            if (InitializeSecurityContextA(&session->credentials,
                    &session->context, NULL, 0, 0, SECURITY_NATIVE_DREP,
                    NULL, 0, &session->context, &output, &attributes,
                    &expiry) == SEC_E_OK && output_buffer.pvBuffer != NULL) {
                (void)send(session->socket, output_buffer.pvBuffer,
                           (int)output_buffer.cbBuffer, 0);
                FreeContextBuffer(output_buffer.pvBuffer);
            }
        }
    }
    if (session->has_context) DeleteSecurityContext(&session->context);
    if (session->has_credentials) FreeCredentialsHandle(&session->credentials);
    free(session->encrypted);
    free(session->plaintext);
    free(session);
}
