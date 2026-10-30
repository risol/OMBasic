#if !defined(_WIN32)
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define OM_NATIVE_SOCKET(value) ((SOCKET)(value))
#define OM_SOCKET_LENGTH(value) ((int)(value))
#endif

#include "actor_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#else
#include <bcrypt.h>
#endif

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"
#include "network_tls.h"

#define OM_SYSTEM_FILE_MAX_TEXT_BYTES ((size_t)64 * 1024 * 1024)
#define OM_SYSTEM_RANDOM_MAX_BYTES ((size_t)64 * 1024 * 1024)
#define OM_SYSTEM_DIRECTORY_MAX_ENTRIES ((size_t)10000)
#define OM_SYSTEM_NETWORK_MAX_RESOURCES ((size_t)1024)
#define OM_SYSTEM_NETWORK_MAX_BUFFER ((size_t)64 * 1024 * 1024)
#define OM_SYSTEM_PROCESS_MAX_RESOURCES ((size_t)64)
#define OM_SYSTEM_PROCESS_MAX_ARGUMENTS ((size_t)4096)
#define OM_SYSTEM_PROCESS_MAX_ARGUMENT_BYTES ((size_t)1024 * 1024)
#define OM_SYSTEM_PROCESS_MAX_IO_BYTES ((size_t)1024 * 1024)
#define OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES ((size_t)1024 * 1024)

struct Om_Runtime_Network_Resource {
    uint64_t id;
    intptr_t native_socket;
    bool listener;
    bool udp;
    bool closed;
    bool native_closed;
    bool busy_read;
    bool busy_write;
    bool busy_tls;
    Om_Tls_Session *tls_session;
    struct Om_Runtime_Network_Resource *next;
};

struct Om_Runtime_Process_Resource {
    uint64_t id;
    intptr_t native_process;
    intptr_t process_guard;
    intptr_t stdin_handle;
    intptr_t stdout_handle;
    intptr_t stderr_handle;
    bool closed;
    bool stdin_closed;
    bool busy_stdin;
    bool busy_stdout;
    bool busy_stderr;
    bool busy_wait;
    bool exited;
    int64_t exit_code;
    struct Om_Runtime_Process_Resource *next;
};

typedef intptr_t Om_Native_Socket;

#define OM_INVALID_SOCKET_VALUE ((Om_Native_Socket)-1)
#if !defined(_WIN32)
#define OM_NATIVE_SOCKET(value) ((int)(value))
#define OM_SOCKET_LENGTH(value) ((socklen_t)(value))
#endif

static Om_Status system_task_run(void *context);
static void system_file_complete(void *context, Om_Status task_status);
static void system_task_destroy(void *context);
static Om_Status system_task_add_text_item(Om_Runtime_System_Task *task,
                                           const char *bytes,
                                           size_t length);

static Om_Native_Socket network_native_socket(intptr_t value)
{
#if defined(_WIN32)
    return (Om_Native_Socket)OM_NATIVE_SOCKET(value);
#else
    return (Om_Native_Socket)value;
#endif
}

static void network_close_native(Om_Native_Socket socket_value)
{
    if (socket_value == OM_INVALID_SOCKET_VALUE) return;
#if defined(_WIN32)
    (void)shutdown(OM_NATIVE_SOCKET(socket_value), SD_BOTH);
    (void)closesocket(OM_NATIVE_SOCKET(socket_value));
#else
    (void)shutdown((int)socket_value, SHUT_RDWR);
    (void)close((int)socket_value);
#endif
}

static void network_shutdown_native(Om_Native_Socket socket_value)
{
    if (socket_value == OM_INVALID_SOCKET_VALUE) return;
#if defined(_WIN32)
    (void)shutdown((SOCKET)socket_value, SD_BOTH);
#else
    (void)shutdown((int)socket_value, SHUT_RDWR);
#endif
}

static bool network_is_cancelled(Om_Runtime_System_Task *task)
{
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);
    const bool cancelled = system->shutting_down
        || (task->network_resource != NULL && task->network_resource->closed)
        || (task->network_listener != NULL && task->network_listener->closed);
    actor_mutex_unlock(system);
    return cancelled;
}

static bool network_wait_socket(Om_Runtime_System_Task *task,
                                Om_Native_Socket socket_value,
                                bool writable)
{
    for (;;) {
        if (network_is_cancelled(task)) return false;
#if defined(_WIN32)
        fd_set sockets;
        FD_ZERO(&sockets);
        FD_SET(OM_NATIVE_SOCKET(socket_value), &sockets);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        const int ready = select(0, writable ? NULL : &sockets,
            writable ? &sockets : NULL, NULL, &timeout);
#else
        struct pollfd descriptor = {
            .fd = (int)socket_value,
            .events = writable ? POLLOUT : POLLIN,
        };
        const int ready = poll(&descriptor, 1, 100);
#endif
        if (ready > 0) return true;
        if (ready < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() != WSAEINTR) return false;
#else
            if (errno != EINTR) return false;
#endif
        }
    }
}

static bool network_tls_cancelled(void *context)
{
    return network_is_cancelled((Om_Runtime_System_Task *)context);
}

static bool network_tls_wait_socket(void *context, bool writable)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->network_resource == NULL) return false;
    return network_wait_socket(task,
        network_native_socket(task->network_resource->native_socket), writable);
}

static bool network_set_nonblocking(Om_Native_Socket socket_value)
{
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(OM_NATIVE_SOCKET(socket_value), (long)FIONBIO,
                       &enabled) == 0;
#else
    const int flags = fcntl((int)socket_value, F_GETFL, 0);
    return flags >= 0 && fcntl((int)socket_value, F_SETFL,
                                flags | O_NONBLOCK) == 0;
#endif
}

static const char *network_last_error_code(void)
{
#if defined(_WIN32)
    switch (WSAGetLastError()) {
    case WSAEADDRINUSE: return "address_in_use";
    case WSAEACCES: return "permission_denied";
    case WSAECONNREFUSED: return "connection_refused";
    case WSAECONNRESET: return "connection_reset";
    case WSAENOTSOCK: return "closed";
    case WSAEAFNOSUPPORT: return "unsupported";
    case WSAEINVAL: return "invalid_address";
    default: return "io_error";
    }
#else
    switch (errno) {
    case EADDRINUSE: return "address_in_use";
    case EACCES: return "permission_denied";
    case ECONNREFUSED: return "connection_refused";
    case ECONNRESET: return "connection_reset";
    case EBADF: return "closed";
    case EAFNOSUPPORT: return "unsupported";
    case EINVAL: return "invalid_address";
    default: return "io_error";
    }
#endif
}

static Om_Status network_open_socket(Om_Runtime_System_Task *task,
                                     bool listening,
                                     bool datagram)
{
    char service[8];
    const int service_length = snprintf(service, sizeof(service), "%" PRId64,
                                        task->requested_length);
    if (service_length < 0 || (size_t)service_length >= sizeof(service)) {
        task->error_code = "invalid_address";
        return OM_STATUS_INVALID_ARGUMENT;
    }
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = datagram ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = datagram ? IPPROTO_UDP : IPPROTO_TCP;
    if (listening && task->path[0] == '\0') hints.ai_flags = AI_PASSIVE;
    struct addrinfo *addresses = NULL;
    const int resolve_status = getaddrinfo(
        task->path[0] == '\0' && listening ? NULL : task->path,
        service, &hints, &addresses);
    if (resolve_status != 0) {
        task->error_code = "invalid_address";
        return OM_STATUS_IO_ERROR;
    }
    Om_Status status = OM_STATUS_IO_ERROR;
    task->error_code = "io_error";
    for (struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        if (network_is_cancelled(task)) {
            task->error_code = "closed";
            status = OM_STATUS_CANCELLED;
            break;
        }
#if defined(_WIN32)
        SOCKET native = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);
        if (native == INVALID_SOCKET) {
            task->error_code = network_last_error_code();
            continue;
        }
        Om_Native_Socket socket_value = (Om_Native_Socket)native;
#else
        const int native = socket(address->ai_family, address->ai_socktype,
                                  address->ai_protocol);
        if (native < 0) {
            task->error_code = network_last_error_code();
            continue;
        }
        Om_Native_Socket socket_value = (Om_Native_Socket)native;
#endif
        if (!network_set_nonblocking(socket_value)) {
            task->error_code = network_last_error_code();
            network_close_native(socket_value);
            continue;
        }
        if (listening) {
            int reuse = 1;
            (void)setsockopt(OM_NATIVE_SOCKET(socket_value), SOL_SOCKET, SO_REUSEADDR,
                (const char *)&reuse, (int)sizeof(reuse));
            if (bind(OM_NATIVE_SOCKET(socket_value), address->ai_addr,
                     OM_SOCKET_LENGTH(address->ai_addrlen)) == 0
                && listen(OM_NATIVE_SOCKET(socket_value), task->network_backlog) == 0) {
                task->network_resource->native_socket = (intptr_t)socket_value;
                status = OM_STATUS_OK;
                break;
            }
            task->error_code = network_last_error_code();
            network_close_native(socket_value);
            continue;
        }
        if (datagram) {
            int bound = -1;
            if (address->ai_family == AF_INET) {
                struct sockaddr_in local = {0};
                local.sin_family = AF_INET;
                local.sin_addr.s_addr = htonl(INADDR_ANY);
                local.sin_port = htons((uint16_t)task->network_backlog);
                bound = bind(OM_NATIVE_SOCKET(socket_value),
                    (const struct sockaddr *)&local, (int)sizeof(local));
            } else if (address->ai_family == AF_INET6) {
                struct sockaddr_in6 local = {0};
                local.sin6_family = AF_INET6;
                local.sin6_addr = in6addr_any;
                local.sin6_port = htons((uint16_t)task->network_backlog);
                bound = bind(OM_NATIVE_SOCKET(socket_value),
                    (const struct sockaddr *)&local, (int)sizeof(local));
            }
            if (bound != 0) {
                task->error_code = network_last_error_code();
                network_close_native(socket_value);
                continue;
            }
        }
        int connected = connect(OM_NATIVE_SOCKET(socket_value), address->ai_addr,
                                OM_SOCKET_LENGTH(address->ai_addrlen));
        if (connected != 0) {
#if defined(_WIN32)
            const int error = WSAGetLastError();
            const bool in_progress = error == WSAEWOULDBLOCK
                || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
            const int error = errno;
            const bool in_progress = error == EINPROGRESS
                || error == EWOULDBLOCK || error == EALREADY;
#endif
            if (!in_progress || !network_wait_socket(task, socket_value, true)) {
                task->error_code = network_is_cancelled(task)
                    ? "closed" : network_last_error_code();
                network_close_native(socket_value);
                if (network_is_cancelled(task)) {
                    status = OM_STATUS_CANCELLED;
                    break;
                }
                continue;
            }
            int connect_error = 0;
#if defined(_WIN32)
            int connect_error_length = (int)sizeof(connect_error);
#else
            socklen_t connect_error_length = (socklen_t)sizeof(connect_error);
#endif
            if (getsockopt(OM_NATIVE_SOCKET(socket_value), SOL_SOCKET, SO_ERROR,
                    (char *)&connect_error, &connect_error_length) != 0
                || connect_error != 0) {
#if defined(_WIN32)
                WSASetLastError(connect_error);
#else
                errno = connect_error;
#endif
                task->error_code = network_last_error_code();
                network_close_native(socket_value);
                continue;
            }
        }
        task->network_resource->native_socket = (intptr_t)socket_value;
        status = OM_STATUS_OK;
        break;
    }
    freeaddrinfo(addresses);
    return status;
}

