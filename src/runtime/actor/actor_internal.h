#ifndef OMBASIC_RUNTIME_ACTOR_INTERNAL_H
#define OMBASIC_RUNTIME_ACTOR_INTERNAL_H

#include "runtime_internal.h"

#include "blocking_pool.h"
#include "timer_service.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define OM_RUNTIME_SYSTEM_QUEUE_CAPACITY ((size_t)1024)
#define OM_RUNTIME_REMOTE_ACTOR_ID_BIT (UINT64_C(1) << 63)

typedef struct Om_Runtime_Actor_Message {
    void *object;
    struct Om_Runtime_Actor_Message *next;
} Om_Runtime_Actor_Message;

typedef struct Om_Runtime_Actor_Watcher {
    Om_Runtime_Actor_Ref actor;
    bool remote;
    bool notified;
    struct Om_Runtime_Actor_Watcher *next;
} Om_Runtime_Actor_Watcher;

typedef struct Om_Runtime_Ready_Actor {
    Om_Runtime_Actor *actor;
    struct Om_Runtime_Ready_Actor *next;
} Om_Runtime_Ready_Actor;

typedef struct Om_Runtime_Actor_Thread {
#if defined(_WIN32)
    HANDLE native;
#else
    pthread_t native;
#endif
} Om_Runtime_Actor_Thread;

struct Om_Runtime_Actor {
    Om_Runtime_Actor_Ref reference;
    const Om_Runtime_Actor_Descriptor *descriptor;
    Om_Execution_Context *context;
    void *state_object;
    void *state_payload;
    Om_Runtime_Root_Frame *state_root;
    Om_Runtime_Actor_Message *mailbox_head;
    Om_Runtime_Actor_Message *mailbox_tail;
    Om_Runtime_Actor_Watcher *watchers;
    bool running;
    bool scheduled;
    bool stop_requested;
    bool terminated;
    bool failed;
    struct Om_Runtime_Actor *next;
};

struct Om_Runtime_Actor_System {
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    Om_Execution_Context *root_context;
    Om_Execution_Context *mailbox_context;
    const Om_Runtime_Program_Descriptor *program;
    const Om_Runtime_Distribution_Hooks *distribution_hooks;
    void *distribution_state;
    size_t distribution_hooks_active;
    Om_Blocking_Pool *blocking_pool;
    Om_Timer_Service *timer_service;
    Om_Runtime_Actor_Thread *threads;
    size_t worker_count;
    Om_Runtime_Actor *actors;
    Om_Runtime_Ready_Actor *ready_head;
    Om_Runtime_Ready_Actor *ready_tail;
    uint64_t next_actor_id;
    size_t outstanding_messages;
    size_t running_handlers;
    bool stopping;
    bool shutting_down;
    bool joined;
};

typedef struct Om_Runtime_System_Task {
    Om_Runtime_Actor_System *system;
    Om_System_Message_Id operation;
    uint64_t recipient_id;
    char *path;
    uint8_t *input_bytes;
    size_t input_length;
    uint8_t *result_bytes;
    size_t result_length;
    Om_Execution_Context *payload_context;
    void *payload_object;
} Om_Runtime_System_Task;

void actor_mutex_lock(Om_Runtime_Actor_System *system);
void actor_mutex_unlock(Om_Runtime_Actor_System *system);
void actor_condition_wait(Om_Runtime_Actor_System *system);
void actor_condition_signal(Om_Runtime_Actor_System *system);
void actor_condition_broadcast(Om_Runtime_Actor_System *system);
bool actor_sync_init(Om_Runtime_Actor_System *system);
void actor_sync_destroy(Om_Runtime_Actor_System *system);
Om_Runtime_Actor *actor_find_locked(Om_Runtime_Actor_System *system,
                                   Om_Runtime_Actor_Ref reference);
const Om_Runtime_Actor_Handler_Descriptor *actor_handler_for(
    const Om_Runtime_Actor_Descriptor *actor,
    const Om_Runtime_Type_Descriptor *message_type);
Om_Status actor_enqueue_object_locked(Om_Runtime_Actor_System *system,
                                     Om_Runtime_Actor *actor,
                                     void *message_object);
Om_Status actor_schedule_locked(Om_Runtime_Actor_System *system,
                                Om_Runtime_Actor *actor);
Om_Status actor_discard_mailbox_locked(Om_Runtime_Actor_System *system,
                                       Om_Runtime_Actor *actor);
Om_Runtime_Actor_Message *actor_pop_message_locked(
    Om_Runtime_Actor *actor);
Om_Status actor_notify_watchers_locked(Om_Runtime_Actor_System *system,
                                      Om_Runtime_Actor *actor);
void actor_fail_locked(Om_Runtime_Actor_System *system, uint64_t actor_id);
Om_Status actor_mailbox_root_provider(
    Om_Execution_Context *context,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context);
Om_Status actor_send_console_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_file_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_timer_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);

#endif
