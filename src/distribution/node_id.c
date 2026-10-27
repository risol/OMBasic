#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ombasic/distribution/protocol.h"

#include <stdbool.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

Om_Status om_distribution_node_id_generate(Om_Distribution_Node_Id *node_id)
{
    if (node_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *node_id = (Om_Distribution_Node_Id){0};
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(
        NULL, node_id->bytes, (ULONG)sizeof(node_id->bytes),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return OM_STATUS_IO_ERROR;
    }
#else
    const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return OM_STATUS_IO_ERROR;
    }
    size_t received = 0;
    while (received < sizeof(node_id->bytes)) {
        const ssize_t count = read(
            descriptor, node_id->bytes + received,
            sizeof(node_id->bytes) - received);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(descriptor);
            *node_id = (Om_Distribution_Node_Id){0};
            return OM_STATUS_IO_ERROR;
        }
        if (count == 0) {
            (void)close(descriptor);
            *node_id = (Om_Distribution_Node_Id){0};
            return OM_STATUS_IO_ERROR;
        }
        received += (size_t)count;
    }
    if (close(descriptor) != 0) {
        *node_id = (Om_Distribution_Node_Id){0};
        return OM_STATUS_IO_ERROR;
    }
#endif
    /* Mark the value as UUIDv4-shaped for easier inspection in diagnostics. */
    node_id->bytes[6] = (uint8_t)((node_id->bytes[6] & UINT8_C(0x0f))
                                  | UINT8_C(0x40));
    node_id->bytes[8] = (uint8_t)((node_id->bytes[8] & UINT8_C(0x3f))
                                  | UINT8_C(0x80));
    bool nonzero = false;
    for (size_t index = 0; index < sizeof(node_id->bytes); ++index) {
        nonzero = nonzero || node_id->bytes[index] != 0;
    }
    return nonzero ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}