static Om_Status system_network_run(Om_Runtime_System_Task *task)
{
    const Om_System_Message_Id operation = task->operation;
    if (operation == OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START) {
        if (task->network_resource == NULL
            || task->network_resource->native_socket == -1) {
            task->error_code = "closed";
            return OM_STATUS_CANCELLED;
        }
        const bool started = om_tls_client_start(
            task->network_resource->native_socket, task->path, task,
            network_tls_cancelled, network_tls_wait_socket,
            &task->tls_session, &task->error_code);
        if (!started) return OM_STATUS_IO_ERROR;
        task->result_integer = (int64_t)task->network_resource->id;
        return OM_STATUS_OK;
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TLS_SEND) {
        size_t sent = 0;
        const bool succeeded = om_tls_send(task->network_resource->tls_session,
            task->input_bytes, task->input_length, task,
            network_tls_cancelled, network_tls_wait_socket, &sent,
            &task->error_code);
        if (!succeeded) return OM_STATUS_IO_ERROR;
        task->result_integer = (int64_t)sent;
        return OM_STATUS_OK;
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE) {
        const size_t maximum = (size_t)task->requested_length;
        if (maximum == 0) return OM_STATUS_OK;
        task->result_bytes = om_alloc(maximum);
        if (task->result_bytes == NULL) {
            task->error_code = "resource_exhausted";
            return OM_STATUS_OUT_OF_MEMORY;
        }
        task->result_capacity = maximum;
        if (!om_tls_receive(task->network_resource->tls_session,
            task->result_bytes, maximum, task, network_tls_cancelled,
            network_tls_wait_socket, &task->result_length, &task->error_code)) {
            return OM_STATUS_IO_ERROR;
        }
        return OM_STATUS_OK;
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TLS_CLOSE) {
        om_tls_close(task->tls_session);
        task->tls_session = NULL;
        if (task->network_resource != NULL
            && !task->network_resource->native_closed
            && task->network_resource->native_socket != -1) {
            network_close_native(network_native_socket(
                task->network_resource->native_socket));
        }
        return OM_STATUS_OK;
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TCP_LISTEN
        || operation == OM_SYSTEM_MESSAGE_ID_TCP_CONNECT) {
        return network_open_socket(task,
            operation == OM_SYSTEM_MESSAGE_ID_TCP_LISTEN, false);
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_UDP_CONNECT) {
        return network_open_socket(task, false, true);
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT) {
        if (task->network_listener == NULL
            || !task->network_listener->listener) {
            task->error_code = "invalid_argument";
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Native_Socket listener = network_native_socket(
            task->network_listener->native_socket);
        for (;;) {
            if (!network_wait_socket(task, listener, false)) {
                task->error_code = "closed";
                return OM_STATUS_CANCELLED;
            }
#if defined(_WIN32)
            SOCKET accepted = accept(OM_NATIVE_SOCKET(listener), NULL, NULL);
            if (accepted == INVALID_SOCKET) {
                const int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAEINTR) continue;
                task->error_code = network_last_error_code();
                return OM_STATUS_IO_ERROR;
            }
            const Om_Native_Socket accepted_socket = (Om_Native_Socket)accepted;
#else
            const int accepted = accept((int)listener, NULL, NULL);
            if (accepted < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                task->error_code = network_last_error_code();
                return OM_STATUS_IO_ERROR;
            }
            const Om_Native_Socket accepted_socket = (Om_Native_Socket)accepted;
#endif
            if (!network_set_nonblocking(accepted_socket)) {
                task->error_code = network_last_error_code();
                network_close_native(accepted_socket);
                return OM_STATUS_IO_ERROR;
            }
            task->network_resource->native_socket = (intptr_t)accepted_socket;
            return OM_STATUS_OK;
        }
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TCP_SEND) {
        if (network_is_cancelled(task)) {
            task->error_code = "closed";
            return OM_STATUS_CANCELLED;
        }
        const Om_Native_Socket socket_value = network_native_socket(
            task->network_resource->native_socket);
        for (;;) {
#if defined(_WIN32)
            const int sent = send(OM_NATIVE_SOCKET(socket_value),
                (const char *)task->input_bytes, (int)task->input_length, 0);
            if (sent >= 0) {
                task->result_integer = sent;
                return OM_STATUS_OK;
            }
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
#else
            const ssize_t sent = send((int)socket_value,
                task->input_bytes, task->input_length, MSG_NOSIGNAL);
            if (sent >= 0) {
                task->result_integer = (int64_t)sent;
                return OM_STATUS_OK;
            }
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
#endif
                if (!network_wait_socket(task, socket_value, true)) {
                    task->error_code = "closed";
                    return OM_STATUS_CANCELLED;
                }
                continue;
            }
            task->error_code = network_last_error_code();
            return OM_STATUS_IO_ERROR;
        }
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_UDP_SEND) {
        if (network_is_cancelled(task)) {
            task->error_code = "closed";
            return OM_STATUS_CANCELLED;
        }
        const Om_Native_Socket socket_value = network_native_socket(
            task->network_resource->native_socket);
        for (;;) {
#if defined(_WIN32)
            const int sent = send(OM_NATIVE_SOCKET(socket_value),
                (const char *)task->input_bytes, (int)task->input_length, 0);
            if (sent >= 0) {
#else
            const ssize_t sent = send((int)socket_value,
                task->input_bytes, task->input_length, MSG_NOSIGNAL);
            if (sent >= 0) {
#endif
                if ((size_t)sent != task->input_length) {
                    task->error_code = "io_error";
                    return OM_STATUS_IO_ERROR;
                }
                task->result_integer = (int64_t)sent;
                return OM_STATUS_OK;
            }
#if defined(_WIN32)
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
#else
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
#endif
                if (!network_wait_socket(task, socket_value, true)) {
                    task->error_code = "closed";
                    return OM_STATUS_CANCELLED;
                }
                continue;
            }
            task->error_code = network_last_error_code();
            return OM_STATUS_IO_ERROR;
        }
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE) {
        const size_t maximum = (size_t)task->requested_length;
        if (maximum == 0) {
            task->error_code = "invalid_argument";
            return OM_STATUS_INVALID_ARGUMENT;
        }
        task->result_bytes = om_alloc(maximum);
        if (task->result_bytes == NULL) {
            task->error_code = "resource_exhausted";
            return OM_STATUS_OUT_OF_MEMORY;
        }
        task->result_capacity = maximum;
        const Om_Native_Socket socket_value = network_native_socket(
            task->network_resource->native_socket);
        for (;;) {
            if (!network_wait_socket(task, socket_value, false)) {
                task->error_code = "closed";
                return OM_STATUS_CANCELLED;
            }
#if defined(_WIN32)
            const int received = recv(OM_NATIVE_SOCKET(socket_value),
                (char *)task->result_bytes, (int)maximum, 0);
            if (received >= 0) {
                task->result_length = (size_t)received;
                return OM_STATUS_OK;
            }
            const int error = WSAGetLastError();
            if (error == WSAEMSGSIZE) {
                task->error_code = "datagram_too_large";
                return OM_STATUS_IO_ERROR;
            }
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) continue;
#else
            struct iovec vector = {.iov_base = task->result_bytes,
                                   .iov_len = maximum};
            struct msghdr message = {0};
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            const ssize_t received = recvmsg((int)socket_value, &message,
                                             MSG_TRUNC);
            if (received >= 0) {
                if ((size_t)received > maximum
                    || (message.msg_flags & MSG_TRUNC) != 0) {
                    task->error_code = "datagram_too_large";
                    return OM_STATUS_IO_ERROR;
                }
                task->result_length = (size_t)received;
                return OM_STATUS_OK;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
#endif
            task->error_code = network_last_error_code();
            return OM_STATUS_IO_ERROR;
        }
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE) {
        const size_t maximum = (size_t)task->requested_length;
        if (maximum == 0) return OM_STATUS_OK;
        task->result_bytes = om_alloc(maximum);
        if (task->result_bytes == NULL) {
            task->error_code = "resource_exhausted";
            return OM_STATUS_OUT_OF_MEMORY;
        }
        task->result_capacity = maximum;
        const Om_Native_Socket socket_value = network_native_socket(
            task->network_resource->native_socket);
        for (;;) {
            if (!network_wait_socket(task, socket_value, false)) {
                task->error_code = "closed";
                return OM_STATUS_CANCELLED;
            }
#if defined(_WIN32)
            const int received = recv(OM_NATIVE_SOCKET(socket_value),
                (char *)task->result_bytes, (int)maximum, 0);
            if (received > 0) {
                task->result_length = (size_t)received;
                return OM_STATUS_OK;
            }
            if (received == 0) {
                task->error_code = "end_of_stream";
                return OM_STATUS_IO_ERROR;
            }
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) continue;
#else
            const ssize_t received = recv((int)socket_value,
                task->result_bytes, maximum, 0);
            if (received > 0) {
                task->result_length = (size_t)received;
                return OM_STATUS_OK;
            }
            if (received == 0) {
                task->error_code = "end_of_stream";
                return OM_STATUS_IO_ERROR;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
#endif
            task->error_code = network_last_error_code();
            return OM_STATUS_IO_ERROR;
        }
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_TCP_CLOSE) {
        if (task->network_resource != NULL) {
            Om_Runtime_Actor_System *system = task->system;
            actor_mutex_lock(system);
            if (!task->network_resource->native_closed
                && task->network_resource->native_socket != -1) {
                network_close_native(network_native_socket(
                    task->network_resource->native_socket));
                task->network_resource->native_closed = true;
            }
            actor_mutex_unlock(system);
        }
        return OM_STATUS_OK;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static bool system_dns_host_valid(const char *host)
{
    if (host == NULL || host[0] == '\0') return false;
    struct in_addr ipv4 = {0};
    struct in6_addr ipv6 = {0};
    if (inet_pton(AF_INET, host, &ipv4) == 1
        || inet_pton(AF_INET6, host, &ipv6) == 1) {
        return true;
    }

    const size_t length = strlen(host);
    if (length > 253) return false;
    size_t label_length = 0;
    bool previous_hyphen = false;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = (unsigned char)host[index];
        if (character == '.') {
            if (label_length == 0 || previous_hyphen) return false;
            label_length = 0;
            previous_hyphen = false;
            continue;
        }
        const bool alpha = (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (character == '-') {
            if (label_length == 0) return false;
            previous_hyphen = true;
        } else if (alpha || digit) {
            previous_hyphen = false;
        } else {
            return false;
        }
        ++label_length;
        if (label_length > 63) return false;
    }
    return !previous_hyphen
        && (label_length != 0
            || (length > 1 && host[length - 1] == '.'));
}

static Om_Status system_dns_resolve(Om_Runtime_System_Task *task)
{
    if (task == NULL || task->path == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if (!system_dns_host_valid(task->path)) {
        task->error_code = "invalid_host";
        return OM_STATUS_INVALID_ARGUMENT;
    }
    char service[6];
    const int service_length = snprintf(service, sizeof(service), "%d",
                                        task->network_backlog);
    if (service_length < 1 || (size_t)service_length >= sizeof(service)) {
        task->error_code = "invalid_host";
        return OM_STATUS_INVALID_ARGUMENT;
    }
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    struct addrinfo *results = NULL;
    const int resolve_status = getaddrinfo(task->path, service, &hints,
                                           &results);
    if (resolve_status != 0) {
#if defined(_WIN32)
        if (resolve_status == WSAHOST_NOT_FOUND
            || resolve_status == WSANO_DATA) {
            task->error_code = "not_found";
        } else if (resolve_status == WSATRY_AGAIN) {
            task->error_code = "temporary_failure";
        } else {
            task->error_code = "io_error";
        }
#else
        if (resolve_status == EAI_AGAIN) {
            task->error_code = "temporary_failure";
        } else if (resolve_status == EAI_SYSTEM) {
            task->error_code = "io_error";
        } else {
            task->error_code = "not_found";
        }
#endif
        return OM_STATUS_NOT_FOUND;
    }

    Om_Status status = OM_STATUS_OK;
    size_t count = 0;
    for (const struct addrinfo *item = results; item != NULL;
         item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) {
            continue;
        }
        if (count >= 256) {
            task->error_code = "resource_exhausted";
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        const void *address_bytes = item->ai_family == AF_INET
            ? (const void *)&((const struct sockaddr_in *)item->ai_addr)->sin_addr
            : (const void *)&((const struct sockaddr_in6 *)item->ai_addr)->sin6_addr;
        char address_text[INET6_ADDRSTRLEN];
        if (inet_ntop(item->ai_family, address_bytes, address_text,
                      sizeof(address_text)) == NULL) {
            task->error_code = "io_error";
            status = OM_STATUS_IO_ERROR;
            break;
        }
        status = system_task_add_text_item(task, address_text,
                                           strlen(address_text));
        if (status != OM_STATUS_OK) break;
        ++count;
    }
    freeaddrinfo(results);
    if (status == OM_STATUS_OK && count == 0) {
        task->error_code = "not_found";
        return OM_STATUS_NOT_FOUND;
    }
    return status;
}

#if !defined(_WIN32)
extern char **environ;

static Om_Status system_process_spawn(Om_Runtime_System_Task *task)
{
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0
        || pipe(stderr_pipe) != 0 || pipe(exec_pipe) != 0) {
        task->error_code = errno == EMFILE || errno == ENFILE
            ? "resource_exhausted" : "spawn_failed";
        goto failed;
    }
    for (size_t index = 0; index < 2; ++index) {
        const int descriptors[] = {stdin_pipe[index], stdout_pipe[index],
                                   stderr_pipe[index], exec_pipe[index]};
        for (size_t descriptor_index = 0;
             descriptor_index < sizeof(descriptors) / sizeof(descriptors[0]);
             ++descriptor_index) {
            const int descriptor_flags = fcntl(descriptors[descriptor_index],
                                                F_GETFD, 0);
            if (descriptor_flags < 0
                || fcntl(descriptors[descriptor_index], F_SETFD,
                         descriptor_flags | FD_CLOEXEC) < 0) {
                task->error_code = "spawn_failed";
                goto failed;
            }
        }
    }
    const int flags = fcntl(exec_pipe[1], F_GETFD, 0);
    if (flags < 0 || fcntl(exec_pipe[1], F_SETFD, flags | FD_CLOEXEC) < 0) {
        task->error_code = "spawn_failed";
        goto failed;
    }
    char **argv = om_alloc_array(task->process_argument_count + 2,
                                 sizeof(*argv));
    if (argv == NULL) {
        task->error_code = "resource_exhausted";
        goto failed;
    }
    argv[0] = task->path;
    for (size_t index = 0; index < task->process_argument_count; ++index) {
        argv[index + 1] = task->process_arguments[index];
    }
    argv[task->process_argument_count + 1] = NULL;
    char **envp = task->process_replace_environment
        ? task->process_environment : environ;
    if (task->process_replace_environment && envp != NULL) {
        envp[task->process_environment_count] = NULL;
    }
    const pid_t child = fork();
    if (child == 0) {
        (void)dup2(stdin_pipe[0], STDIN_FILENO);
        (void)dup2(stdout_pipe[1], STDOUT_FILENO);
        (void)dup2(stderr_pipe[1], STDERR_FILENO);
        (void)fcntl(STDIN_FILENO, F_SETFD, 0);
        (void)fcntl(STDOUT_FILENO, F_SETFD, 0);
        (void)fcntl(STDERR_FILENO, F_SETFD, 0);
        for (size_t index = 0; index < 2; ++index) {
            (void)close(stdin_pipe[index]);
            (void)close(stdout_pipe[index]);
            (void)close(stderr_pipe[index]);
            if (index == 0) (void)close(exec_pipe[index]);
        }
        if (task->working_directory != NULL
            && task->working_directory[0] != '\0'
            && chdir(task->working_directory) != 0) {
            const int child_error = errno;
            (void)write(exec_pipe[1], &child_error, sizeof(child_error));
            _exit(127);
        }
        execve(task->path, argv, envp);
        const int child_error = errno;
        (void)write(exec_pipe[1], &child_error, sizeof(child_error));
        _exit(127);
    }
    om_free(argv);
    argv = NULL;
    if (child < 0) {
        task->error_code = errno == EAGAIN || errno == ENOMEM
            ? "resource_exhausted" : "spawn_failed";
        goto failed;
    }
    (void)close(stdin_pipe[0]); stdin_pipe[0] = -1;
    (void)close(stdout_pipe[1]); stdout_pipe[1] = -1;
    (void)close(stderr_pipe[1]); stderr_pipe[1] = -1;
    (void)close(exec_pipe[1]); exec_pipe[1] = -1;
    int child_error = 0;
    ssize_t error_bytes;
    do { error_bytes = read(exec_pipe[0], &child_error, sizeof(child_error)); }
    while (error_bytes < 0 && errno == EINTR);
    (void)close(exec_pipe[0]); exec_pipe[0] = -1;
    if (error_bytes < 0) {
        (void)kill(child, SIGKILL);
        int child_status = 0;
        (void)waitpid(child, &child_status, 0);
        task->error_code = "spawn_failed";
        goto failed;
    }
    if (error_bytes > 0) {
        (void)close(stdin_pipe[1]); stdin_pipe[1] = -1;
        (void)close(stdout_pipe[0]); stdout_pipe[0] = -1;
        (void)close(stderr_pipe[0]); stderr_pipe[0] = -1;
        int child_status = 0;
        (void)waitpid(child, &child_status, 0);
        task->error_code = child_error == ENOENT ? "not_found"
            : child_error == EACCES ? "permission_denied" : "spawn_failed";
        goto failed;
    }
    task->process_resource->native_process = (intptr_t)child;
    task->process_resource->process_guard = -1;
#if defined(__linux__) && defined(SYS_pidfd_open)
    task->process_resource->process_guard =
        (intptr_t)syscall(SYS_pidfd_open, child, 0U);
#endif
    task->process_resource->stdin_handle = (intptr_t)stdin_pipe[1];
    task->process_resource->stdout_handle = (intptr_t)stdout_pipe[0];
    task->process_resource->stderr_handle = (intptr_t)stderr_pipe[0];
    return OM_STATUS_OK;

failed:
    for (size_t index = 0; index < 2; ++index) {
        if (stdin_pipe[index] >= 0) (void)close(stdin_pipe[index]);
        if (stdout_pipe[index] >= 0) (void)close(stdout_pipe[index]);
        if (stderr_pipe[index] >= 0) (void)close(stderr_pipe[index]);
        if (exec_pipe[index] >= 0) (void)close(exec_pipe[index]);
    }
    return OM_STATUS_IO_ERROR;
}
#else
static wchar_t *process_utf8_to_wide(const char *text, size_t length)
{
    if (length == 0) {
        wchar_t *empty = om_alloc(sizeof(*empty));
        if (empty != NULL) empty[0] = L'\0';
        return empty;
    }
    if (length > (size_t)INT_MAX) return NULL;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, (int)length, NULL, 0);
    if (required <= 0) return NULL;
    wchar_t *wide = om_alloc_array((size_t)required + 1, sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
            (int)length, wide, required) != required) {
        om_free(wide);
        return NULL;
    }
    wide[required] = L'\0';
    return wide;
}

static bool process_append_quoted(wchar_t *output, size_t capacity,
                                  size_t *length, const wchar_t *argument)
{
    if (*length + 2 >= capacity) return false;
    output[(*length)++] = L'"';
    size_t slashes = 0;
    for (const wchar_t *cursor = argument;; ++cursor) {
        const wchar_t value = *cursor;
        if (value == L'\\') { ++slashes; continue; }
        if (value == L'"') {
            if (slashes > (capacity - *length - 1) / 2) return false;
            for (size_t index = 0; index < slashes * 2 + 1; ++index)
                output[(*length)++] = L'\\';
            output[(*length)++] = L'"';
            slashes = 0;
            continue;
        }
        if (value == L'\0') {
            if (slashes > (capacity - *length - 2) / 2) return false;
            for (size_t index = 0; index < slashes * 2; ++index)
                output[(*length)++] = L'\\';
            output[(*length)++] = L'"';
            output[*length] = L'\0';
            return true;
        }
        if (slashes > capacity - *length - 1) return false;
        for (size_t index = 0; index < slashes; ++index)
            output[(*length)++] = L'\\';
        if (*length + 1 >= capacity) return false;
        output[(*length)++] = value;
        slashes = 0;
    }
}

static Om_Status system_process_spawn(Om_Runtime_System_Task *task)
{
    const size_t command_capacity = 32768;
    wchar_t *command = om_alloc_array(command_capacity, sizeof(*command));
    if (command == NULL) { task->error_code = "resource_exhausted"; return OM_STATUS_OUT_OF_MEMORY; }
    size_t command_length = 0;
    wchar_t *executable = process_utf8_to_wide(task->path, strlen(task->path));
    if (executable == NULL) { om_free(command); task->error_code = "invalid_argument"; return OM_STATUS_INVALID_ARGUMENT; }
    bool ok = process_append_quoted(command, command_capacity,
                                     &command_length, executable);
    om_free(executable);
    wchar_t **wide_arguments = task->process_argument_count == 0 ? NULL
        : om_alloc_array(task->process_argument_count, sizeof(*wide_arguments));
    if (task->process_argument_count != 0 && wide_arguments == NULL) ok = false;
    if (wide_arguments != NULL) memset(wide_arguments, 0,
        task->process_argument_count * sizeof(*wide_arguments));
    for (size_t index = 0; ok && index < task->process_argument_count; ++index) {
        wide_arguments[index] = process_utf8_to_wide(
            task->process_arguments[index], strlen(task->process_arguments[index]));
        if (wide_arguments[index] == NULL) { ok = false; break; }
        if (command_length + 1 >= command_capacity) { ok = false; break; }
        command[command_length++] = L' ';
        ok = process_append_quoted(command, command_capacity, &command_length,
                                   wide_arguments[index]);
    }
    for (size_t index = 0; wide_arguments != NULL
         && index < task->process_argument_count; ++index) om_free(wide_arguments[index]);
    om_free(wide_arguments);
    if (!ok) { om_free(command); task->error_code = "resource_exhausted"; return OM_STATUS_OUT_OF_RANGE; }

    wchar_t *cwd = NULL;
    if (task->working_directory != NULL && task->working_directory[0] != '\0') {
        cwd = process_utf8_to_wide(task->working_directory,
                                   strlen(task->working_directory));
        if (cwd == NULL) { om_free(command); task->error_code = "invalid_argument"; return OM_STATUS_INVALID_ARGUMENT; }
    }
    wchar_t **wide_environment = NULL;
    size_t environment_units = 1;
    if (task->process_replace_environment) {
        wide_environment = om_alloc_array(task->process_environment_count + 1,
                                           sizeof(*wide_environment));
        if (wide_environment == NULL) { om_free(cwd); om_free(command); task->error_code = "resource_exhausted"; return OM_STATUS_OUT_OF_MEMORY; }
        memset(wide_environment, 0, (task->process_environment_count + 1) * sizeof(*wide_environment));
        for (size_t index = 0; index < task->process_environment_count; ++index) {
            wide_environment[index] = process_utf8_to_wide(
                task->process_environment[index], strlen(task->process_environment[index]));
            if (wide_environment[index] == NULL) { ok = false; break; }
            environment_units += wcslen(wide_environment[index]) + 1;
        }
        if (!ok || environment_units > 32767) {
            for (size_t index = 0; index < task->process_environment_count; ++index) om_free(wide_environment[index]);
            om_free(wide_environment); om_free(cwd); om_free(command);
            task->error_code = ok ? "resource_exhausted" : "invalid_argument";
            return OM_STATUS_OUT_OF_RANGE;
        }
    }
    wchar_t *environment_block = NULL;
    if (task->process_replace_environment) {
        environment_block = om_alloc_array(environment_units + 1,
                                            sizeof(*environment_block));
        if (environment_block == NULL) ok = false;
        else {
            memset(environment_block, 0,
                   (environment_units + 1) * sizeof(*environment_block));
            size_t offset = 0;
            for (size_t index = 0; index < task->process_environment_count; ++index) {
                const size_t length = wcslen(wide_environment[index]);
                (void)memcpy(environment_block + offset, wide_environment[index],
                             length * sizeof(*environment_block));
                offset += length + 1;
            }
            environment_block[offset] = L'\0';
        }
    }
    for (size_t index = 0; wide_environment != NULL
         && index < task->process_environment_count; ++index) om_free(wide_environment[index]);
    om_free(wide_environment);
    if (!ok) { om_free(environment_block); om_free(cwd); om_free(command); task->error_code = "resource_exhausted"; return OM_STATUS_OUT_OF_MEMORY; }

    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE child_stdin = INVALID_HANDLE_VALUE, parent_stdin = INVALID_HANDLE_VALUE;
    HANDLE parent_stdout = INVALID_HANDLE_VALUE, child_stdout = INVALID_HANDLE_VALUE;
    HANDLE parent_stderr = INVALID_HANDLE_VALUE, child_stderr = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&child_stdin, &parent_stdin, &security, 0)
        || !CreatePipe(&parent_stdout, &child_stdout, &security, 0)
        || !CreatePipe(&parent_stderr, &child_stderr, &security, 0)) {
        task->error_code = GetLastError() == ERROR_TOO_MANY_OPEN_FILES
            ? "resource_exhausted" : "spawn_failed";
        goto windows_spawn_cleanup;
    }
    (void)SetHandleInformation(parent_stdin, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(parent_stdout, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(parent_stderr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin;
    startup.hStdOutput = child_stdout;
    startup.hStdError = child_stderr;
    PROCESS_INFORMATION information = {0};
    const DWORD creation_flags = task->process_replace_environment
        ? CREATE_UNICODE_ENVIRONMENT : 0;
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, creation_flags,
            environment_block, cwd, &startup, &information)) {
        const DWORD error = GetLastError();
        task->error_code = error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND ? "not_found"
            : error == ERROR_ACCESS_DENIED ? "permission_denied" : "spawn_failed";
        goto windows_spawn_cleanup;
    }
    (void)CloseHandle(information.hThread);
    task->process_resource->native_process = (intptr_t)information.hProcess;
    task->process_resource->stdin_handle = (intptr_t)parent_stdin; parent_stdin = INVALID_HANDLE_VALUE;
    task->process_resource->stdout_handle = (intptr_t)parent_stdout; parent_stdout = INVALID_HANDLE_VALUE;
    task->process_resource->stderr_handle = (intptr_t)parent_stderr; parent_stderr = INVALID_HANDLE_VALUE;
    (void)CloseHandle(child_stdin); child_stdin = INVALID_HANDLE_VALUE;
    (void)CloseHandle(child_stdout); child_stdout = INVALID_HANDLE_VALUE;
    (void)CloseHandle(child_stderr); child_stderr = INVALID_HANDLE_VALUE;
    om_free(environment_block); om_free(cwd); om_free(command);
    return OM_STATUS_OK;

windows_spawn_cleanup:
    if (child_stdin != INVALID_HANDLE_VALUE) (void)CloseHandle(child_stdin);
    if (parent_stdin != INVALID_HANDLE_VALUE) (void)CloseHandle(parent_stdin);
    if (parent_stdout != INVALID_HANDLE_VALUE) (void)CloseHandle(parent_stdout);
    if (child_stdout != INVALID_HANDLE_VALUE) (void)CloseHandle(child_stdout);
    if (parent_stderr != INVALID_HANDLE_VALUE) (void)CloseHandle(parent_stderr);
    if (child_stderr != INVALID_HANDLE_VALUE) (void)CloseHandle(child_stderr);
    om_free(environment_block); om_free(cwd); om_free(command);
    return OM_STATUS_IO_ERROR;
}
#endif

