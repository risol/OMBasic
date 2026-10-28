#ifndef OMBASIC_RUNTIME_ABI_DISTRIBUTION_H
#define OMBASIC_RUNTIME_ABI_DISTRIBUTION_H

#include <stdint.h>

#include "ombasic/runtime/abi/actor.h"

/* The generated wrapper owns this state slot and destroys it before the
   program's actor system and execution context stop. */
typedef struct Om_Runtime_Distribution {
    void *owned_state;
} Om_Runtime_Distribution;

Om_Status om_runtime_distribution_create(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    Om_Runtime_Distribution *distribution);
void om_runtime_distribution_destroy(Om_Runtime_Distribution *distribution);

#endif
