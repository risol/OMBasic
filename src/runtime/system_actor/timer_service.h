#ifndef OMBASIC_RUNTIME_SYSTEM_ACTOR_TIMER_SERVICE_H
#define OMBASIC_RUNTIME_SYSTEM_ACTOR_TIMER_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"

typedef struct Om_Timer_Service Om_Timer_Service;

typedef void (*Om_Timer_Complete)(void *context, Om_Status status);
typedef void (*Om_Timer_Destroy)(void *context);

typedef struct Om_Timer_Task {
    uint64_t delay_milliseconds;
    Om_Timer_Complete complete;
    Om_Timer_Destroy destroy;
    /* A timer task owns stable native data only, never an Actor heap pointer. */
    void *context;
} Om_Timer_Task;

Om_Status om_timer_service_create(Om_Timer_Service **service,
                                  size_t queue_capacity);
Om_Status om_timer_service_schedule(Om_Timer_Service *service,
                                    Om_Timer_Task task,
                                    uint64_t *timer_id);
Om_Status om_timer_service_cancel(Om_Timer_Service *service,
                                  uint64_t timer_id);
void om_timer_service_shutdown(Om_Timer_Service *service);
void om_timer_service_destroy(Om_Timer_Service *service);

#endif
