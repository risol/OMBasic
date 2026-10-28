#ifndef OMBASIC_RUNTIME_ABI_ACTOR_H
#define OMBASIC_RUNTIME_ABI_ACTOR_H

#include <stdint.h>

#include "ombasic/runtime/abi/program_descriptor.h"

typedef struct Om_Runtime_Actor_Ref {
    uint64_t id;
} Om_Runtime_Actor_Ref;

/* Actor state, message, handler, and descriptor data are borrowed and remain
   valid for the lifetime of the program module and actor system. */
Om_Status om_runtime_actor_system_start(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    size_t worker_count);
Om_Status om_runtime_actor_system_wait(Om_Execution_Context *context);
Om_Status om_runtime_actor_spawn_id(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t *result_id);
Om_Status om_runtime_actor_spawn_at(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t node_ref,
    Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_self_id(Om_Execution_Context *context,
                                   uint64_t *result_id);
Om_Status om_runtime_actor_state_object(Om_Execution_Context *context,
                                        void **result_object);
Om_Status om_runtime_actor_message_object(Om_Execution_Context *context,
                                          void **result_object);
Om_Status om_runtime_actor_send_id(Om_Execution_Context *context,
                                   uint64_t target_id,
                                   const void *message_object);
Om_Status om_runtime_actor_watch_id(Om_Execution_Context *context,
                                    uint64_t target_id);
Om_Status om_runtime_actor_stop(Om_Execution_Context *context);

#endif
