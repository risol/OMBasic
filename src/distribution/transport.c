#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ombasic/distribution/transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Om_Socket;
#define OM_INVALID_SOCKET INVALID_SOCKET
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int Om_Socket;
#define OM_INVALID_SOCKET (-1)
#endif

typedef struct Om_Distribution_Socket_State {
    Om_Socket socket;
    uint16_t local_port;
    uint8_t *receive_storage;
#if defined(_WIN32)
    bool winsock_started;
#endif
} Om_Distribution_Socket_State;

static void close_socket_handle(Om_Socket socket)
{
    if (socket == OM_INVALID_SOCKET) {
        return;
    }
#if defined(_WIN32)
    (void)closesocket(socket);
#else
    (void)close(socket);
#endif
}

static void interrupt_socket_handle(Om_Socket socket)
{
    if (socket == OM_INVALID_SOCKET) {
        return;
    }
#if defined(_WIN32)
    (void)shutdown(socket, SD_BOTH);
#else
    (void)shutdown(socket, SHUT_RDWR);
#endif
}

static void configure_no_sigpipe(Om_Socket socket)
{
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    const int enabled = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE,
                     &enabled, (socklen_t)sizeof(enabled));
#else
    (void)socket;
#endif
}

static Om_Status network_startup(Om_Distribution_Socket_State *state)
{
#if defined(_WIN32)
    WSADATA data = {0};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return OM_STATUS_IO_ERROR;
    }
    state->winsock_started = true;
#else
    (void)state;
#endif
    return OM_STATUS_OK;
}

static void network_cleanup(Om_Distribution_Socket_State *state)
{
#if defined(_WIN32)
    if (state->winsock_started) {
        (void)WSACleanup();
        state->winsock_started = false;
    }
#else
    (void)state;
#endif
}

static void socket_state_destroy(Om_Distribution_Socket_State *state)
{
    if (state == NULL) {
        return;
    }
    if (state->socket != OM_INVALID_SOCKET) {
        close_socket_handle(state->socket);
        state->socket = OM_INVALID_SOCKET;
    }
    network_cleanup(state);
    om_free(state->receive_storage);
    om_free(state);
}

static bool string_has_embedded_nul(Om_String_View text)
{
    for (size_t index = 0; index < text.length; ++index) {
        if (text.data[index] == '\0') {
            return true;
        }
    }
    return false;
}

static Om_Status copy_host(Om_String_View host, char **copy)
{
    if (copy == NULL || host.data == NULL || host.length == 0
        || string_has_embedded_nul(host)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *copy = NULL;
    size_t byte_count = 0;
    if (!om_checked_size_add(host.length, 1, &byte_count)) {
        return OM_STATUS_OVERFLOW;
    }
    char *buffer = om_alloc(byte_count);
    if (buffer == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(buffer, host.data, host.length);
    buffer[host.length] = '\0';
    *copy = buffer;
    return OM_STATUS_OK;
}

static void format_port(uint16_t port, char text[6])
{
    char reversed[5] = {0};
    size_t count = 0;
    do {
        reversed[count] = (char)('0' + (port % UINT16_C(10)));
        ++count;
        port = (uint16_t)(port / UINT16_C(10));
    } while (port != 0 && count < sizeof(reversed));
    for (size_t index = 0; index < count; ++index) {
        text[index] = reversed[count - index - 1];
    }
    text[count] = '\0';
}

static bool socket_option_reuse_address(Om_Socket socket)
{
    const int enabled = 1;
#if defined(_WIN32)
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                      (const char *)&enabled, (int)sizeof(enabled)) == 0;
#else
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                      &enabled, (socklen_t)sizeof(enabled)) == 0;
#endif
}

static Om_Status query_local_port(Om_Socket socket, uint16_t *port)
{
    struct sockaddr_storage address = {0};
#if defined(_WIN32)
    int address_length = (int)sizeof(address);
    if (getsockname(socket, (struct sockaddr *)&address,
                    &address_length) != 0) {
        return OM_STATUS_IO_ERROR;
    }
#else
    socklen_t address_length = (socklen_t)sizeof(address);
    if (getsockname(socket, (struct sockaddr *)&address,
                    &address_length) != 0) {
        return OM_STATUS_IO_ERROR;
    }
#endif
    if (address.ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)&address;
        *port = ntohs(ipv4->sin_port);
        return OM_STATUS_OK;
    }
    if (address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)&address;
        *port = ntohs(ipv6->sin6_port);
        return OM_STATUS_OK;
    }
    return OM_STATUS_IO_ERROR;
}