static Om_Status system_process_run(Om_Runtime_System_Task *task)
{
    const Om_System_Message_Id operation = task->operation;
    if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN) {
        return system_process_spawn(task);
    }
    struct Om_Runtime_Process_Resource *process = task->process_resource;
    if (process == NULL) {
        task->error_code = "closed";
        return OM_STATUS_IO_ERROR;
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN) {
#if defined(_WIN32)
        HANDLE pipe = (HANDLE)process->stdin_handle;
        if (pipe == INVALID_HANDLE_VALUE) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        size_t offset = 0;
        while (offset < task->input_length) {
            const DWORD amount = task->input_length - offset > UINT32_MAX
                ? UINT32_MAX : (DWORD)(task->input_length - offset);
            DWORD written = 0;
            if (!WriteFile(pipe, task->input_bytes + offset, amount, &written, NULL)
                || written == 0) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
            offset += written;
        }
        task->result_integer = (int64_t)offset;
        return OM_STATUS_OK;
#else
        const int descriptor = (int)process->stdin_handle;
        if (descriptor < 0) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        sigset_t blocked, previous, pending;
        (void)sigemptyset(&blocked);
        (void)sigaddset(&blocked, SIGPIPE);
        const bool was_pending = sigpending(&pending) == 0
            && sigismember(&pending, SIGPIPE) == 1;
        const bool mask_changed = pthread_sigmask(SIG_BLOCK, &blocked,
                                                  &previous) == 0;
        size_t offset = 0;
        while (offset < task->input_length) {
            const ssize_t written = write(descriptor, task->input_bytes + offset,
                                          task->input_length - offset);
            if (written > 0) { offset += (size_t)written; continue; }
            if (written < 0 && errno == EINTR) continue;
            task->error_code = errno == EPIPE ? "closed" : "io_error";
            if (mask_changed) {
                if (!was_pending) {
                    struct timespec zero = {0};
                    (void)sigtimedwait(&blocked, NULL, &zero);
                }
                (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
            }
            return OM_STATUS_IO_ERROR;
        }
        if (mask_changed) (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
        task->result_integer = (int64_t)offset;
        return OM_STATUS_OK;
#endif
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN) {
#if defined(_WIN32)
        HANDLE pipe = (HANDLE)process->stdin_handle;
        if (pipe != INVALID_HANDLE_VALUE) (void)CloseHandle(pipe);
        return OM_STATUS_OK;
#else
        const int descriptor = (int)process->stdin_handle;
        if (descriptor >= 0) {
            (void)close(descriptor);
        /* The completion callback clears the handle while holding the system lock. */
        }
        return OM_STATUS_OK;
#endif
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT
        || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR) {
        const intptr_t native_handle = operation
            == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT
                ? process->stdout_handle : process->stderr_handle;
#if defined(_WIN32)
        HANDLE pipe = (HANDLE)native_handle;
        if (pipe == INVALID_HANDLE_VALUE) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        const size_t maximum = (size_t)task->requested_length;
        if (maximum == 0) return OM_STATUS_OK;
        task->result_bytes = om_alloc(maximum);
        if (task->result_bytes == NULL) { task->error_code = "resource_exhausted"; return OM_STATUS_OUT_OF_MEMORY; }
        DWORD received = 0;
        const DWORD amount = maximum > UINT32_MAX ? UINT32_MAX : (DWORD)maximum;
        if (!ReadFile(pipe, task->result_bytes, amount, &received, NULL)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                (void)CloseHandle(pipe);
                task->error_code = "closed";
            } else task->error_code = "io_error";
            return OM_STATUS_IO_ERROR;
        }
        if (received == 0) { (void)CloseHandle(pipe); task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        task->result_length = received;
        return OM_STATUS_OK;
#else
        const int descriptor = (int)native_handle;
        if (descriptor < 0) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        const size_t maximum = (size_t)task->requested_length;
        if (maximum == 0) return OM_STATUS_OK;
        task->result_bytes = maximum == 0 ? NULL : om_alloc(maximum);
        if (maximum != 0 && task->result_bytes == NULL) {
            task->error_code = "resource_exhausted";
            return OM_STATUS_OUT_OF_MEMORY;
        }
        ssize_t received;
        do { received = read(descriptor, task->result_bytes, maximum); }
        while (received < 0 && errno == EINTR);
        if (received < 0) { task->error_code = "io_error"; return OM_STATUS_IO_ERROR; }
        if (received == 0) {
            (void)close(descriptor);
            task->error_code = "closed";
            return OM_STATUS_IO_ERROR;
        }
        task->result_length = (size_t)received;
        return OM_STATUS_OK;
#endif
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE) {
#if defined(_WIN32)
        if (process->native_process != -1
            && !TerminateProcess((HANDLE)process->native_process, 1)) {
            task->error_code = "io_error"; return OM_STATUS_IO_ERROR;
        }
        return OM_STATUS_OK;
#else
        const pid_t child = (pid_t)process->native_process;
        if (child > 0 && kill(child, SIGTERM) != 0 && errno != ESRCH) {
            task->error_code = "io_error";
            return OM_STATUS_IO_ERROR;
        }
        return OM_STATUS_OK;
#endif
    }
    if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT) {
#if defined(_WIN32)
        HANDLE child = (HANDLE)process->native_process;
        if (child == INVALID_HANDLE_VALUE) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        if (WaitForSingleObject(child, INFINITE) != WAIT_OBJECT_0) { task->error_code = "io_error"; return OM_STATUS_IO_ERROR; }
        DWORD exit_code = 255;
        if (!GetExitCodeProcess(child, &exit_code)) { task->error_code = "io_error"; return OM_STATUS_IO_ERROR; }
        task->result_integer = (int64_t)exit_code;
        return OM_STATUS_OK;
#else
        const pid_t child = (pid_t)process->native_process;
        if (child <= 0) { task->error_code = "closed"; return OM_STATUS_IO_ERROR; }
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); }
        while (waited < 0 && errno == EINTR);
        if (waited < 0) { task->error_code = errno == ECHILD ? "closed" : "io_error"; return OM_STATUS_IO_ERROR; }
        task->result_integer = WIFEXITED(status) ? WEXITSTATUS(status)
            : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 255;
        return OM_STATUS_OK;
#endif
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static void process_resource_cleanup(struct Om_Runtime_Process_Resource *process)
{
    if (process == NULL) return;
#if defined(_WIN32)
    if (process->native_process != -1) {
        HANDLE child = (HANDLE)process->native_process;
        (void)TerminateProcess(child, 1);
        (void)WaitForSingleObject(child, 1000);
        (void)CloseHandle(child);
    }
    if (process->stdin_handle != -1) (void)CloseHandle((HANDLE)process->stdin_handle);
    if (process->stdout_handle != -1) (void)CloseHandle((HANDLE)process->stdout_handle);
    if (process->stderr_handle != -1) (void)CloseHandle((HANDLE)process->stderr_handle);
#else
    const pid_t child = (pid_t)process->native_process;
    if (child > 0 && !process->exited) {
        (void)kill(child, SIGKILL);
        int child_status = 0;
        while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
    }
    if (process->stdin_handle >= 0) (void)close((int)process->stdin_handle);
    if (process->stdout_handle >= 0) (void)close((int)process->stdout_handle);
    if (process->stderr_handle >= 0) (void)close((int)process->stderr_handle);
#endif
    process->native_process = -1;
#if !defined(_WIN32)
    if (process->process_guard >= 0) (void)close((int)process->process_guard);
#endif
    process->process_guard = -1;
    process->stdin_handle = -1;
    process->stdout_handle = -1;
    process->stderr_handle = -1;
}

static const Om_Runtime_Type_Descriptor *actor_system_message_type(
    const Om_Runtime_Actor_System *system,
    Om_System_Message_Id message_id)
{
    if (system == NULL || system->program == NULL) return NULL;
    for (size_t index = 0; index < system->program->type_count; ++index) {
        const Om_Runtime_Type_Descriptor *type = system->program->types[index];
        if (type != NULL && type->system_message_id == message_id) {
            return type;
        }
    }
    return NULL;
}

static Om_Status actor_deliver_result_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient_id,
    Om_System_Message_Id completion_id,
    bool succeeded,
    bool success_boolean,
    int64_t success_integer,
    const uint8_t *success_bytes,
    size_t success_length,
    const Om_Runtime_System_Text_Item *success_items,
    size_t success_item_count,
    const char *error_text)
{

    Om_Runtime_Actor *recipient = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){recipient_id});
    if (recipient == NULL || recipient->terminated
        || recipient->stop_requested) {
        return OM_STATUS_OK;
    }
    const Om_Runtime_Type_Descriptor *completion_type =
        actor_system_message_type(system, completion_id);
    if (completion_type == NULL || completion_type->kind != OM_RUNTIME_TYPE_MESSAGE
        || completion_type->field_count != 1
        || completion_type->fields[0].type == NULL
        || !result_descriptor_valid(completion_type->fields[0].type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *result_type =
        completion_type->fields[0].type;
    const Om_Runtime_Field_Descriptor *payload_field =
        &result_type->fields[succeeded ? 1 : 2];
    if ((succeeded && payload_field->type->kind != OM_RUNTIME_TYPE_UNIT
         && payload_field->type->kind != OM_RUNTIME_TYPE_STRING
         && payload_field->type->kind != OM_RUNTIME_TYPE_BOOL
         && payload_field->type->kind != OM_RUNTIME_TYPE_INT
         && (payload_field->type->kind != OM_RUNTIME_TYPE_LIST
             || payload_field->type->element_type == NULL
             || (payload_field->type->element_type->kind
                    != OM_RUNTIME_TYPE_STRING
                 && payload_field->type->element_type->kind
                    != OM_RUNTIME_TYPE_INT)))
        || (!succeeded && payload_field->type->kind != OM_RUNTIME_TYPE_STRING)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    void *completion_object = NULL;
    Om_Status status = om_runtime_record_create(system->mailbox_context,
        completion_type, &completion_object);

    if (status != OM_STATUS_OK) return status;
    void *roots[] = {completion_object, NULL, NULL, NULL};
    Om_Runtime_Root_Frame *root = om_runtime_roots_push(
        system->mailbox_context, roots, 4);

    if (root == NULL) return OM_STATUS_OUT_OF_MEMORY;

    unsigned char *result_value = om_alloc(result_type->size);
    if (result_value == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    } else {
        (void)memset(result_value, 0, result_type->size);
        (void)memcpy(result_value + result_type->fields[0].offset,
                     &succeeded, sizeof(succeeded));
        if (payload_field->type->kind == OM_RUNTIME_TYPE_BOOL) {
            (void)memcpy(result_value + payload_field->offset,
                         &success_boolean, sizeof(success_boolean));
        } else if (payload_field->type->kind == OM_RUNTIME_TYPE_INT) {
            (void)memcpy(result_value + payload_field->offset,
                         &success_integer, sizeof(success_integer));
        } else if (payload_field->type->kind == OM_RUNTIME_TYPE_STRING) {
            const uint8_t *text = success_bytes;
            size_t length = success_length;
            static const uint8_t fallback_error[] = "system operation failed";
            if (!succeeded) {
                text = error_text == NULL ? fallback_error
                    : (const uint8_t *)error_text;
                length = strlen((const char *)text);
            }
            status = om_runtime_string_create_utf8_default(
                system->mailbox_context, text, length, &roots[1]);

            if (status == OM_STATUS_OK) {
                (void)memcpy(result_value + payload_field->offset, &roots[1],
                             sizeof(roots[1]));
            }
        } else if (payload_field->type->kind == OM_RUNTIME_TYPE_LIST) {
            status = om_runtime_list_create(system->mailbox_context,
                payload_field->type, &roots[2]);
            if (payload_field->type->element_type->kind
                == OM_RUNTIME_TYPE_STRING) {
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < success_item_count; ++index) {
                    status = om_runtime_string_create_utf8_default(
                        system->mailbox_context,
                        (const uint8_t *)success_items[index].bytes,
                        success_items[index].length, &roots[3]);
                    if (status == OM_STATUS_OK) {
                        const Om_Runtime_Value item = {.managed = roots[3]};
                        status = om_runtime_list_append(system->mailbox_context,
                                                         roots[2], item);
                        roots[3] = NULL;
                    }
                }
            } else {
                if (success_length != 0 && success_bytes == NULL) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                }
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < success_length; ++index) {
                    const Om_Runtime_Value item = {
                        .integer = (int64_t)success_bytes[index],
                    };
                    status = om_runtime_list_append_value(
                        system->mailbox_context, roots[2], &item);
                }
            }
            if (status == OM_STATUS_OK) {
                (void)memcpy(result_value + payload_field->offset, &roots[2],
                             sizeof(roots[2]));
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set_value(system->mailbox_context,
                completion_object, 0, result_value);

        }
    }
    om_free(result_value);
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, recipient,
                                              completion_object);

    }
    const Om_Status pop_status = om_runtime_roots_pop(
        system->mailbox_context, root);

    if (status == OM_STATUS_OK) status = pop_status;
    return status;
}

static Om_Status actor_deliver_message_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient_id,
    Om_Execution_Context *source_context,
    const void *message_object)
{
    Om_Runtime_Actor *recipient = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){recipient_id});
    if (recipient == NULL || recipient->terminated
        || recipient->stop_requested) {
        return OM_STATUS_OK;
    }
    const Om_Runtime_Object_Header *source = find_object(source_context,
                                                          message_object);
    if (source == NULL || source->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *mailbox_object = NULL;
    Om_Status status = om_runtime_object_clone(source_context,
        system->mailbox_context, message_object, &mailbox_object);
    if (status == OM_STATUS_OK
        && actor_handler_for(recipient->descriptor, source->fields.type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, recipient, mailbox_object);
    }
    return status;
}

Om_Status actor_send_console_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    const bool read_line = message_type != NULL
        && message_type->system_message_id
            == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE;
    if (message_type == NULL
        || (message_type->system_message_id
                != OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE && !read_line)
        || message_type->field_count != (read_line ? 1U : 2U)) {
        return OM_STATUS_NOT_FOUND;
    }

    void *text_object = NULL;
    uint64_t callback_id = 0;
    Om_String_View text = {0};
    Om_Status status = OM_STATUS_OK;
    if (read_line) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &callback_id);
    } else {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &text_object);
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 1,
                                           &callback_id);
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_string_view(source_context, text_object, &text);
        }
    }
    if (status != OM_STATUS_OK) return status;

    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX
        || (read_line && system->next_console_read_sequence == UINT64_MAX)) {
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = read_line ? OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE
                               : OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE,
        .recipient_id = callback_id,
        .sequence = read_line ? system->next_console_read_sequence : 0,
        .input_length = text.length,
    };
    if (text.length != 0) {
        task->input_bytes = om_alloc(text.length);
        if (task->input_bytes == NULL) {
            system_task_destroy(task);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(task->input_bytes, text.data, text.length);
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    } else if (read_line) {
        ++system->next_console_read_sequence;
    }
    return status;
}

