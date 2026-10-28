#ifndef OMBASIC_RUNTIME_DISTRIBUTION_H
#define OMBASIC_RUNTIME_DISTRIBUTION_H

#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/runtime/native_runtime.h"

typedef struct Om_Runtime_Distribution {
    void *owned_state;
} Om_Runtime_Distribution;

/* Start after actor_system_start and destroy before the actor system's root
   execution context. The program descriptor is borrowed for this lifetime. */
Om_Status om_runtime_distribution_create(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    Om_Runtime_Distribution *distribution
);

Om_Status om_runtime_distribution_listen(
    Om_Runtime_Distribution *distribution,
    Om_String_View bind_address,
    uint16_t port,
    int backlog,
    uint16_t *bound_port
);

/* Accept and connect complete the program/interface fingerprint handshake.
   Returned node refs are local handles for SPAWN AT and become stable for the
   distribution adapter's lifetime. */
Om_Status om_runtime_distribution_accept(
    Om_Runtime_Distribution *distribution,
    uint64_t *node_ref
);
Om_Status om_runtime_distribution_connect(
    Om_Runtime_Distribution *distribution,
    Om_String_View host,
    uint16_t port,
    uint64_t *node_ref
);

void om_runtime_distribution_destroy(
    Om_Runtime_Distribution *distribution
);

#endif