Om_Status om_distribution_listener_open(
    Om_Distribution_Listener *listener,
    Om_String_View bind_address,
    uint16_t port,
    int backlog)
{
    if (listener == NULL || listener->owned_state != NULL || backlog <= 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Socket_State *state = om_alloc(sizeof(*state));
    if (state == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *state = (Om_Distribution_Socket_State){.socket = OM_INVALID_SOCKET};
    Om_Status status = network_startup(state);
    char *host = NULL;
    if (status == OM_STATUS_OK) {
        status = copy_host(bind_address, &host);
    }
    if (status != OM_STATUS_OK) {
        socket_state_destroy(state);
        return status;
    }
    char service[6] = {0};
    format_port(port, service);
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0) {
        om_free(host);
        socket_state_destroy(state);
        return OM_STATUS_IO_ERROR;
    }
    om_free(host);
    status = OM_STATUS_IO_ERROR;
    for (struct addrinfo *address = addresses;
         address != NULL; address = address->ai_next) {
        state->socket = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);
        if (state->socket == OM_INVALID_SOCKET) {
            continue;
        }
        configure_no_sigpipe(state->socket);
        if (socket_option_reuse_address(state->socket)
            && bind(state->socket, address->ai_addr,
#if defined(_WIN32)
                    (int)address->ai_addrlen
#else
                    (socklen_t)address->ai_addrlen
#endif
                    ) == 0
            && listen(state->socket, backlog) == 0) {
            status = query_local_port(state->socket, &state->local_port);
            if (status == OM_STATUS_OK) {
                break;
            }
        }
        close_socket_handle(state->socket);
        state->socket = OM_INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (status != OM_STATUS_OK) {
        socket_state_destroy(state);
        return status;
    }
    listener->owned_state = state;
    return OM_STATUS_OK;
}

uint16_t om_distribution_listener_port(
    const Om_Distribution_Listener *listener)
{
    if (listener == NULL || listener->owned_state == NULL) {
        return 0;
    }
    const Om_Distribution_Socket_State *state = listener->owned_state;
    return state->local_port;
}

Om_Status om_distribution_listener_accept(
    const Om_Distribution_Listener *listener,
    Om_Distribution_Connection *connection)
{
    if (listener == NULL || listener->owned_state == NULL
        || connection == NULL || connection->owned_state != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Distribution_Socket_State *listener_state =
        listener->owned_state;
    Om_Distribution_Socket_State *state = om_alloc(sizeof(*state));
    if (state == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *state = (Om_Distribution_Socket_State){.socket = OM_INVALID_SOCKET};
    Om_Status status = network_startup(state);
    if (status != OM_STATUS_OK) {
        socket_state_destroy(state);
        return status;
    }
    state->socket = accept(listener_state->socket, NULL, NULL);
    if (state->socket == OM_INVALID_SOCKET) {
        socket_state_destroy(state);
        return OM_STATUS_IO_ERROR;
    }
    configure_no_sigpipe(state->socket);
    connection->owned_state = state;
    return OM_STATUS_OK;
}

Om_Status om_distribution_connection_connect(
    Om_Distribution_Connection *connection,
    Om_String_View host_view,
    uint16_t port)
{
    if (connection == NULL || connection->owned_state != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Socket_State *state = om_alloc(sizeof(*state));
    if (state == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *state = (Om_Distribution_Socket_State){.socket = OM_INVALID_SOCKET};
    Om_Status status = network_startup(state);
    char *host = NULL;
    if (status == OM_STATUS_OK) {
        status = copy_host(host_view, &host);
    }
    if (status != OM_STATUS_OK) {
        socket_state_destroy(state);
        return status;
    }
    char service[6] = {0};
    format_port(port, service);
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0) {
        om_free(host);
        socket_state_destroy(state);
        return OM_STATUS_IO_ERROR;
    }
    om_free(host);
    status = OM_STATUS_IO_ERROR;
    for (struct addrinfo *address = addresses;
         address != NULL; address = address->ai_next) {
        state->socket = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);
        if (state->socket == OM_INVALID_SOCKET) {
            continue;
        }
        configure_no_sigpipe(state->socket);
        if (connect(state->socket, address->ai_addr,
#if defined(_WIN32)
                    (int)address->ai_addrlen
#else
                    (socklen_t)address->ai_addrlen
#endif
                    ) == 0) {
            status = OM_STATUS_OK;
            break;
        }
        close_socket_handle(state->socket);
        state->socket = OM_INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (status != OM_STATUS_OK) {
        socket_state_destroy(state);
        return status;
    }
    connection->owned_state = state;
    return OM_STATUS_OK;
}

static Om_Status send_all(Om_Socket socket,
                          const uint8_t *bytes,
                          size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        size_t remaining = length - sent;
        if (remaining > (size_t)INT32_MAX) {
            remaining = (size_t)INT32_MAX;
        }
#if defined(_WIN32)
        const int count = send(socket, (const char *)bytes + sent,
                               (int)remaining, 0);
        if (count == SOCKET_ERROR) {
            return OM_STATUS_IO_ERROR;
        }
#else
#if defined(MSG_NOSIGNAL)
        const ssize_t count = send(socket, bytes + sent, remaining,
                                   MSG_NOSIGNAL);
#else
        const ssize_t count = send(socket, bytes + sent, remaining, 0);
#endif
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return OM_STATUS_IO_ERROR;
        }
#endif
        if (count == 0) {
            return OM_STATUS_IO_ERROR;
        }
        sent += (size_t)count;
    }
    return OM_STATUS_OK;
}

static Om_Status receive_exact(Om_Socket socket,
                               uint8_t *bytes,
                               size_t length,
                               bool clean_eof_allowed)
{
    size_t received = 0;
    while (received < length) {
        size_t remaining = length - received;
        if (remaining > (size_t)INT32_MAX) {
            remaining = (size_t)INT32_MAX;
        }
#if defined(_WIN32)
        const int count = recv(socket, (char *)bytes + received,
                               (int)remaining, 0);
        if (count == SOCKET_ERROR) {
            return OM_STATUS_IO_ERROR;
        }
#else
        const ssize_t count = recv(socket, bytes + received, remaining, 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return OM_STATUS_IO_ERROR;
        }
#endif
        if (count == 0) {
            return received == 0 && clean_eof_allowed
                ? OM_STATUS_END_OF_INPUT : OM_STATUS_IO_ERROR;
        }
        received += (size_t)count;
    }
    return OM_STATUS_OK;
}

Om_Status om_distribution_connection_send(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Frame *frame,
    size_t maximum_frame_bytes)
{
    if (connection == NULL || connection->owned_state == NULL
        || frame == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (frame->payload.length > UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    size_t frame_size = 0;
    if (frame->payload.length > SIZE_MAX - OM_DISTRIBUTION_FRAME_HEADER_SIZE) {
        return OM_STATUS_OVERFLOW;
    }
    frame_size = OM_DISTRIBUTION_FRAME_HEADER_SIZE + frame->payload.length;
    const size_t maximum = maximum_frame_bytes == 0
        ? OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES : maximum_frame_bytes;
    if (frame_size > maximum) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    uint8_t *encoded = om_alloc(frame_size);
    if (encoded == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t written = 0;
    Om_Status status = om_distribution_frame_encode(
        frame, (Om_Byte_Slice){encoded, frame_size}, maximum, &written);
    if (status == OM_STATUS_OK) {
        Om_Distribution_Socket_State *state = connection->owned_state;
        status = send_all(state->socket, encoded, written);
        if (status != OM_STATUS_OK) {
            interrupt_socket_handle(state->socket);
        }
    }
    om_free(encoded);
    return status;
}

Om_Status om_distribution_connection_receive(
    Om_Distribution_Connection *connection,
    size_t maximum_frame_bytes,
    Om_Distribution_Frame *frame)
{
    if (connection == NULL || connection->owned_state == NULL
        || frame == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Socket_State *state = connection->owned_state;
    om_free(state->receive_storage);
    state->receive_storage = NULL;
    uint8_t header[OM_DISTRIBUTION_FRAME_HEADER_SIZE] = {0};
    Om_Status status = receive_exact(state->socket, header, sizeof(header), true);
    if (status != OM_STATUS_OK) {
        *frame = (Om_Distribution_Frame){0};
        interrupt_socket_handle(state->socket);
        return status;
    }
    size_t frame_size = 0;
    const size_t maximum = maximum_frame_bytes == 0
        ? OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES : maximum_frame_bytes;
    status = om_distribution_frame_size_from_header(
        (Om_Const_Byte_Slice){header, sizeof(header)}, maximum, &frame_size);
    if (status != OM_STATUS_OK) {
        interrupt_socket_handle(state->socket);
        *frame = (Om_Distribution_Frame){0};
        return status;
    }
    state->receive_storage = om_alloc(frame_size);
    if (state->receive_storage == NULL) {
        interrupt_socket_handle(state->socket);
        *frame = (Om_Distribution_Frame){0};
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(state->receive_storage, header, sizeof(header));
    if (frame_size > sizeof(header)) {
        status = receive_exact(state->socket,
            state->receive_storage + sizeof(header), frame_size - sizeof(header),
            false);
    }
    if (status == OM_STATUS_OK) {
        size_t consumed = 0;
        status = om_distribution_frame_decode(
            (Om_Const_Byte_Slice){state->receive_storage, frame_size},
            maximum, frame, &consumed);
        if (status == OM_STATUS_OK && consumed != frame_size) {
            status = OM_STATUS_IO_ERROR;
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(state->receive_storage);
        state->receive_storage = NULL;
        interrupt_socket_handle(state->socket);
        *frame = (Om_Distribution_Frame){0};
    }
    return status;
}

void om_distribution_connection_close(
    Om_Distribution_Connection *connection)
{
    if (connection == NULL || connection->owned_state == NULL) {
        return;
    }
    socket_state_destroy(connection->owned_state);
    connection->owned_state = NULL;
}

void om_distribution_connection_interrupt(
    Om_Distribution_Connection *connection)
{
    if (connection == NULL || connection->owned_state == NULL) {
        return;
    }
    const Om_Distribution_Socket_State *state = connection->owned_state;
    if (state->socket == OM_INVALID_SOCKET) {
        return;
    }
    interrupt_socket_handle(state->socket);
}

void om_distribution_listener_close(Om_Distribution_Listener *listener)
{
    if (listener == NULL || listener->owned_state == NULL) {
        return;
    }
    socket_state_destroy(listener->owned_state);
    listener->owned_state = NULL;
}