static Om_Status system_task_copy_string(Om_Execution_Context *context,
                                        const void *string_object,
                                        char **result,
                                        size_t *result_length)
{
    if (context == NULL || string_object == NULL || result == NULL
        || result_length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    *result_length = 0;
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string_object, &view);
    if (status != OM_STATUS_OK) return status;
    if (view.length != 0 && memchr(view.data, '\0', view.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(view.length, 1, &allocation_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    char *copy = om_alloc(allocation_size);
    if (copy == NULL) return OM_STATUS_OUT_OF_MEMORY;
    if (view.length != 0) (void)memcpy(copy, view.data, view.length);
    copy[view.length] = '\0';
    *result = copy;
    *result_length = view.length;
    return OM_STATUS_OK;
}

static const char *system_file_error_from_errno(int error_number)
{
    switch (error_number) {
    case ENOENT: return "not_found";
    case EACCES:
    case EPERM: return "permission_denied";
#if defined(EROFS)
    case EROFS: return "permission_denied";
#endif
    case EEXIST: return "already_exists";
    case ENOTDIR: return "not_directory";
    case EISDIR: return "is_directory";
    case EINVAL:
    case ENAMETOOLONG: return "invalid_path";
    case ENOMEM:
    case ENOSPC:
    case EMFILE:
    case ENFILE: return "resource_exhausted";
#if defined(ENOTSUP)
    case ENOTSUP: return "unsupported_operation";
#endif
    default: return "io_error";
    }
}

static FILE *system_file_open(Om_Runtime_System_Task *task,
                              const char *mode,
                              Om_Status *status)
{
    if (task == NULL || task->path == NULL || status == NULL) return NULL;
#if defined(_WIN32)
    const size_t path_length = strlen(task->path);
    if (path_length >= (size_t)INT_MAX) {
        *status = OM_STATUS_OUT_OF_RANGE;
        return NULL;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        *status = OM_STATUS_IO_ERROR;
        return NULL;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        *status = OM_STATUS_OUT_OF_MEMORY;
        return NULL;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        *status = OM_STATUS_IO_ERROR;
        return NULL;
    }
    FILE *file = NULL;
    const wchar_t *wide_mode = strcmp(mode, "rb") == 0 ? L"rb"
        : strcmp(mode, "ab") == 0 ? L"ab" : L"wb";
    const errno_t open_status = _wfopen_s(&file, wide_path, wide_mode);
    const DWORD attributes = GetFileAttributesW(wide_path);
    if ((open_status != 0 || file == NULL)
        && attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && strcmp(mode, "rb") != 0) {
        task->error_code = "is_directory";
    } else if (open_status != 0 || file == NULL) {
        task->error_code = system_file_error_from_errno((int)open_status);
    }
    om_free(wide_path);
    *status = open_status != 0 || file == NULL
        ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
    return file;
#else
    FILE *file = fopen(task->path, mode);
    if (file == NULL) {
        const int open_error = errno;
        struct stat info;
        if (strcmp(mode, "rb") != 0
            && stat(task->path, &info) == 0 && S_ISDIR(info.st_mode)) {
            task->error_code = "is_directory";
        } else {
            task->error_code = system_file_error_from_errno(open_error);
        }
    }
    *status = file == NULL ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
    return file;
#endif
}

static Om_Status system_file_read(Om_Runtime_System_Task *task)
{

    Om_Status status = OM_STATUS_OK;
    FILE *file = system_file_open(task, "rb", &status);

    if (file == NULL) return status;
    size_t capacity = 0;
    for (;;) {
        if (task->result_length == OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
            const int extra = fgetc(file);
            if (extra != EOF) {
                task->error_code = "resource_exhausted";
                status = OM_STATUS_OUT_OF_RANGE;
            } else if (ferror(file)) {
                task->error_code = system_file_error_from_errno(errno);
                status = OM_STATUS_IO_ERROR;
            }
            break;
        }
        if (task->result_length == capacity) {
            size_t next_capacity = capacity == 0 ? 8192 : capacity;
            if (capacity != 0) {
                if (capacity > SIZE_MAX / 2) {
                    task->error_code = "resource_exhausted";
                    status = OM_STATUS_OUT_OF_RANGE;
                    break;
                }
                next_capacity = capacity * 2;
            }
            if (next_capacity > OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
                next_capacity = OM_SYSTEM_FILE_MAX_TEXT_BYTES;
            }
            uint8_t *next = om_realloc_array(task->result_bytes,
                next_capacity, sizeof(*next));
            if (next == NULL) {
                task->error_code = "resource_exhausted";
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            task->result_bytes = next;
            capacity = next_capacity;
        }
        const size_t available = capacity - task->result_length;
        const size_t received = fread(task->result_bytes + task->result_length,
            1, available, file);
        task->result_length += received;
        if (ferror(file)) {
            task->error_code = system_file_error_from_errno(errno);
            status = OM_STATUS_IO_ERROR;
            break;
        }
        if (feof(file)) break;
        if (received == 0) {
            task->error_code = "io_error";
            status = OM_STATUS_IO_ERROR;
            break;
        }
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        task->error_code = system_file_error_from_errno(errno);
        status = OM_STATUS_IO_ERROR;
    }

    return status;
}

static Om_Status system_task_copy_string_list(
    Om_Execution_Context *context,
    const void *list_object,
    char ***items_out,
    size_t *count_out,
    size_t *bytes_out)
{
    if (context == NULL || list_object == NULL || items_out == NULL
        || count_out == NULL || bytes_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *items_out = NULL;
    *count_out = 0;
    *bytes_out = 0;
    size_t count = 0;
    Om_Status status = om_runtime_list_length(context, list_object, &count);
    if (status != OM_STATUS_OK) return status;
    if (count > OM_SYSTEM_PROCESS_MAX_ARGUMENTS) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    char **items = count == 0 ? NULL : om_alloc_array(count + 1,
                                                       sizeof(*items));
    if (count != 0 && items == NULL) return OM_STATUS_OUT_OF_MEMORY;
    if (items != NULL) memset(items, 0, (count + 1) * sizeof(*items));
    size_t bytes = 0;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        Om_Runtime_Value item = {0};
        size_t item_length = 0;
        status = om_runtime_list_get(context, list_object, index, &item);
        if (status == OM_STATUS_OK) {
            status = system_task_copy_string(context, item.managed, &items[index],
                                              &item_length);
        }
        if (status == OM_STATUS_OK
            && (item_length > OM_SYSTEM_PROCESS_MAX_ARGUMENT_BYTES - bytes)) {
            status = OM_STATUS_OUT_OF_RANGE;
        }
        if (status == OM_STATUS_OK) bytes += item_length;
    }
    if (status != OM_STATUS_OK) {
        for (size_t index = 0; index < count; ++index) om_free(items[index]);
        om_free(items);
        return status;
    }
    *items_out = items;
    *count_out = count;
    *bytes_out = bytes;
    return OM_STATUS_OK;
}

static Om_Status system_file_write(Om_Runtime_System_Task *task, bool append)
{
    if (task->input_oversized
        || task->input_length > OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = OM_STATUS_OK;
    FILE *file = system_file_open(task, append ? "ab" : "wb", &status);
    if (file == NULL) return status;
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, file)
            != task->input_length) {
        task->error_code = system_file_error_from_errno(errno);
        status = OM_STATUS_IO_ERROR;
    }
    if (status == OM_STATUS_OK && fflush(file) != 0) {
        task->error_code = system_file_error_from_errno(errno);
        status = OM_STATUS_IO_ERROR;
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        task->error_code = system_file_error_from_errno(errno);
        status = OM_STATUS_IO_ERROR;
    }
    return status;
}

static Om_Status system_file_exists(Om_Runtime_System_Task *task)
{
    if (task == NULL || task->path == NULL) return OM_STATUS_INVALID_ARGUMENT;
#if defined(_WIN32)
    const size_t path_length = strlen(task->path);
    if (path_length >= (size_t)INT_MAX) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const DWORD attributes = GetFileAttributesW(wide_path);
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    om_free(wide_path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        task->result_boolean = true;
        return OM_STATUS_OK;
    }
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        task->result_boolean = false;
        return OM_STATUS_OK;
    }
    task->error_code = error == ERROR_ACCESS_DENIED
        ? "permission_denied" : "io_error";
    return OM_STATUS_IO_ERROR;
#else
    struct stat info;
    if (stat(task->path, &info) == 0) {
        task->result_boolean = true;
        return OM_STATUS_OK;
    }
    const int error = errno;
    if (error == ENOENT || error == ENOTDIR) {
        task->result_boolean = false;
        return OM_STATUS_OK;
    }
    task->error_code = system_file_error_from_errno(error);
    return OM_STATUS_IO_ERROR;
#endif
}

static Om_Status system_file_delete(Om_Runtime_System_Task *task)
{
    if (task == NULL || task->path == NULL) return OM_STATUS_INVALID_ARGUMENT;
#if defined(_WIN32)
    const size_t path_length = strlen(task->path);
    if (path_length >= (size_t)INT_MAX) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const DWORD attributes = GetFileAttributesW(wide_path);
    if (attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        om_free(wide_path);
        task->error_code = "is_directory";
        return OM_STATUS_IO_ERROR;
    }
    const BOOL removed = DeleteFileW(wide_path);
    const DWORD error = removed ? ERROR_SUCCESS : GetLastError();
    om_free(wide_path);
    if (removed) return OM_STATUS_OK;
    task->error_code = error == ERROR_FILE_NOT_FOUND
        || error == ERROR_PATH_NOT_FOUND ? "not_found"
        : error == ERROR_ACCESS_DENIED ? "permission_denied" : "io_error";
    return OM_STATUS_IO_ERROR;
#else
    if (unlink(task->path) == 0) return OM_STATUS_OK;
    task->error_code = system_file_error_from_errno(errno);
    return OM_STATUS_IO_ERROR;
#endif
}

static Om_Status system_directory_create_one(Om_Runtime_System_Task *task,
                                             const char *path,
                                             const wchar_t *wide_path)
{
#if defined(_WIN32)
    (void)path;
    DWORD attributes = GetFileAttributesW(wide_path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return OM_STATUS_OK;
        task->error_code = "already_exists";
        return OM_STATUS_IO_ERROR;
    }
    if (CreateDirectoryW(wide_path, NULL)) return OM_STATUS_OK;
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        attributes = GetFileAttributesW(wide_path);
        if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return OM_STATUS_OK;
        }
        task->error_code = "already_exists";
    } else if (error == ERROR_ACCESS_DENIED) {
        task->error_code = "permission_denied";
    } else if (error == ERROR_PATH_NOT_FOUND) {
        task->error_code = "not_found";
    } else if (error == ERROR_INVALID_NAME
               || error == ERROR_FILENAME_EXCED_RANGE) {
        task->error_code = "invalid_path";
    } else if (error == ERROR_NOT_ENOUGH_MEMORY
               || error == ERROR_DISK_FULL) {
        task->error_code = "resource_exhausted";
    } else {
        task->error_code = "io_error";
    }
    return OM_STATUS_IO_ERROR;
#else
    (void)wide_path;
    if (mkdir(path, 0777) == 0) return OM_STATUS_OK;
    const int error = errno;
    if (error == EEXIST) {
        struct stat info;
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
            return OM_STATUS_OK;
        }
        task->error_code = "already_exists";
    } else {
        task->error_code = system_file_error_from_errno(error);
    }
    return OM_STATUS_IO_ERROR;
#endif
}

static Om_Status system_directory_create(Om_Runtime_System_Task *task)
{
    if (task == NULL || task->path == NULL) return OM_STATUS_INVALID_ARGUMENT;
#if defined(_WIN32)
    const size_t path_length = strlen(task->path);
    if (path_length >= (size_t)INT_MAX) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    size_t wide_path_length = (size_t)wide_length - 1;
    size_t root_length = 0;
    if (wide_path[0] == L'\\' && wide_path[1] == L'\\') {
        size_t cursor = 2;
        while (cursor < wide_path_length && wide_path[cursor] != L'\\'
               && wide_path[cursor] != L'/') ++cursor;
        while (cursor < wide_path_length && (wide_path[cursor] == L'\\'
               || wide_path[cursor] == L'/')) ++cursor;
        while (cursor < wide_path_length && wide_path[cursor] != L'\\'
               && wide_path[cursor] != L'/') ++cursor;
        root_length = cursor;
    } else if (wide_path_length >= 2 && wide_path[1] == L':') {
        root_length = wide_path_length >= 3 && (wide_path[2] == L'\\'
            || wide_path[2] == L'/') ? 3 : 2;
    } else if (wide_path[0] == L'\\' || wide_path[0] == L'/') {
        root_length = 1;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = root_length; index < wide_path_length; ++index) {
        if (wide_path[index] != L'\\' && wide_path[index] != L'/') continue;
        if (index == 0) continue;
        const wchar_t saved = wide_path[index];
        wide_path[index] = L'\0';
        status = system_directory_create_one(task, NULL, wide_path);
        wide_path[index] = saved;
        if (status != OM_STATUS_OK) break;
    }
    if (status == OM_STATUS_OK) {
        status = system_directory_create_one(task, NULL, wide_path);
    }
    om_free(wide_path);
    return status;
#else
    const size_t path_length = strlen(task->path);
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; index < path_length; ++index) {
        if (task->path[index] != '/') continue;
        if (index == 0) continue;
        const char saved = task->path[index];
        task->path[index] = '\0';
        status = system_directory_create_one(task, task->path, NULL);
        task->path[index] = saved;
        if (status != OM_STATUS_OK) return status;
    }
    return system_directory_create_one(task, task->path, NULL);
#endif
}

static Om_Status system_task_add_text_item(Om_Runtime_System_Task *task,
                                           const char *bytes,
                                           size_t length)
{
    if (task->result_item_count >= OM_SYSTEM_DIRECTORY_MAX_ENTRIES
        || length > OM_SYSTEM_FILE_MAX_TEXT_BYTES - task->result_item_bytes) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (om_utf8_validate((Om_Const_Byte_Slice){
            (const uint8_t *)bytes, length}) != OM_STATUS_OK) {
        task->error_code = "invalid_utf8";
        return OM_STATUS_IO_ERROR;
    }
    if (task->result_item_count == task->result_item_capacity) {
        size_t next_capacity = task->result_item_capacity == 0
            ? 16 : task->result_item_capacity * 2;
        if (next_capacity > OM_SYSTEM_DIRECTORY_MAX_ENTRIES) {
            next_capacity = OM_SYSTEM_DIRECTORY_MAX_ENTRIES;
        }
        Om_Runtime_System_Text_Item *next = om_realloc_array(
            task->result_items, next_capacity, sizeof(*next));
        if (next == NULL) {
            task->error_code = "resource_exhausted";
            return OM_STATUS_OUT_OF_MEMORY;
        }
        task->result_items = next;
        task->result_item_capacity = next_capacity;
    }
    char *copy = om_alloc(length + 1);
    if (copy == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (length != 0) (void)memcpy(copy, bytes, length);
    copy[length] = '\0';
    task->result_items[task->result_item_count++] =
        (Om_Runtime_System_Text_Item){copy, length};
    task->result_item_bytes += length;
    return OM_STATUS_OK;
}

static int system_text_item_compare(const void *left_pointer,
                                    const void *right_pointer)
{
    const Om_Runtime_System_Text_Item *left = left_pointer;
    const Om_Runtime_System_Text_Item *right = right_pointer;
    const size_t common = left->length < right->length
        ? left->length : right->length;
    const int order = memcmp(left->bytes, right->bytes, common);
    if (order != 0) return order;
    return left->length < right->length ? -1
        : left->length > right->length ? 1 : 0;
}

static Om_Status system_directory_list(Om_Runtime_System_Task *task)
{
    if (task == NULL || task->path == NULL) return OM_STATUS_INVALID_ARGUMENT;
#if defined(_WIN32)
    const size_t path_length = strlen(task->path);
    if (path_length >= (size_t)INT_MAX) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length + 2,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    size_t wide_path_length = (size_t)wide_length - 1;
    const DWORD attributes = GetFileAttributesW(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        om_free(wide_path);
        task->error_code = error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND ? "not_found"
            : error == ERROR_ACCESS_DENIED ? "permission_denied" : "io_error";
        return OM_STATUS_IO_ERROR;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        om_free(wide_path);
        task->error_code = "not_directory";
        return OM_STATUS_IO_ERROR;
    }
    if (wide_path_length != 0 && wide_path[wide_path_length - 1] != L'\\'
        && wide_path[wide_path_length - 1] != L'/') {
        wide_path[wide_path_length++] = L'\\';
    }
    wide_path[wide_path_length] = L'*';
    wide_path[wide_path_length + 1] = L'\0';
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(wide_path, &entry);
    om_free(wide_path);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            task->error_code = error == ERROR_ACCESS_DENIED
                ? "permission_denied" : "io_error";
            return OM_STATUS_IO_ERROR;
        }
    } else {
        Om_Status status = OM_STATUS_OK;
        for (;;) {
            const wchar_t *name = entry.cFileName;
            if (wcscmp(name, L".") != 0 && wcscmp(name, L"..") != 0) {
                const int utf8_length = WideCharToMultiByte(CP_UTF8,
                    WC_ERR_INVALID_CHARS, name, -1, NULL, 0, NULL, NULL);
                if (utf8_length <= 0) {
                    task->error_code = "invalid_utf8";
                    status = OM_STATUS_IO_ERROR;
                } else {
                    char *utf8_name = om_alloc((size_t)utf8_length);
                    if (utf8_name == NULL) {
                        task->error_code = "resource_exhausted";
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        const int written = WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS, name, -1, utf8_name,
                            utf8_length, NULL, NULL);
                        if (written != utf8_length) {
                            task->error_code = "invalid_utf8";
                            status = OM_STATUS_IO_ERROR;
                        } else {
                            status = system_task_add_text_item(task, utf8_name,
                                (size_t)utf8_length - 1);
                        }
                        om_free(utf8_name);
                    }
                }
            }
            if (status != OM_STATUS_OK) break;
            if (!FindNextFileW(search, &entry)) {
                const DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    task->error_code = "io_error";
                    status = OM_STATUS_IO_ERROR;
                }
                break;
            }
        }
        (void)FindClose(search);
        if (status != OM_STATUS_OK) return status;
    }
#else
    DIR *directory = opendir(task->path);
    if (directory == NULL) {
        task->error_code = system_file_error_from_errno(errno);
        return OM_STATUS_IO_ERROR;
    }
    Om_Status status = OM_STATUS_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                task->error_code = system_file_error_from_errno(errno);
                status = OM_STATUS_IO_ERROR;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) continue;
        status = system_task_add_text_item(task, entry->d_name,
                                            strlen(entry->d_name));
        if (status != OM_STATUS_OK) break;
    }
    if (closedir(directory) != 0 && status == OM_STATUS_OK) {
        task->error_code = system_file_error_from_errno(errno);
        status = OM_STATUS_IO_ERROR;
    }
    if (status != OM_STATUS_OK) return status;
#endif
    if (task->result_item_count > 1) {
        qsort(task->result_items, task->result_item_count,
              sizeof(*task->result_items), system_text_item_compare);
    }
    return OM_STATUS_OK;
}

static Om_Status system_file_stat(Om_Runtime_System_Task *task)
{
    if (task == NULL || task->path == NULL) return OM_STATUS_INVALID_ARGUMENT;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    const char *kind = "other";
#if defined(_WIN32)
    const size_t path_length = strlen(task->path);
    if (path_length >= (size_t)INT_MAX) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        task->error_code = "invalid_path";
        return OM_STATUS_IO_ERROR;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        task->path, (int)path_length + 1, wide_path, wide_length);
    struct _stat64 info;
    const int stat_status = converted == wide_length
        ? _wstat64(wide_path, &info) : -1;
    const int stat_error = errno;
    om_free(wide_path);
    if (stat_status != 0) {
        task->error_code = converted != wide_length ? "invalid_path"
            : system_file_error_from_errno(stat_error);
        return OM_STATUS_IO_ERROR;
    }
    if (info.st_size < 0) {
        task->error_code = "unsupported_operation";
        return OM_STATUS_IO_ERROR;
    }
    size_bytes = (uint64_t)info.st_size;
    if ((info.st_mode & _S_IFMT) == _S_IFREG) kind = "file";
    else if ((info.st_mode & _S_IFMT) == _S_IFDIR) kind = "directory";
    if ((int64_t)info.st_mtime > INT64_MAX / 1000
        || (int64_t)info.st_mtime < INT64_MIN / 1000) {
        task->error_code = "unsupported_operation";
        return OM_STATUS_IO_ERROR;
    }
    modified_unix_ms = (int64_t)info.st_mtime * 1000;
