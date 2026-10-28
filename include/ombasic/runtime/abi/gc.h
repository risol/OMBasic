#ifndef OMBASIC_RUNTIME_ABI_GC_H
#define OMBASIC_RUNTIME_ABI_GC_H

#include <stddef.h>

#include "ombasic/runtime/abi/base.h"

/* Root slots are native stack addresses containing managed pointers. Frames
   must be popped in last-in-first-out order. */
Om_Runtime_Root_Frame *om_runtime_roots_push(Om_Execution_Context *context,
                                             void **root_slots,
                                             size_t root_count);
Om_Status om_runtime_roots_pop(Om_Execution_Context *context,
                               Om_Runtime_Root_Frame *frame);

#endif
