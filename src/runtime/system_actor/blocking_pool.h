#ifndef OMBASIC_RUNTIME_SYSTEM_ACTOR_BLOCKING_POOL_H
#define OMBASIC_RUNTIME_SYSTEM_ACTOR_BLOCKING_POOL_H

#include <stddef.h>

#include "ombasic/foundation/status.h"

typedef struct Om_Blocking_Pool Om_Blocking_Pool;

typedef Om_Status (*Om_Blocking_Task_Run)(void *context);
typedef void (*Om_Blocking_Task_Complete)(void *context, Om_Status status);
typedef void (*Om_Blocking_Task_Destroy)(void *context);

typedef struct Om_Blocking_Task {
    Om_Blocking_Task_Run run;
    Om_Blocking_Task_Complete complete;
    Om_Blocking_Task_Destroy destroy;
    /* Owns copied native data only; never retain an Actor heap pointer here. */
    void *context;
} Om_Blocking_Task;

typedef enum Om_Blocking_Pool_Shutdown_Mode {
    OM_BLOCKING_POOL_DRAIN = 0,
    OM_BLOCKING_POOL_CANCEL_PENDING
} Om_Blocking_Pool_Shutdown_Mode;

Om_Status om_blocking_pool_create(Om_Blocking_Pool **pool,
                                  size_t worker_count,
                                  size_t queue_capacity);
Om_Status om_blocking_pool_submit(Om_Blocking_Pool *pool,
                                  Om_Blocking_Task task);
void om_blocking_pool_shutdown(Om_Blocking_Pool *pool,
                               Om_Blocking_Pool_Shutdown_Mode mode);
void om_blocking_pool_destroy(Om_Blocking_Pool *pool);

#endif