#else
    struct stat info;
    if (stat(task->path, &info) != 0) {
        task->error_code = system_file_error_from_errno(errno);
        return OM_STATUS_IO_ERROR;
    }
    if (info.st_size < 0) {
        task->error_code = "unsupported_operation";
        return OM_STATUS_IO_ERROR;
    }
    size_bytes = (uint64_t)info.st_size;
    if (S_ISREG(info.st_mode)) kind = "file";
    else if (S_ISDIR(info.st_mode)) kind = "directory";
    const int64_t seconds = (int64_t)info.st_mtime;
    if (seconds > INT64_MAX / 1000 || seconds < INT64_MIN / 1000) {
        task->error_code = "unsupported_operation";
        return OM_STATUS_IO_ERROR;
    }
    modified_unix_ms = seconds * 1000;
#endif
    char size_text[32];
    char modified_text[32];
    const int size_length = snprintf(size_text, sizeof(size_text), "%" PRIu64,
                                     size_bytes);
    const int modified_length = snprintf(modified_text, sizeof(modified_text),
        "%" PRId64, modified_unix_ms);
    if (size_length < 0 || (size_t)size_length >= sizeof(size_text)
        || modified_length < 0
        || (size_t)modified_length >= sizeof(modified_text)) {
        task->error_code = "io_error";
        return OM_STATUS_IO_ERROR;
    }
    Om_Status status = system_task_add_text_item(task, kind, strlen(kind));
    if (status == OM_STATUS_OK) {
        status = system_task_add_text_item(task, size_text,
                                            (size_t)size_length);
    }
    if (status == OM_STATUS_OK) {
        status = system_task_add_text_item(task, modified_text,
                                            (size_t)modified_length);
    }
    return status;
}

static Om_Status system_console_write(Om_Runtime_System_Task *task)
{
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, stdout)
            != task->input_length) {
        return OM_STATUS_IO_ERROR;
    }
    return fflush(stdout) == 0 ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}

static bool system_logger_wait_turn(Om_Runtime_System_Task *task)
{
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);
    while (!system->shutting_down
           && task->sequence != system->serving_logger_sequence) {
        actor_condition_wait(system);
    }
    const bool ready = !system->shutting_down;
    actor_mutex_unlock(system);
    return ready;
}

static Om_Status system_logger_write(Om_Runtime_System_Task *task)
{
    if (!system_logger_wait_turn(task)) return OM_STATUS_CANCELLED;
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, stdout)
            != task->input_length) {
        return OM_STATUS_IO_ERROR;
    }
    return fflush(stdout) == 0 ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}

static Om_Status system_logger_flush(Om_Runtime_System_Task *task)
{
    if (!system_logger_wait_turn(task)) return OM_STATUS_CANCELLED;
    return fflush(stdout) == 0 ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}

static bool system_console_is_shutting_down(Om_Runtime_Actor_System *system)
{
    actor_mutex_lock(system);
    const bool shutting_down = system->shutting_down;
    actor_mutex_unlock(system);
    return shutting_down;
}

static bool system_console_wait_turn(Om_Runtime_System_Task *task)
{
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);
    while (!system->shutting_down
           && task->sequence != system->serving_console_read_sequence) {
        actor_condition_wait(system);
    }
    const bool ready = !system->shutting_down;
    actor_mutex_unlock(system);
    return ready;
}

#if defined(_WIN32)
static Om_Status system_console_read_wide_character(
    Om_Runtime_System_Task *task,
    HANDLE input,
    WCHAR *character,
    bool *end_of_input)
{
    for (;;) {
        if (system_console_is_shutting_down(task->system)) {
            return OM_STATUS_CANCELLED;
        }
        const DWORD wait_status = WaitForSingleObject(input, 50);
        if (wait_status == WAIT_TIMEOUT) continue;
        if (wait_status != WAIT_OBJECT_0) return OM_STATUS_IO_ERROR;
        DWORD count = 0;
        if (!ReadConsoleW(input, character, 1, &count, NULL)) {
            return OM_STATUS_IO_ERROR;
        }
        *end_of_input = count == 0;
        return OM_STATUS_OK;
    }
}
#endif

static Om_Status system_console_next_byte(
    Om_Runtime_System_Task *task,
    uint8_t *byte_value,
    bool *newline,
    bool *backspace,
    bool *end_of_input)
{
    *newline = false;
    *backspace = false;
    *end_of_input = false;
#if defined(_WIN32)
    if (task->console_input) {
        if (task->console_pending_index < task->console_pending_count) {
            *byte_value = task->console_pending_bytes[
                task->console_pending_index++];
            return OM_STATUS_OK;
        }
        task->console_pending_count = 0;
        task->console_pending_index = 0;
        WCHAR first = 0;
        Om_Status status = system_console_read_wide_character(task,
            GetStdHandle(STD_INPUT_HANDLE), &first, end_of_input);
        if (status != OM_STATUS_OK || *end_of_input) return status;
        if (first == L'\r' || first == L'\n') {
            *newline = true;
            return OM_STATUS_OK;
        }
        if (first == L'\b') {
            *backspace = true;
            return OM_STATUS_OK;
        }
        WCHAR pair[2] = {first, 0};
        int pair_length = 1;
        if (first >= 0xd800 && first <= 0xdbff) {
            WCHAR second = 0;
            status = system_console_read_wide_character(task,
                GetStdHandle(STD_INPUT_HANDLE), &second, end_of_input);
            if (status != OM_STATUS_OK || *end_of_input) {
                return status == OM_STATUS_OK ? OM_STATUS_IO_ERROR : status;
            }
            if (second < 0xdc00 || second > 0xdfff) {
                task->error_code = "invalid_utf8";
                return OM_STATUS_IO_ERROR;
            }
            pair[1] = second;
            pair_length = 2;
        } else if (first >= 0xdc00 && first <= 0xdfff) {
            task->error_code = "invalid_utf8";
            return OM_STATUS_IO_ERROR;
        }
        const int encoded_length = WideCharToMultiByte(CP_UTF8,
            WC_ERR_INVALID_CHARS, pair, pair_length,
            (char *)task->console_pending_bytes,
            (int)sizeof(task->console_pending_bytes), NULL, NULL);
        if (encoded_length <= 0) {
            task->error_code = "invalid_utf8";
            return OM_STATUS_IO_ERROR;
        }
        task->console_pending_count = (size_t)encoded_length;
        *byte_value = task->console_pending_bytes[0];
        task->console_pending_index = 1;
        return OM_STATUS_OK;
    }
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == NULL || input == INVALID_HANDLE_VALUE) {
        return OM_STATUS_IO_ERROR;
    }
    const DWORD input_type = GetFileType(input);
    if (input_type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) {
        return OM_STATUS_IO_ERROR;
    }
    const bool input_pipe = input_type == FILE_TYPE_PIPE;
    for (;;) {
        if (system_console_is_shutting_down(task->system)) {
            return OM_STATUS_CANCELLED;
        }
        if (input_pipe) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
                const DWORD error_code = GetLastError();
                if (error_code == ERROR_BROKEN_PIPE
                    || error_code == ERROR_HANDLE_EOF) {
                    *end_of_input = true;
                    return OM_STATUS_OK;
                }
                return OM_STATUS_IO_ERROR;
            }
            if (available == 0) {
                Sleep(50);
                continue;
            }
        }
        DWORD count = 0;
        if (!ReadFile(input, byte_value, 1, &count, NULL)) {
            const DWORD error_code = GetLastError();
            if (error_code == ERROR_BROKEN_PIPE
                || error_code == ERROR_HANDLE_EOF) {
                *end_of_input = true;
                return OM_STATUS_OK;
            }
            return OM_STATUS_IO_ERROR;
        }
        *end_of_input = count == 0;
        *newline = count != 0 && *byte_value == '\n';
        return OM_STATUS_OK;
    }
#else
    for (;;) {
        if (system_console_is_shutting_down(task->system)) {
            return OM_STATUS_CANCELLED;
        }
        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
        };
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return OM_STATUS_IO_ERROR;
        }
        if (ready == 0) continue;
        const ssize_t count = read(STDIN_FILENO, byte_value, 1);
        if (count < 0) {
            if (errno == EINTR) continue;
            return OM_STATUS_IO_ERROR;
        }
        *end_of_input = count == 0;
        *newline = count != 0 && *byte_value == '\n';
        return OM_STATUS_OK;
    }
#endif
}

static Om_Status system_console_line_append_byte(
    Om_Runtime_System_Task *task,
    uint8_t byte_value)
{
    if (task->result_length >= OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
        task->input_oversized = true;
        return OM_STATUS_OK;
    }
    if (task->result_length == task->result_capacity) {
        size_t next_capacity = task->result_capacity == 0 ? 256
            : task->result_capacity * 2;
        if (next_capacity > OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
            next_capacity = OM_SYSTEM_FILE_MAX_TEXT_BYTES;
        }
        uint8_t *next = om_realloc_array(task->result_bytes,
                                         next_capacity, sizeof(*next));
        if (next == NULL) return OM_STATUS_OUT_OF_MEMORY;
        task->result_bytes = next;
        task->result_capacity = next_capacity;
    }
    task->result_bytes[task->result_length++] = byte_value;
    return OM_STATUS_OK;
}

static Om_Status system_console_read_line(Om_Runtime_System_Task *task)
{
    if (!system_console_wait_turn(task)) return OM_STATUS_CANCELLED;
#if defined(_WIN32)
    DWORD previous_mode = 0;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == NULL || input == INVALID_HANDLE_VALUE) {
        task->error_code = "io_error";
        return OM_STATUS_IO_ERROR;
    }
    if (GetConsoleMode(input, &previous_mode)) {
        task->console_input = true;
        if (!SetConsoleMode(input,
                previous_mode & ~(DWORD)ENABLE_LINE_INPUT)) {
            task->error_code = "io_error";
            return OM_STATUS_IO_ERROR;
        }
    }
#endif
    Om_Status status = OM_STATUS_OK;
    bool line_terminated = false;
    bool end_of_input = false;
    for (;;) {
        uint8_t byte_value = 0;
        bool newline = false;
        bool backspace = false;
        status = system_console_next_byte(task, &byte_value, &newline,
                                          &backspace, &end_of_input);
        if (status != OM_STATUS_OK || end_of_input) break;
        if (newline) {
            line_terminated = true;
            break;
        }
        if (backspace) {
            if (task->result_length != 0) {
                --task->result_length;
                while (task->result_length != 0
                       && (task->result_bytes[task->result_length] & 0xc0) == 0x80) {
                    --task->result_length;
                }
            }
            continue;
        }
        status = system_console_line_append_byte(task, byte_value);
        if (status != OM_STATUS_OK) break;
    }
#if defined(_WIN32)
    if (task->console_input) {
        (void)SetConsoleMode(input, previous_mode);
        task->console_input = false;
    }
#endif
    if (status != OM_STATUS_OK) {
        if (status == OM_STATUS_IO_ERROR && task->error_code == NULL) {
            task->error_code = "io_error";
        }
        return status;
    }
    if (task->input_oversized) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (!line_terminated && end_of_input && task->result_length == 0) {
        task->error_code = "end_of_input";
        return OM_STATUS_IO_ERROR;
    }
    if (line_terminated && task->result_length != 0
        && task->result_bytes[task->result_length - 1] == '\r') {
        --task->result_length;
    }
    if (om_utf8_validate((Om_Const_Byte_Slice){task->result_bytes,
                                               task->result_length})
        != OM_STATUS_OK) {
        task->error_code = "invalid_utf8";
        return OM_STATUS_IO_ERROR;
    }
    return OM_STATUS_OK;
}

static Om_Status system_clock_now(Om_Runtime_System_Task *task)
{
    if (task == NULL
        || (task->operation != OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW
            && task->operation != OM_SYSTEM_MESSAGE_ID_CLOCK_UTC_NOW)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    if (task->operation == OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW) {
        const ULONGLONG milliseconds = GetTickCount64();
        if (milliseconds > (ULONGLONG)INT64_MAX) {
            task->error_code = "out_of_range";
            return OM_STATUS_OUT_OF_RANGE;
        }
        task->result_integer = (int64_t)milliseconds;
        return OM_STATUS_OK;
    }
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks;
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    const uint64_t epoch_milliseconds = ticks.QuadPart / UINT64_C(10000);
    const uint64_t windows_epoch_milliseconds = UINT64_C(11644473600000);
    if (epoch_milliseconds >= windows_epoch_milliseconds) {
        task->result_integer = (int64_t)(epoch_milliseconds
            - windows_epoch_milliseconds);
    } else {
        task->result_integer = -(int64_t)(windows_epoch_milliseconds
            - epoch_milliseconds);
    }
    return OM_STATUS_OK;
#else
    const clockid_t clock_id = task->operation
            == OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW
        ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    struct timespec now;
    if (clock_gettime(clock_id, &now) != 0) {
        task->error_code = "io_error";
        return OM_STATUS_IO_ERROR;
    }
    const int64_t seconds = (int64_t)now.tv_sec;
    if (seconds > INT64_MAX / 1000 || seconds < INT64_MIN / 1000) {
        task->error_code = "out_of_range";
        return OM_STATUS_OUT_OF_RANGE;
    }
    const int64_t base_milliseconds = seconds * 1000;
    const int64_t subsecond_milliseconds = (int64_t)(now.tv_nsec / 1000000);
    if (base_milliseconds > INT64_MAX - subsecond_milliseconds) {
        task->error_code = "out_of_range";
        return OM_STATUS_OUT_OF_RANGE;
    }
    task->result_integer = base_milliseconds + subsecond_milliseconds;
    return OM_STATUS_OK;
#endif
}

static Om_Status system_secure_random_bytes(Om_Runtime_System_Task *task)
{
    if (task->requested_length < 0) {
        task->error_code = "invalid_length";
        return OM_STATUS_OUT_OF_RANGE;
    }
    if ((uint64_t)task->requested_length > OM_SYSTEM_RANDOM_MAX_BYTES) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t length = (size_t)task->requested_length;
    if (length == 0) {
        task->result_length = 0;
        return OM_STATUS_OK;
    }
    task->result_bytes = om_alloc(length);
    if (task->result_bytes == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    task->result_capacity = length;
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(NULL, task->result_bytes,
        (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        task->error_code = "entropy_unavailable";
        return OM_STATUS_IO_ERROR;
    }
#else
    size_t offset = 0;
    while (offset < length) {
        const ssize_t count = getrandom(task->result_bytes + offset,
                                        length - offset, GRND_NONBLOCK);
        if (count < 0) {
            if (errno == EINTR) continue;
            task->error_code = errno == ENOSYS ? "unsupported"
                                               : "entropy_unavailable";
            return errno == ENOSYS ? OM_STATUS_NOT_FOUND : OM_STATUS_IO_ERROR;
        }
        if (count == 0) {
            task->error_code = "entropy_unavailable";
            return OM_STATUS_IO_ERROR;
        }
        offset += (size_t)count;
    }
#endif
    task->result_length = length;
    return OM_STATUS_OK;
}

static Om_Status system_environment_store_text(Om_Runtime_System_Task *task,
                                                const char *text,
                                                size_t length)
{
    if (length > OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (om_utf8_validate((Om_Const_Byte_Slice){
            (const uint8_t *)text, length}) != OM_STATUS_OK) {
        task->error_code = "invalid_utf8";
        return OM_STATUS_IO_ERROR;
    }
    task->result_bytes = om_alloc(length == 0 ? 1 : length);
    if (task->result_bytes == NULL) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (length != 0) (void)memcpy(task->result_bytes, text, length);
    task->result_length = length;
    task->result_capacity = length;
    return OM_STATUS_OK;
}

#if defined(_WIN32)
static Om_Status system_environment_utf8_from_wide(Om_Runtime_System_Task *task,
                                                    const wchar_t *wide,
                                                    size_t wide_length)
{
    if (wide_length >= (size_t)INT_MAX) return OM_STATUS_OUT_OF_RANGE;
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide, (int)wide_length, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        task->error_code = "invalid_utf8";
        return OM_STATUS_IO_ERROR;
    }
    if ((size_t)needed > OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES) {
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
    }
    char *text = om_alloc((size_t)needed + 1);
    if (text == NULL) return OM_STATUS_OUT_OF_MEMORY;
    if (needed != 0 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide, (int)wide_length, text, needed, NULL, NULL) != needed) {
        om_free(text);
        task->error_code = "io_error";
        return OM_STATUS_IO_ERROR;
    }
    text[needed] = '\0';
    const Om_Status status = system_environment_store_text(task, text,
        (size_t)needed);
    om_free(text);
    return status;
}

static wchar_t *system_environment_wide_from_utf8(const char *text)
{
    const size_t length = strlen(text);
    if (length >= (size_t)INT_MAX) return NULL;
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, (int)length + 1, NULL, 0);
    if (needed <= 0) return NULL;
    wchar_t *wide = om_alloc_array((size_t)needed, sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
            (int)length + 1, wide, needed) != needed) {
        om_free(wide);
        return NULL;
    }
    return wide;
}
#endif

static Om_Status system_environment_run(Om_Runtime_System_Task *task)
{
    if (task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS) {
        Om_Execution_Context *context = task->system->root_context;
        for (size_t index = 0; index < context->command_line_argument_count;
             ++index) {
            const char *argument = context->command_line_arguments[index];
            const Om_Status status = system_task_add_text_item(task, argument,
                strlen(argument));
            if (status != OM_STATUS_OK) return status;
        }
        return OM_STATUS_OK;
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_GET) {
        if (task->error_code != NULL
            && strcmp(task->error_code, "invalid_name") == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (task->error_code != NULL
            && strcmp(task->error_code, "resource_exhausted") == 0) {
            return OM_STATUS_OUT_OF_RANGE;
        }
#if defined(_WIN32)
        wchar_t *name = system_environment_wide_from_utf8(task->path);
        if (name == NULL) {
            task->error_code = "invalid_name";
            return OM_STATUS_INVALID_ARGUMENT;
        }
        SetLastError(ERROR_SUCCESS);
        const DWORD needed = GetEnvironmentVariableW(name, NULL, 0);
        if (needed == 0) {
            const DWORD error = GetLastError();
            om_free(name);
            if (error == ERROR_SUCCESS) {
                return system_environment_store_text(task, "", 0);
            }
            task->error_code = error == ERROR_ENVVAR_NOT_FOUND
                ? "not_found" : "io_error";
            return error == ERROR_ENVVAR_NOT_FOUND
                ? OM_STATUS_NOT_FOUND : OM_STATUS_IO_ERROR;
        }
        wchar_t *value = om_alloc_array((size_t)needed, sizeof(*value));
        if (value == NULL) { om_free(name); return OM_STATUS_OUT_OF_MEMORY; }
        const DWORD length = GetEnvironmentVariableW(name, value, needed);
        om_free(name);
        if (length == 0 || length >= needed) {
            om_free(value);
            task->error_code = "io_error";
            return OM_STATUS_IO_ERROR;
        }
        const Om_Status status = system_environment_utf8_from_wide(task,
            value, (size_t)length);
        om_free(value);
        return status;
#else
        const char *value = getenv(task->path);
        if (value == NULL) {
            task->error_code = "not_found";
            return OM_STATUS_NOT_FOUND;
        }
        return system_environment_store_text(task, value, strlen(value));
#endif
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_CURRENT_DIRECTORY) {
#if defined(_WIN32)
        DWORD needed = GetCurrentDirectoryW(0, NULL);
        if (needed == 0) { task->error_code = "io_error"; return OM_STATUS_IO_ERROR; }
        wchar_t *wide = om_alloc_array((size_t)needed + 1, sizeof(*wide));
        if (wide == NULL) return OM_STATUS_OUT_OF_MEMORY;
        const DWORD length = GetCurrentDirectoryW(needed + 1, wide);
        const Om_Status status = length == 0 || length > needed
            ? OM_STATUS_IO_ERROR
            : system_environment_utf8_from_wide(task, wide, length);
        om_free(wide);
        if (status != OM_STATUS_OK) task->error_code = "io_error";
        return status;
#else
        size_t capacity = 256;
        while (capacity <= OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES) {
            char *path = om_alloc(capacity);
            if (path == NULL) return OM_STATUS_OUT_OF_MEMORY;
            if (getcwd(path, capacity) != NULL) {
                const Om_Status status = system_environment_store_text(task,
                    path, strlen(path));
                om_free(path);
                return status;
            }
            const int error = errno;
            om_free(path);
            if (error != ERANGE) {
                task->error_code = "io_error";
                return OM_STATUS_IO_ERROR;
            }
            capacity *= 2;
        }
        task->error_code = "resource_exhausted";
        return OM_STATUS_OUT_OF_RANGE;
#endif
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH) {
#if defined(_WIN32)
        size_t capacity = 256;
        for (;;) {
            wchar_t *path = om_alloc_array(capacity, sizeof(*path));
            if (path == NULL) return OM_STATUS_OUT_OF_MEMORY;
            const DWORD length = GetModuleFileNameW(NULL, path,
                capacity > UINT32_MAX ? UINT32_MAX : (DWORD)capacity);
            if (length == 0) {
                om_free(path); task->error_code = "io_error";
                return OM_STATUS_IO_ERROR;
            }
            if ((size_t)length < capacity - 1) {
                const Om_Status status = system_environment_utf8_from_wide(
                    task, path, (size_t)length);
                om_free(path);
                return status;
            }
            om_free(path);
            if (capacity >= OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES / sizeof(*path)) {
                task->error_code = "resource_exhausted";
                return OM_STATUS_OUT_OF_RANGE;
            }
            capacity *= 2;
        }
#elif defined(__linux__)
        size_t capacity = 256;
        for (;;) {
            char *path = om_alloc(capacity);
            if (path == NULL) return OM_STATUS_OUT_OF_MEMORY;
            const ssize_t length = readlink("/proc/self/exe", path, capacity);
            if (length < 0) {
                om_free(path); task->error_code = "io_error";
                return OM_STATUS_IO_ERROR;
            }
            if ((size_t)length < capacity) {
                const Om_Status status = system_environment_store_text(task,
                    path, (size_t)length);
                om_free(path);
                return status;
            }
            om_free(path);
            if (capacity >= OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES) {
                task->error_code = "resource_exhausted";
                return OM_STATUS_OUT_OF_RANGE;
            }
            capacity *= 2;
        }
#else
        task->error_code = "unsupported";
        return OM_STATUS_NOT_FOUND;
#endif
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status system_task_run(void *context)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if (task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE) {
        return system_console_write(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE) {
        return system_logger_write(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH) {
        return system_logger_flush(task);
    }
    if (task->operation >= OM_SYSTEM_MESSAGE_ID_TCP_LISTEN
        && task->operation <= OM_SYSTEM_MESSAGE_ID_TCP_CLOSE) {
        return system_network_run(task);
    }
    if (task->operation >= OM_SYSTEM_MESSAGE_ID_UDP_CONNECT
        && task->operation <= OM_SYSTEM_MESSAGE_ID_UDP_CLOSE) {
        return system_network_run(task);
    }
    if (task->operation >= OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START
        && task->operation <= OM_SYSTEM_MESSAGE_ID_TLS_CLOSE) {
        return system_network_run(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE) {
        return system_dns_resolve(task);
    }
    if (task->operation >= OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN
        && task->operation <= OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT) {
        return system_process_run(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE) {
        return system_console_read_line(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW
        || task->operation == OM_SYSTEM_MESSAGE_ID_CLOCK_UTC_NOW) {
        return system_clock_now(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES) {
        return system_secure_random_bytes(task);
    }
    if (task->operation >= OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS
        && task->operation <= OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH) {
        return system_environment_run(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES) {
        return system_file_read(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_EXISTS) {
        return system_file_exists(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_DELETE) {
        return system_file_delete(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_CREATE) {
        return system_directory_create(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST) {
        return system_directory_list(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_STAT) {
        return system_file_stat(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE
        || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND
        || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE_BYTES
        || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES) {
        return system_file_write(task,
            task->operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND
            || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static void system_file_complete(void *context, Om_Status task_status)
{

    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->system == NULL) return;
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);

    if (task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE
        && task->sequence == system->serving_console_read_sequence) {
        ++system->serving_console_read_sequence;
        actor_condition_broadcast(system);
    }
    if ((task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE
         || task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH)
        && task->sequence == system->serving_logger_sequence) {
        ++system->serving_logger_sequence;
        actor_condition_broadcast(system);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE) {
        system->logger_pending_bytes = task->input_length
            <= system->logger_pending_bytes
                ? system->logger_pending_bytes - task->input_length : 0;
    }
    const bool network_create = task->operation
            == OM_SYSTEM_MESSAGE_ID_TCP_LISTEN
        || task->operation == OM_SYSTEM_MESSAGE_ID_TCP_CONNECT
        || task->operation == OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT
        || task->operation == OM_SYSTEM_MESSAGE_ID_UDP_CONNECT;
    if (network_create && task->network_resource_is_new) {
        if (system->pending_network_resources != 0) {
            --system->pending_network_resources;
        }
        if (task_status == OM_STATUS_OK && !system->shutting_down) {
            task->network_resource->next = system->network_resources;
            system->network_resources = task->network_resource;
            task->network_resource_registered = true;
            ++system->active_network_resources;
            task->result_integer = (int64_t)task->network_resource->id;
        } else if (task->network_resource->native_socket != -1) {
            network_close_native(network_native_socket(
                task->network_resource->native_socket));
            task->network_resource->native_socket = -1;
        }
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT
        && task->network_listener != NULL) {
        task->network_listener->busy_read = false;
        if (task->network_listener->closed
            && !task->network_listener->busy_write
            && !task->network_listener->native_closed
            && task->network_listener->native_socket != -1) {
            network_close_native(network_native_socket(
                task->network_listener->native_socket));
            task->network_listener->native_closed = true;
        }
    } else if (task->operation == OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START
               && task->network_resource != NULL) {
        task->network_resource->busy_tls = false;
        if (task_status == OM_STATUS_OK && !system->shutting_down
            && !task->network_resource->closed) {
            task->network_resource->tls_session = task->tls_session;
            task->tls_session = NULL;
        } else if (task_status == OM_STATUS_OK) {
            task_status = OM_STATUS_CANCELLED;
            task->error_code = "closed";
        }
    } else if ((task->operation == OM_SYSTEM_MESSAGE_ID_TLS_SEND
                || task->operation == OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE)
               && task->network_resource != NULL) {
        task->network_resource->busy_tls = false;
    } else if ((task->operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_TCP_SEND
                || task->operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_UDP_SEND)
               && task->network_resource != NULL) {
        if (task->operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE
            || task->operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE) {
            task->network_resource->busy_read = false;
        } else {
            task->network_resource->busy_write = false;
        }
        if (task->network_resource->closed
            && !task->network_resource->busy_read
            && !task->network_resource->busy_write
            && !task->network_resource->native_closed
            && task->network_resource->native_socket != -1) {
            network_close_native(network_native_socket(
                task->network_resource->native_socket));
            task->network_resource->native_closed = true;
        }
    }
    const bool process_spawn = task->operation
        == OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN;
    if (process_spawn && task->process_resource_is_new) {
        if (system->pending_process_resources != 0) {
            --system->pending_process_resources;
        }
        if (task_status == OM_STATUS_OK && !system->shutting_down) {
            task->process_resource->next = system->process_resources;
            system->process_resources = task->process_resource;
            task->process_resource_registered = true;
            ++system->active_process_resources;
            task->result_integer = (int64_t)task->process_resource->id;
        }
    }
    if (task->process_resource != NULL) {
        if (task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN
            || task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN) {
            task->process_resource->busy_stdin = false;
            if (task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN
                || (task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN
                    && task_status != OM_STATUS_OK
                    && task->error_code != NULL
                    && strcmp(task->error_code, "closed") == 0)) {
                task->process_resource->stdin_closed = true;
                task->process_resource->stdin_handle = -1;
            }
        } else if (task->operation
                       == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT) {
            task->process_resource->busy_stdout = false;
            if (task_status != OM_STATUS_OK && task->error_code != NULL
                && strcmp(task->error_code, "closed") == 0) {
                task->process_resource->stdout_handle = -1;
            }
        } else if (task->operation
                       == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR) {
            task->process_resource->busy_stderr = false;
            if (task_status != OM_STATUS_OK && task->error_code != NULL
                && strcmp(task->error_code, "closed") == 0) {
                task->process_resource->stderr_handle = -1;
            }
        } else if (task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT
                   && task_status == OM_STATUS_OK) {
            task->process_resource->busy_wait = false;
            task->process_resource->exited = true;
            task->process_resource->exit_code = task->result_integer;
#if defined(_WIN32)
            if (task->process_resource->native_process != -1) {
                (void)CloseHandle((HANDLE)task->process_resource->native_process);
            }
#endif
            task->process_resource->native_process = -1;
        } else if (task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT) {
            task->process_resource->busy_wait = false;
        }
    }

    if (!system->shutting_down) {
        if (task_status == OM_STATUS_OK
            && (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                || task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE)
            && om_utf8_validate((Om_Const_Byte_Slice){task->result_bytes,
                task->result_length}) != OM_STATUS_OK) {
            task->error_code = "invalid_utf8";
            task_status = OM_STATUS_IO_ERROR;
        }
        const Om_System_Message_Id completion_id =
            task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
                ? OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE
                ? OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? OM_SYSTEM_MESSAGE_ID_FILE_READ_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES
                ? OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE_BYTES
                ? OM_SYSTEM_MESSAGE_ID_FILE_WRITE_BYTES_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES
                ? OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND
                ? OM_SYSTEM_MESSAGE_ID_FILE_APPEND_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_EXISTS
                ? OM_SYSTEM_MESSAGE_ID_FILE_EXISTS_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_DELETE
                ? OM_SYSTEM_MESSAGE_ID_FILE_DELETE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_CREATE
                ? OM_SYSTEM_MESSAGE_ID_DIRECTORY_CREATE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST
                ? OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_STAT
                ? OM_SYSTEM_MESSAGE_ID_FILE_STAT_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW
                ? OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_CLOCK_UTC_NOW
                ? OM_SYSTEM_MESSAGE_ID_CLOCK_UTC_NOW_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES
                ? OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS
                ? OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_GET
                ? OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_GET_COMPLETED
            : task->operation
                    == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_CURRENT_DIRECTORY
                ? OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_CURRENT_DIRECTORY_COMPLETED
            : task->operation
                    == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH
                ? OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE
                ? OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE
                ? OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH
                ? OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TCP_LISTEN
                ? OM_SYSTEM_MESSAGE_ID_TCP_LISTEN_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TCP_CONNECT
                ? OM_SYSTEM_MESSAGE_ID_TCP_CONNECT_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT
                ? OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TCP_SEND
                ? OM_SYSTEM_MESSAGE_ID_TCP_SEND_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE
                ? OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TCP_CLOSE
                ? OM_SYSTEM_MESSAGE_ID_TCP_CLOSE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_UDP_CONNECT
                ? OM_SYSTEM_MESSAGE_ID_UDP_CONNECT_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_UDP_SEND
                ? OM_SYSTEM_MESSAGE_ID_UDP_SEND_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE
                ? OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_UDP_CLOSE
                ? OM_SYSTEM_MESSAGE_ID_UDP_CLOSE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START
                ? OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TLS_SEND
                ? OM_SYSTEM_MESSAGE_ID_TLS_SEND_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE
                ? OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT
                ? OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT_COMPLETED
                : OM_SYSTEM_MESSAGE_ID_FILE_WRITE_COMPLETED;
        const char *error_text = task->operation
                == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
            ? "io_error"
            : task->error_code == NULL ? "io_error" : task->error_code;
        const Om_Status delivery_status = actor_deliver_result_locked(system,
            task->recipient_id, completion_id, task_status == OM_STATUS_OK,
            task->result_boolean, task->result_integer,
            (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES
                || task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE
                || task->operation == OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES
                || task->operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT
                || task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR
                || task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_GET
                || task->operation
                    == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_CURRENT_DIRECTORY
                || task->operation
                    == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH)
                ? task->result_bytes : NULL,
            (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES
                || task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_READ_LINE
                || task->operation == OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES
                || task->operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE
                || task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT
                || task->operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR
                || task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_GET
                || task->operation
                    == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_CURRENT_DIRECTORY
                || task->operation
                    == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH)
                ? task->result_length : 0,
            (task->operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST
                || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_STAT
                || task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS
                || task->operation == OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE)
                ? task->result_items : NULL,
            (task->operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST
                || task->operation == OM_SYSTEM_MESSAGE_ID_FILE_STAT
                || task->operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS
                || task->operation == OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE)
                ? task->result_item_count : 0,
            error_text);

        if (delivery_status != OM_STATUS_OK) {
            actor_fail_locked(system, task->recipient_id);
        }
    }
    if (system->outstanding_messages != 0) {
        --system->outstanding_messages;
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

}

static void timer_task_complete(void *context, Om_Status task_status)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->system == NULL) return;
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);
    if (task_status == OM_STATUS_OK && !system->shutting_down) {
        const Om_Status delivery_status = actor_deliver_message_locked(system,
            task->recipient_id, task->payload_context, task->payload_object);
        if (delivery_status != OM_STATUS_OK) {
            actor_fail_locked(system, task->recipient_id);
        }
    }
    if (system->outstanding_messages != 0) {
        --system->outstanding_messages;
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);
}

static void system_task_destroy(void *context)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL) return;

    om_free(task->path);
    om_free(task->working_directory);

    for (size_t index = 0; index < task->process_argument_count; ++index) {
        om_free(task->process_arguments[index]);
    }
    om_free(task->process_arguments);
    for (size_t index = 0; index < task->process_environment_count; ++index) {
        om_free(task->process_environment[index]);
    }
    om_free(task->process_environment);

    om_free(task->input_bytes);

    om_free(task->result_bytes);

    for (size_t index = 0; index < task->result_item_count; ++index) {
        om_free(task->result_items[index].bytes);
    }
    om_free(task->result_items);

    if (task->payload_context != NULL) {
        om_runtime_context_destroy(task->payload_context);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START
        && task->tls_session != NULL) {
        om_tls_close(task->tls_session);
        task->tls_session = NULL;
    }
    if (task->network_resource_is_new
        && !task->network_resource_registered
        && task->network_resource != NULL) {
        if (task->network_resource->native_socket != -1) {
            network_close_native(network_native_socket(
                task->network_resource->native_socket));
        }
        om_free(task->network_resource);
    }
    if (task->process_resource_is_new
        && !task->process_resource_registered
        && task->process_resource != NULL) {
        process_resource_cleanup(task->process_resource);
        om_free(task->process_resource);
    }
    om_free(task);

}

Om_Status actor_send_file_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL) return OM_STATUS_NOT_FOUND;
    const Om_System_Message_Id operation = message_type->system_message_id;
    const size_t expected_fields = operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_EXISTS
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_DELETE
        || operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_CREATE
        || operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_STAT
        ? 2 : (operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE_BYTES
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES) ? 3 : 0;
    if (expected_fields == 0 || message_type->field_count != expected_fields) {
        return OM_STATUS_NOT_FOUND;
    }
    void *path_object = NULL;
    void *contents_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                              &path_object);
    if (status == OM_STATUS_OK
        && (operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE_BYTES
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES)) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &contents_object);
    }
    const size_t recipient_field = operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_READ_BYTES
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_EXISTS
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_DELETE
        || operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_CREATE
        || operation == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST
        || operation == OM_SYSTEM_MESSAGE_ID_FILE_STAT
        ? 1 : 2;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object,
            recipient_field, &recipient_id);
    }
    if (status != OM_STATUS_OK) return status;

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = operation,
        .recipient_id = recipient_id,
        .error_code = "io_error",
    };
    size_t path_length = 0;
    status = system_task_copy_string(source_context, path_object, &task->path,
                                     &path_length);
    if (status == OM_STATUS_OK
        && (operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND)) {
        Om_String_View contents_view = {0};
        status = om_runtime_string_view(source_context, contents_object,
                                        &contents_view);
        if (status == OM_STATUS_OK
            && contents_view.length > OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
            task->input_oversized = true;
        } else if (status == OM_STATUS_OK) {
            char *contents = NULL;
            status = system_task_copy_string(source_context, contents_object,
                &contents, &task->input_length);
            task->input_bytes = (uint8_t *)contents;
        }
    } else if (status == OM_STATUS_OK
        && (operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE_BYTES
            || operation == OM_SYSTEM_MESSAGE_ID_FILE_APPEND_BYTES)) {
        size_t byte_count = 0;
        status = om_runtime_list_length(source_context, contents_object,
                                        &byte_count);
        if (status == OM_STATUS_OK && byte_count > OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
            task->input_oversized = true;
        } else if (status == OM_STATUS_OK) {
            task->input_length = byte_count;
            task->input_bytes = byte_count == 0 ? NULL : om_alloc(byte_count);
            if (byte_count != 0 && task->input_bytes == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < byte_count; ++index) {
                Om_Runtime_Value byte_value = {0};
                status = om_runtime_list_get(source_context, contents_object,
                                             index, &byte_value);
                if (status == OM_STATUS_OK
                    && (byte_value.integer < 0 || byte_value.integer > 255)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                }
                if (status == OM_STATUS_OK) {
                    task->input_bytes[index] = (uint8_t)byte_value.integer;
                }
            }
        }
    }
    (void)path_length;
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        system_task_destroy(task);
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
        if (status == OM_STATUS_OUT_OF_RANGE
            || status == OM_STATUS_OUT_OF_MEMORY) {
            const Om_Status delivery_status = actor_deliver_result_locked(
                system, recipient_id,
                OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES_COMPLETED,
                false, false, 0, NULL, 0, NULL, 0,
                "resource_exhausted");
            if (delivery_status != OM_STATUS_OK) {
                actor_fail_locked(system, recipient_id);
                return delivery_status;
            }
            return OM_STATUS_OK;
        }
    }
    return status;
}

Om_Status actor_send_clock_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL || message_type->field_count != 1
        || (message_type->system_message_id
                != OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW
            && message_type->system_message_id
                != OM_SYSTEM_MESSAGE_ID_CLOCK_UTC_NOW)) {
        return OM_STATUS_NOT_FOUND;
    }
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object,
                                              0, &recipient_id);
    if (status != OM_STATUS_OK) return status;
    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = message_type->system_message_id,
        .recipient_id = recipient_id,
        .error_code = "io_error",
    };
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

Om_Status actor_send_secure_random_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL
        || message_type->system_message_id
            != OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES
        || message_type->field_count != 2) {
        return OM_STATUS_NOT_FOUND;
    }
    int64_t requested_length = 0;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object,
                                              0, &requested_length);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object,
                                       1, &recipient_id);
    }
    if (status != OM_STATUS_OK) return status;
    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES,
        .recipient_id = recipient_id,
        .requested_length = requested_length,
        .error_code = "entropy_unavailable",
    };
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

Om_Status actor_send_logger_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (system == NULL || source_context == NULL || message_type == NULL
        || message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_System_Message_Id operation = message_type->system_message_id;
    const bool write = operation == OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE;
    const bool flush = operation == OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH;
    if ((!write && !flush) || message_type->field_count != (write ? 2U : 1U)) {
        return OM_STATUS_NOT_FOUND;
    }
    uint64_t recipient_id = 0;
    void *text_object = NULL;
    Om_Status status = OM_STATUS_OK;
    if (write) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &text_object);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object,
                                       write ? 1 : 0, &recipient_id);
    }
    Om_String_View text = {0};
    if (status == OM_STATUS_OK && write) {
        status = om_runtime_string_view(source_context, text_object, &text);
    }
    if (status != OM_STATUS_OK) return status;
    if (write && text.length > OM_SYSTEM_FILE_MAX_TEXT_BYTES) {
        const Om_Status delivered = actor_deliver_result_locked(system,
            recipient_id, OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE_COMPLETED,
            false, false, 0, NULL, 0, NULL, 0, "resource_exhausted");
        if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient_id);
        return delivered;
    }
    if (system->blocking_pool == NULL) return OM_STATUS_CANCELLED;
    if (system->outstanding_messages == SIZE_MAX
        || system->next_logger_sequence == UINT64_MAX
        || (write && text.length > OM_SYSTEM_FILE_MAX_TEXT_BYTES
                - system->logger_pending_bytes)) {
        const Om_Status delivered = actor_deliver_result_locked(system,
            recipient_id, write ? OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE_COMPLETED
                                : OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH_COMPLETED,
            false, false, 0, NULL, 0, NULL, 0, "resource_exhausted");
        if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient_id);
        return delivered;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) {
        const Om_Status delivered = actor_deliver_result_locked(system,
            recipient_id, write ? OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE_COMPLETED
                                : OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH_COMPLETED,
            false, false, 0, NULL, 0, NULL, 0, "resource_exhausted");
        if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient_id);
        return delivered;
    }
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = operation,
        .recipient_id = recipient_id,
        .sequence = system->next_logger_sequence,
        .error_code = "io_error",
        .input_length = write ? text.length : 0,
    };
    if (write && text.length != 0) {
        task->input_bytes = om_alloc(text.length);
        if (task->input_bytes == NULL) {
            system_task_destroy(task);
            const Om_Status delivered = actor_deliver_result_locked(system,
                recipient_id, OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE_COMPLETED,
                false, false, 0, NULL, 0, NULL, 0, "resource_exhausted");
            if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient_id);
            return delivered;
        }
        (void)memcpy(task->input_bytes, text.data, text.length);
    }
    ++system->outstanding_messages;
    if (write) system->logger_pending_bytes += text.length;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        if (write) system->logger_pending_bytes -= text.length;
        system_task_destroy(task);
        actor_condition_broadcast(system);
        if (status == OM_STATUS_OUT_OF_RANGE
            || status == OM_STATUS_OUT_OF_MEMORY) {
            const Om_Status delivered = actor_deliver_result_locked(system,
                recipient_id,
                write ? OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE_COMPLETED
                      : OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH_COMPLETED,
                false, false, 0, NULL, 0, NULL, 0, "resource_exhausted");
            if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient_id);
            return delivered;
        }
        return status;
    }
    ++system->next_logger_sequence;
    return OM_STATUS_OK;
}

void actor_network_shutdown_locked(Om_Runtime_Actor_System *system)
{
    if (system == NULL) return;
    for (struct Om_Runtime_Network_Resource *resource =
             system->network_resources;
         resource != NULL; resource = resource->next) {
        resource->closed = true;
        if (!resource->native_closed && resource->native_socket != -1) {
            network_shutdown_native(
                network_native_socket(resource->native_socket));
        }
    }
    system->active_network_resources = 0;
}

void actor_network_destroy(Om_Runtime_Actor_System *system)
{
    if (system == NULL) return;
    struct Om_Runtime_Network_Resource *resource = system->network_resources;
    while (resource != NULL) {
        struct Om_Runtime_Network_Resource *next = resource->next;
        if (!resource->native_closed && resource->native_socket != -1) {
            network_close_native(network_native_socket(resource->native_socket));
        }
        om_tls_close(resource->tls_session);
        om_free(resource);
        resource = next;
    }
    system->network_resources = NULL;
#if defined(_WIN32)
    if (system->network_initialized) (void)WSACleanup();
#endif
    system->network_initialized = false;
}

void actor_process_shutdown_locked(Om_Runtime_Actor_System *system)
{
    if (system == NULL) return;
    bool needs_grace = false;
    for (struct Om_Runtime_Process_Resource *process = system->process_resources;
         process != NULL; process = process->next) {
        process->closed = true;
        if (process->native_process == -1 || process->exited) continue;
        needs_grace = true;
#if defined(_WIN32)
        (void)TerminateProcess((HANDLE)process->native_process, 1);
#else
        if (process->process_guard >= 0) {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
            (void)syscall(SYS_pidfd_send_signal, (int)process->process_guard,
                          SIGTERM, NULL, 0U);
#else
            (void)kill((pid_t)process->native_process, SIGTERM);
#endif
        } else {
            (void)kill((pid_t)process->native_process, SIGTERM);
        }
#endif
    }
#if !defined(_WIN32)
    if (needs_grace) {
        struct timespec grace = {.tv_sec = 0, .tv_nsec = 500000000};
        while (nanosleep(&grace, &grace) != 0 && errno == EINTR) {}
        for (struct Om_Runtime_Process_Resource *process = system->process_resources;
             process != NULL; process = process->next) {
          if (process->native_process != -1 && !process->exited) {
            if (process->process_guard >= 0) {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
                (void)syscall(SYS_pidfd_send_signal,
                    (int)process->process_guard, SIGKILL, NULL, 0U);
#else
                (void)kill((pid_t)process->native_process, SIGKILL);
#endif
            } else if (!process->busy_wait) {
                (void)kill((pid_t)process->native_process, SIGKILL);
            }
          }
        }
    }
#endif
    (void)needs_grace;
    system->active_process_resources = 0;
}

void actor_process_destroy(Om_Runtime_Actor_System *system)
{
    if (system == NULL) return;
    struct Om_Runtime_Process_Resource *process = system->process_resources;
    while (process != NULL) {
        struct Om_Runtime_Process_Resource *next = process->next;
        process_resource_cleanup(process);
        om_free(process);
        process = next;
    }
    system->process_resources = NULL;
    system->active_process_resources = 0;
    system->pending_process_resources = 0;
}

static struct Om_Runtime_Network_Resource *network_find_resource_locked(
    Om_Runtime_Actor_System *system,
    int64_t resource_id)
{
    if (resource_id <= 0) return NULL;
    for (struct Om_Runtime_Network_Resource *resource =
             system->network_resources;
         resource != NULL; resource = resource->next) {
        if (resource->id == (uint64_t)resource_id) return resource;
    }
    return NULL;
}

static Om_System_Message_Id network_completion_id(
    Om_System_Message_Id operation)
{
    switch (operation) {
    case OM_SYSTEM_MESSAGE_ID_TCP_LISTEN:
        return OM_SYSTEM_MESSAGE_ID_TCP_LISTEN_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TCP_CONNECT:
        return OM_SYSTEM_MESSAGE_ID_TCP_CONNECT_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT:
        return OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TCP_SEND:
        return OM_SYSTEM_MESSAGE_ID_TCP_SEND_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE:
        return OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TCP_CLOSE:
        return OM_SYSTEM_MESSAGE_ID_TCP_CLOSE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_UDP_CONNECT:
        return OM_SYSTEM_MESSAGE_ID_UDP_CONNECT_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_UDP_SEND:
        return OM_SYSTEM_MESSAGE_ID_UDP_SEND_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE:
        return OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_UDP_CLOSE:
        return OM_SYSTEM_MESSAGE_ID_UDP_CLOSE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE:
        return OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START:
        return OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TLS_SEND:
        return OM_SYSTEM_MESSAGE_ID_TLS_SEND_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE:
        return OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_TLS_CLOSE:
        return OM_SYSTEM_MESSAGE_ID_TLS_CLOSE_COMPLETED;
    default:
        return OM_SYSTEM_MESSAGE_ID_NONE;
    }
}

static Om_Status network_deliver_error_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient,
    Om_System_Message_Id operation,
    const char *error)
{
    const Om_Status status = actor_deliver_result_locked(system, recipient,
        network_completion_id(operation), false, false, 0, NULL, 0,
        NULL, 0, error);
    if (status != OM_STATUS_OK) actor_fail_locked(system, recipient);
    return status;
}

Om_Status actor_send_network_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (system == NULL || source_context == NULL || message_type == NULL
        || message_object == NULL) return OM_STATUS_INVALID_ARGUMENT;
    const Om_System_Message_Id operation = message_type->system_message_id;
    const bool listen_request = operation == OM_SYSTEM_MESSAGE_ID_TCP_LISTEN;
    const bool connect_request = operation == OM_SYSTEM_MESSAGE_ID_TCP_CONNECT;
    const bool accept_request = operation == OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT;
    const bool send_request = operation == OM_SYSTEM_MESSAGE_ID_TCP_SEND;
    const bool receive_request = operation == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE;
    const bool close_request = operation == OM_SYSTEM_MESSAGE_ID_TCP_CLOSE;
    const bool udp_connect_request = operation == OM_SYSTEM_MESSAGE_ID_UDP_CONNECT;
    const bool udp_send_request = operation == OM_SYSTEM_MESSAGE_ID_UDP_SEND;
    const bool udp_receive_request = operation == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE;
    const bool udp_close_request = operation == OM_SYSTEM_MESSAGE_ID_UDP_CLOSE;
    const bool dns_resolve_request = operation == OM_SYSTEM_MESSAGE_ID_DNS_RESOLVE;
    const bool tls_client_start_request =
        operation == OM_SYSTEM_MESSAGE_ID_TLS_CLIENT_START;
    const bool tls_send_request = operation == OM_SYSTEM_MESSAGE_ID_TLS_SEND;
    const bool tls_receive_request =
        operation == OM_SYSTEM_MESSAGE_ID_TLS_RECEIVE;
    const bool tls_close_request = operation == OM_SYSTEM_MESSAGE_ID_TLS_CLOSE;
    const bool any_send_request = send_request || udp_send_request
        || tls_send_request;
    const bool any_receive_request = receive_request || udp_receive_request
        || tls_receive_request;
    const bool any_close_request = close_request || udp_close_request
        || tls_close_request;
    const size_t expected_fields = listen_request ? 4
        : connect_request ? 3
        : udp_connect_request ? 4
        : dns_resolve_request ? 3
        : tls_client_start_request ? 3
        : (accept_request || any_close_request) ? 2
        : (any_send_request || any_receive_request) ? 3 : 0;
    if (expected_fields == 0 || message_type->field_count != expected_fields) {
        return OM_STATUS_NOT_FOUND;
    }
    uint64_t recipient = 0;
    int64_t resource_id = 0;
    int64_t port_or_limit = 0;
    int64_t backlog = 0;
    void *address_object = NULL;
    void *byte_list = NULL;
    Om_Status status = OM_STATUS_OK;
    if (tls_client_start_request) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &resource_id);
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 1,
                                           &address_object);
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 2,
                                           &recipient);
        }
    } else if (listen_request || connect_request || udp_connect_request
               || dns_resolve_request) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &address_object);
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 1,
                                           &port_or_limit);
        }
        if (status == OM_STATUS_OK && listen_request) {
            status = om_runtime_record_get(source_context, message_object, 2,
                                           &backlog);
        }
        if (status == OM_STATUS_OK && udp_connect_request) {
            status = om_runtime_record_get(source_context, message_object, 2,
                                           &backlog);
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object,
                listen_request || udp_connect_request ? 3
                    : connect_request || dns_resolve_request ? 2 : 0,
                &recipient);
        }
    } else if (accept_request || any_close_request) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &resource_id);
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 1,
                                           &recipient);
        }
    } else if (any_send_request || any_receive_request) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &resource_id);
        if (status == OM_STATUS_OK && any_send_request) {
            status = om_runtime_record_get(source_context, message_object, 1,
                                           &byte_list);
        } else if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 1,
                                           &port_or_limit);
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_get(source_context, message_object, 2,
                                           &recipient);
        }
    }
    if (status != OM_STATUS_OK) return status;
#if defined(_WIN32)
    if (!system->network_initialized) {
        WSADATA winsock_data;
        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
            return network_deliver_error_locked(system, recipient, operation,
                                                 "unsupported");
        }
        system->network_initialized = true;
    }
#endif
    if ((listen_request || connect_request || udp_connect_request
         || dns_resolve_request)
        && (port_or_limit < 0 || port_or_limit > 65535)) {
        return network_deliver_error_locked(system, recipient, operation,
            dns_resolve_request ? "invalid_port" : "invalid_address");
    }
    if (udp_connect_request && (backlog < 0 || backlog > 65535)) {
        return network_deliver_error_locked(system, recipient, operation,
                                             "invalid_address");
    }
    if (listen_request && (backlog < 1 || backlog > 128)) {
        return network_deliver_error_locked(system, recipient, operation,
                                             "invalid_argument");
    }
    if (any_receive_request
        && (port_or_limit < 0
            || (uint64_t)port_or_limit > (udp_receive_request
                ? (size_t)65507 : OM_SYSTEM_NETWORK_MAX_BUFFER)
            || (udp_receive_request && port_or_limit == 0))) {
        return network_deliver_error_locked(system, recipient, operation,
                                             "resource_exhausted");
    }
    size_t input_length = 0;
    if (any_send_request) {
        status = om_runtime_list_length(source_context, byte_list, &input_length);
        if (status != OM_STATUS_OK) return status;
        if (udp_send_request && input_length > (size_t)65507) {
            return network_deliver_error_locked(system, recipient, operation,
                                                 "datagram_too_large");
        }
        if (input_length > OM_SYSTEM_NETWORK_MAX_BUFFER) {
            return network_deliver_error_locked(system, recipient, operation,
                                                 "resource_exhausted");
        }
    }
    struct Om_Runtime_Network_Resource *resource = NULL;
    struct Om_Runtime_Network_Resource *listener = NULL;
    if (accept_request || any_send_request || any_receive_request
        || tls_client_start_request || any_close_request) {
        resource = network_find_resource_locked(system, resource_id);
        if (resource == NULL) {
            if (any_close_request) {
                return actor_deliver_result_locked(system, recipient,
                    network_completion_id(operation), true, false, 0,
                    NULL, 0, NULL, 0, NULL);
            }
            return network_deliver_error_locked(system, recipient, operation,
                                                 "closed");
        }
        if (tls_client_start_request) {
            if (resource->closed || resource->listener || resource->udp
                || resource->native_socket == -1) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            if (resource->tls_session != NULL) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            if (resource->busy_tls || resource->busy_read || resource->busy_write) {
                return network_deliver_error_locked(system, recipient,
                    operation, "resource_exhausted");
            }
            resource->busy_tls = true;
        } else if (any_close_request) {
            if (tls_close_request) {
                if (resource->udp || resource->listener) {
                    return network_deliver_error_locked(system, recipient,
                        operation, "invalid_argument");
                }
                if (resource->closed) {
                    return actor_deliver_result_locked(system, recipient,
                        network_completion_id(operation), true, false, 0,
                        NULL, 0, NULL, 0, NULL);
                }
                if (resource->tls_session == NULL) {
                    return network_deliver_error_locked(system, recipient,
                        operation, "invalid_argument");
                }
                if (resource->busy_tls || resource->busy_read || resource->busy_write) {
                    return network_deliver_error_locked(system, recipient,
                        operation, "resource_exhausted");
                }
                resource->closed = true;
                if (system->active_network_resources != 0) {
                    --system->active_network_resources;
                }
                om_tls_close(resource->tls_session);
                resource->tls_session = NULL;
                if (!resource->native_closed && resource->native_socket != -1) {
                    network_close_native(network_native_socket(
                        resource->native_socket));
                    resource->native_closed = true;
                }
                const Om_Status delivered = actor_deliver_result_locked(system,
                    recipient, network_completion_id(operation), true, false, 0,
                    NULL, 0, NULL, 0, NULL);
                if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient);
                return delivered;
            }
            if (resource->tls_session != NULL) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            if (resource->udp != udp_close_request) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            if (!resource->closed) {
                resource->closed = true;
                if (system->active_network_resources != 0) {
                    --system->active_network_resources;
                }
                if (!resource->native_closed
                    && resource->native_socket != -1) {
                    if (resource->busy_read || resource->busy_write) {
                        network_shutdown_native(network_native_socket(
                            resource->native_socket));
                    } else {
                        network_close_native(network_native_socket(
                            resource->native_socket));
                        resource->native_closed = true;
                    }
                }
            }
            const Om_Status delivered = actor_deliver_result_locked(system,
                recipient, network_completion_id(operation),
                true, false, 0, NULL, 0, NULL, 0, NULL);
            if (delivered != OM_STATUS_OK) actor_fail_locked(system, recipient);
            return delivered;
        } else if (resource->closed) {
            return network_deliver_error_locked(system, recipient, operation,
                                                 "closed");
        } else if (accept_request) {
            if (!resource->listener || resource->udp) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            if (resource->busy_read) {
                return network_deliver_error_locked(system, recipient,
                    operation, "resource_exhausted");
            }
            resource->busy_read = true;
            listener = resource;
        } else if (tls_send_request || tls_receive_request) {
            if (resource->listener || resource->udp
                || resource->tls_session == NULL) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            if (resource->busy_tls) {
                return network_deliver_error_locked(system, recipient,
                    operation, "resource_exhausted");
            }
            resource->busy_tls = true;
        } else {
            if ((any_send_request || any_receive_request)
                && (resource->listener
                    || resource->udp != (udp_send_request || udp_receive_request))) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            bool *busy = any_send_request ? &resource->busy_write
                                      : &resource->busy_read;
            if (*busy) {
                return network_deliver_error_locked(system, recipient,
                    operation, "resource_exhausted");
            }
            if (resource->tls_session != NULL) {
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            *busy = true;
        }
    }
    const bool creates_resource = listen_request || connect_request
        || udp_connect_request
        || accept_request;
    if (creates_resource
        && (system->active_network_resources
                + system->pending_network_resources
                >= OM_SYSTEM_NETWORK_MAX_RESOURCES
            || system->next_network_resource_id >= (uint64_t)INT64_MAX)) {
        if (listener != NULL) listener->busy_read = false;
        return network_deliver_error_locked(system, recipient, operation,
                                             "resource_exhausted");
    }
    if (system->blocking_pool == NULL) {
        if (resource != NULL) {
            if (tls_client_start_request || tls_send_request || tls_receive_request)
                resource->busy_tls = false;
            if (any_send_request) resource->busy_write = false;
            if (any_receive_request || accept_request) resource->busy_read = false;
        }
        return OM_STATUS_CANCELLED;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) {
        if (resource != NULL) {
            if (tls_client_start_request || tls_send_request || tls_receive_request)
                resource->busy_tls = false;
            if (any_send_request) resource->busy_write = false;
            if (any_receive_request || accept_request) resource->busy_read = false;
        }
        return network_deliver_error_locked(system, recipient, operation,
                                             "resource_exhausted");
    }
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = operation,
        .recipient_id = recipient,
        .requested_length = port_or_limit,
        .error_code = "io_error",
        .network_resource = resource,
        .network_listener = listener,
        .tls_session = NULL,
        .network_backlog = dns_resolve_request ? (int)port_or_limit
                                               : (int)backlog,
        .input_length = any_send_request ? input_length : 0,
    };
    if (creates_resource) {
        struct Om_Runtime_Network_Resource *created = om_alloc(sizeof(*created));
        if (created == NULL) {
            system_task_destroy(task);
            if (listener != NULL) listener->busy_read = false;
            return network_deliver_error_locked(system, recipient, operation,
                                                 "resource_exhausted");
        }
        *created = (struct Om_Runtime_Network_Resource){
            .id = ++system->next_network_resource_id,
            .native_socket = -1,
            .listener = listen_request,
            .udp = udp_connect_request,
        };
        task->network_resource = created;
        task->network_resource_is_new = true;
        if (accept_request) task->network_listener = listener;
    }
    if (listen_request || connect_request || udp_connect_request
        || dns_resolve_request || tls_client_start_request) {
        status = system_task_copy_string(source_context, address_object,
                                        &task->path, &input_length);
        if (status != OM_STATUS_OK) {
            system_task_destroy(task);
            if (resource != NULL && tls_client_start_request)
                resource->busy_tls = false;
            return network_deliver_error_locked(system, recipient, operation,
                dns_resolve_request ? "invalid_host" : "invalid_address");
        }
        if (tls_client_start_request
            && (input_length == 0 || input_length > 253
                || !system_dns_host_valid(task->path))) {
            system_task_destroy(task);
            resource->busy_tls = false;
            return network_deliver_error_locked(system, recipient, operation,
                                                 "invalid_host");
        }
        if (dns_resolve_request && input_length == 0) {
            system_task_destroy(task);
            return network_deliver_error_locked(system, recipient, operation,
                                                 "invalid_host");
        }
        if (dns_resolve_request && input_length > 253) {
            system_task_destroy(task);
            return network_deliver_error_locked(system, recipient, operation,
                                                 "invalid_host");
        }
    } else if (any_send_request && input_length != 0) {
        task->input_bytes = om_alloc(input_length);
        if (task->input_bytes == NULL) {
            system_task_destroy(task);
            if (resource != NULL) {
                if (tls_send_request) resource->busy_tls = false;
                else resource->busy_write = false;
            }
            return network_deliver_error_locked(system, recipient, operation,
                                                 "resource_exhausted");
        }
        for (size_t index = 0; index < input_length; ++index) {
            Om_Runtime_Value item = {0};
            status = om_runtime_list_get(source_context, byte_list, index,
                                         &item);
            if (status != OM_STATUS_OK || item.integer < 0
                || item.integer > 255) {
                system_task_destroy(task);
                if (resource != NULL) {
                    if (tls_send_request) resource->busy_tls = false;
                    else resource->busy_write = false;
                }
                return network_deliver_error_locked(system, recipient,
                    operation, "invalid_argument");
            }
            task->input_bytes[index] = (uint8_t)item.integer;
        }
    }
    if (system->outstanding_messages == SIZE_MAX) {
        system_task_destroy(task);
        if (resource != NULL) {
            if (tls_client_start_request || tls_send_request || tls_receive_request)
                resource->busy_tls = false;
            if (any_send_request) resource->busy_write = false;
            if (any_receive_request || accept_request) resource->busy_read = false;
        }
        return network_deliver_error_locked(system, recipient, operation,
                                             "resource_exhausted");
    }
    ++system->outstanding_messages;
    if (creates_resource) ++system->pending_network_resources;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        if (creates_resource && system->pending_network_resources != 0) {
            --system->pending_network_resources;
        }
        if (resource != NULL) {
            if (tls_client_start_request || tls_send_request || tls_receive_request)
                resource->busy_tls = false;
            if (any_send_request) resource->busy_write = false;
            if (any_receive_request || accept_request) resource->busy_read = false;
        }
        system_task_destroy(task);
        actor_condition_broadcast(system);
        if (status == OM_STATUS_OUT_OF_RANGE
            || status == OM_STATUS_OUT_OF_MEMORY) {
            return network_deliver_error_locked(system, recipient, operation,
                                                 "resource_exhausted");
        }
        return status;
    }
    return OM_STATUS_OK;
}

static struct Om_Runtime_Process_Resource *process_find_locked(
    Om_Runtime_Actor_System *system, uint64_t id)
{
    for (struct Om_Runtime_Process_Resource *item = system->process_resources;
         item != NULL; item = item->next) {
        if (item->id == id) return item;
    }
    return NULL;
}

static Om_System_Message_Id process_completion_id(Om_System_Message_Id id)
{
    switch (id) {
    case OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN: return OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN: return OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN: return OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT: return OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR: return OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE: return OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE_COMPLETED;
    case OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT: return OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT_COMPLETED;
    default: return OM_SYSTEM_MESSAGE_ID_NONE;
    }
}

static Om_Status process_error_locked(Om_Runtime_Actor_System *system,
    uint64_t recipient, Om_System_Message_Id operation, const char *error)
{
    const Om_System_Message_Id completion = process_completion_id(operation);
    if (completion == OM_SYSTEM_MESSAGE_ID_NONE) return OM_STATUS_NOT_FOUND;
    return actor_deliver_result_locked(system, recipient, completion, false,
        false, 0, NULL, 0, NULL, 0, error);
}

Om_Status actor_send_process_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (system == NULL || source_context == NULL || message_type == NULL
        || message_object == NULL) return OM_STATUS_INVALID_ARGUMENT;
    const Om_System_Message_Id operation = message_type->system_message_id;
    const bool spawn = operation == OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN;
    const size_t field_count = spawn ? 6
        : operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN
            || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT
            || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR ? 3
        : operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN
            || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE
            || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT ? 2 : 0;
    if (field_count == 0 || message_type->field_count != field_count)
        return OM_STATUS_NOT_FOUND;

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){.system = system, .operation = operation,
        .error_code = "io_error"};
    Om_Status status = OM_STATUS_OK;
    uint64_t process_id = 0;
    const size_t reply_index = field_count - 1;
    status = om_runtime_record_get(source_context, message_object, reply_index,
                                   &task->recipient_id);
    if (spawn) {
        void *executable = NULL, *arguments = NULL, *environment = NULL;
        void *cwd = NULL;
        if (status == OM_STATUS_OK) status = om_runtime_record_get(source_context, message_object, 0, &executable);
        if (status == OM_STATUS_OK) status = om_runtime_record_get(source_context, message_object, 1, &arguments);
        if (status == OM_STATUS_OK) status = om_runtime_record_get(source_context, message_object, 2, &task->process_replace_environment);
        if (status == OM_STATUS_OK) status = om_runtime_record_get(source_context, message_object, 3, &environment);
        if (status == OM_STATUS_OK) status = om_runtime_record_get(source_context, message_object, 4, &cwd);
        if (status == OM_STATUS_OK) status = system_task_copy_string(source_context, executable, &task->path, &task->input_length);
        size_t args_bytes = 0, env_bytes = 0;
        if (status == OM_STATUS_OK) status = system_task_copy_string_list(source_context, arguments, &task->process_arguments, &task->process_argument_count, &args_bytes);
        if (status == OM_STATUS_OK) status = system_task_copy_string_list(source_context, environment, &task->process_environment, &task->process_environment_count, &env_bytes);
        if (status == OM_STATUS_OK && args_bytes > OM_SYSTEM_PROCESS_MAX_ARGUMENT_BYTES - env_bytes) status = OM_STATUS_OUT_OF_RANGE;
        if (status == OM_STATUS_OK && !task->process_replace_environment
            && task->process_environment_count != 0) status = OM_STATUS_INVALID_ARGUMENT;
        for (size_t index = 0; status == OM_STATUS_OK
             && task->process_replace_environment
             && index < task->process_environment_count; ++index) {
            const char *entry = task->process_environment[index];
            const char *separator = strchr(entry, '=');
            if (separator == NULL || separator == entry) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        size_t cwd_length = 0;
        if (status == OM_STATUS_OK) status = system_task_copy_string(source_context, cwd, &task->working_directory, &cwd_length);
    } else {
        if (status == OM_STATUS_OK)
            status = om_runtime_record_get(source_context, message_object, 0, &process_id);
        if (status == OM_STATUS_OK && (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR))
            status = om_runtime_record_get(source_context, message_object, 1, &task->requested_length);
        if (status == OM_STATUS_OK && operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN) {
            void *bytes = NULL;
            status = om_runtime_record_get(source_context, message_object, 1, &bytes);
            if (status == OM_STATUS_OK) status = om_runtime_list_length(source_context, bytes, &task->input_length);
            if (status == OM_STATUS_OK && task->input_length > OM_SYSTEM_PROCESS_MAX_IO_BYTES) status = OM_STATUS_OUT_OF_RANGE;
            if (status == OM_STATUS_OK && task->input_length != 0) {
                task->input_bytes = om_alloc(task->input_length);
                if (task->input_bytes == NULL) status = OM_STATUS_OUT_OF_MEMORY;
            }
            for (size_t i = 0; status == OM_STATUS_OK && i < task->input_length; ++i) {
                Om_Runtime_Value byte = {0};
                status = om_runtime_list_get(source_context, bytes, i, &byte);
                if (status == OM_STATUS_OK && (byte.integer < 0 || byte.integer > 255)) status = OM_STATUS_OUT_OF_RANGE;
                if (status == OM_STATUS_OK) task->input_bytes[i] = (uint8_t)byte.integer;
            }
        }
    }
    if (status != OM_STATUS_OK) {
        const uint64_t recipient = task->recipient_id;
        const char *error = status == OM_STATUS_OUT_OF_RANGE
            ? "resource_exhausted" : "invalid_argument";
        system_task_destroy(task);
        return process_error_locked(system, recipient, operation, error);
    }
    if ((operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR)
        && (task->requested_length < 0 || (uint64_t)task->requested_length > OM_SYSTEM_PROCESS_MAX_IO_BYTES)) {
        const uint64_t recipient = task->recipient_id;
        system_task_destroy(task);
        return process_error_locked(system, recipient, operation, "resource_exhausted");
    }

    if (spawn) {
        if (system->active_process_resources + system->pending_process_resources >= OM_SYSTEM_PROCESS_MAX_RESOURCES) {
            const uint64_t recipient = task->recipient_id;
            system_task_destroy(task);
            return process_error_locked(system, recipient, operation, "resource_exhausted");
        }
        struct Om_Runtime_Process_Resource *created = om_alloc(sizeof(*created));
        if (created == NULL) {
            const uint64_t recipient = task->recipient_id;
            system_task_destroy(task);
            return process_error_locked(system, recipient, operation, "resource_exhausted");
        }
        *created = (struct Om_Runtime_Process_Resource){.id = ++system->next_process_resource_id,
            .native_process = -1, .process_guard = -1,
            .stdin_handle = -1, .stdout_handle = -1, .stderr_handle = -1};
        task->process_resource = created;
        task->process_resource_is_new = true;
        ++system->pending_process_resources;
    } else {
        struct Om_Runtime_Process_Resource *process = process_find_locked(system, process_id);
        if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT
            && process != NULL && process->exited) {
            const Om_Status delivered = actor_deliver_result_locked(system,
                task->recipient_id,
                OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT_COMPLETED, true, false,
                process->exit_code, NULL, 0, NULL, 0, NULL);
            system_task_destroy(task);
            return delivered;
        }
        if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT
            && process != NULL && process->exited) {
            const Om_Status delivered = actor_deliver_result_locked(system,
                task->recipient_id,
                OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT_COMPLETED, true, false,
                process->exit_code, NULL, 0, NULL, 0, NULL);
            system_task_destroy(task);
            return delivered;
        }
        const bool busy = process != NULL && ((operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN) ? process->busy_stdin
            : operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT ? process->busy_stdout
            : operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR ? process->busy_stderr
            : operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT ? process->busy_wait : false);
        if (process == NULL || process->closed || busy) {
            const uint64_t recipient = task->recipient_id;
            system_task_destroy(task);
            return process_error_locked(system, recipient, operation, process == NULL || process->closed ? "closed" : "resource_exhausted");
        }
        task->process_resource = process;
        if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN || operation == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN) process->busy_stdin = true;
        if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT) process->busy_stdout = true;
        if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR) process->busy_stderr = true;
        if (operation == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT) process->busy_wait = true;
    }
    if (system->blocking_pool == NULL || system->outstanding_messages == SIZE_MAX) {
        const uint64_t recipient = task->recipient_id;
        system_task_destroy(task);
        return process_error_locked(system, recipient, operation, "resource_exhausted");
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool, (Om_Blocking_Task){
        .run = system_task_run, .complete = system_file_complete,
        .destroy = system_task_destroy, .context = task});
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        if (spawn && system->pending_process_resources != 0) --system->pending_process_resources;
        if (!spawn && task->process_resource != NULL) {
            task->process_resource->busy_stdin = false;
            task->process_resource->busy_stdout = false;
            task->process_resource->busy_stderr = false;
            task->process_resource->busy_wait = false;
        }
        const uint64_t recipient = task->recipient_id;
        system_task_destroy(task);
        actor_condition_broadcast(system);
        return process_error_locked(system, recipient, operation, "resource_exhausted");
    }
    return OM_STATUS_OK;
}

Om_Status actor_send_environment_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL) return OM_STATUS_NOT_FOUND;
    const Om_System_Message_Id operation = message_type->system_message_id;
    const bool get = operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_GET;
    const bool supported = get
        || operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_ARGUMENTS
        || operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_CURRENT_DIRECTORY
        || operation == OM_SYSTEM_MESSAGE_ID_ENVIRONMENT_EXECUTABLE_PATH;
    if (!supported || message_type->field_count != (get ? 2U : 1U)) {
        return OM_STATUS_NOT_FOUND;
    }
    void *name_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = OM_STATUS_OK;
    if (get) {
        status = om_runtime_record_get(source_context, message_object, 0,
                                       &name_object);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object,
            get ? 1 : 0, &recipient_id);
    }
    if (status != OM_STATUS_OK) return status;
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = operation,
        .recipient_id = recipient_id,
        .error_code = "io_error",
    };
    if (get) {
        size_t name_length = 0;
        status = system_task_copy_string(source_context, name_object,
            &task->path, &name_length);
        if (status == OM_STATUS_INVALID_ARGUMENT) {
            task->error_code = "invalid_name";
            task->path = om_alloc(1);
            if (task->path == NULL) status = OM_STATUS_OUT_OF_MEMORY;
            else { task->path[0] = '\0'; status = OM_STATUS_OK; }
        } else if (status == OM_STATUS_OUT_OF_RANGE) {
            task->error_code = "resource_exhausted";
            task->path = om_alloc(1);
            if (task->path == NULL) status = OM_STATUS_OUT_OF_MEMORY;
            else { task->path[0] = '\0'; status = OM_STATUS_OK; }
        }
        if (status == OM_STATUS_OK
            && name_length > OM_SYSTEM_ENVIRONMENT_MAX_VALUE_BYTES) {
            om_free(task->path);
            task->path = om_alloc(1);
            if (task->path == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                task->path[0] = '\0';
                task->error_code = "resource_exhausted";
            }
        }
        if (status == OM_STATUS_OK && task->error_code != NULL
            && strcmp(task->error_code, "invalid_name") == 0) {
            /* Preserve the async completion contract for malformed names. */
        } else if (status == OM_STATUS_OK
            && (name_length == 0 || memchr(task->path, '=', name_length))) {
            task->error_code = "invalid_name";
        }
    }
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        system_task_destroy(task);
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){.run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy, .context = task});
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

Om_Status actor_send_timer_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL
        || message_type->system_message_id != OM_SYSTEM_MESSAGE_ID_TIMER_AFTER
        || message_type->field_count != 3) {
        return OM_STATUS_NOT_FOUND;
    }
    int64_t delay = 0;
    void *payload_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                              &delay);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &payload_object);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 2,
                                       &recipient_id);
    }
    if (status != OM_STATUS_OK || delay < 0) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    if (system->timer_service == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->timer_service == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = OM_SYSTEM_MESSAGE_ID_TIMER_AFTER,
        .recipient_id = recipient_id,
    };
    task->payload_context = om_runtime_context_create(
        system->program->collection_threshold);
    if (task->payload_context == NULL) {
        system_task_destroy(task);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_Runtime_Object_Header *payload = find_object(source_context,
                                                           payload_object);
    if (payload == NULL || payload->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        system_task_destroy(task);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = om_runtime_object_clone(source_context, task->payload_context,
                                     payload_object, &task->payload_object);
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    uint64_t timer_id = 0;
    ++system->outstanding_messages;
    status = om_timer_service_schedule(system->timer_service,
        (Om_Timer_Task){
            .delay_milliseconds = (uint64_t)delay,
            .complete = timer_task_complete,
            .destroy = system_task_destroy,
            .context = task,
        }, &timer_id);
    (void)timer_id;
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}
