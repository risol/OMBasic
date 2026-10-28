#include "actor_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

void actor_mutex_lock(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    EnterCriticalSection(&system->mutex);
#else
    (void)pthread_mutex_lock(&system->mutex);
#endif
}

void actor_mutex_unlock(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    LeaveCriticalSection(&system->mutex);
#else
    (void)pthread_mutex_unlock(&system->mutex);
#endif
}

void actor_condition_wait(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    (void)SleepConditionVariableCS(&system->condition, &system->mutex,
                                   INFINITE);
#else
    (void)pthread_cond_wait(&system->condition, &system->mutex);
#endif
}

void actor_condition_signal(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    WakeConditionVariable(&system->condition);
#else
    (void)pthread_cond_signal(&system->condition);
#endif
}

void actor_condition_broadcast(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&system->condition);
#else
    (void)pthread_cond_broadcast(&system->condition);
#endif
}

bool actor_sync_init(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&system->mutex, 0, 0)) {
        return false;
    }
    InitializeConditionVariable(&system->condition);
#else
    if (pthread_mutex_init(&system->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&system->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&system->mutex);
        return false;
    }
#endif
    return true;
}

void actor_sync_destroy(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    DeleteCriticalSection(&system->mutex);
#else
    (void)pthread_cond_destroy(&system->condition);
    (void)pthread_mutex_destroy(&system->mutex);
#endif
}

Om_Runtime_Actor *actor_find_locked(Om_Runtime_Actor_System *system,
                                    Om_Runtime_Actor_Ref reference)
{
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        if (actor->reference.id == reference.id) {
            return actor;
        }
    }
    return NULL;
}

const Om_Runtime_Actor_Handler_Descriptor *actor_handler_for(
    const Om_Runtime_Actor_Descriptor *actor,
    const Om_Runtime_Type_Descriptor *message_type)
{
    for (size_t index = 0; index < actor->handler_count; ++index) {
        const Om_Runtime_Actor_Handler_Descriptor *handler =
            &actor->handlers[index];
        if (handler->message_type->fingerprint == message_type->fingerprint) {
            return handler;
        }
    }
    return NULL;
}

void actor_fail_locked(Om_Runtime_Actor_System *system,
                              uint64_t actor_id)
{
    Om_Runtime_Actor *actor = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){actor_id});
    if (actor == NULL || actor->terminated) return;
    actor->failed = true;
    actor->stop_requested = true;
    if (!actor->running) {
        if (actor_schedule_locked(system, actor) != OM_STATUS_OK) {
            actor->terminated = true;
        }
    }
    actor_condition_broadcast(system);
}

static bool actor_program_contains(const Om_Runtime_Program_Descriptor *program,
                                   const Om_Runtime_Actor_Descriptor *actor)
{
    if (program == NULL || actor == NULL) {
        return false;
    }
    for (size_t index = 0; index < program->actor_count; ++index) {
        if (program->actors[index] == actor) {
            return true;
        }
    }
    return false;
}

static int actor_state_initializer_entry(Om_Execution_Context *context);

static Om_Status actor_context_initialize_state(Om_Runtime_Actor *actor)
{
    Om_Execution_Context *context = actor->context;
    const Om_Runtime_Actor_Descriptor *descriptor = actor->descriptor;
    Om_Status status = om_runtime_set_source_spans(context,
        actor->context->actor_system->program->source_spans,
        actor->context->actor_system->program->source_span_count);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(context, descriptor->state_type,
                                         &actor->state_object);
    }
    if (status == OM_STATUS_OK) {
        actor->state_root = om_runtime_roots_push(context,
            &actor->state_object, 1);
        if (actor->state_root == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        actor->state_payload = object_payload(find_object(context,
                                                          actor->state_object));
    }
    if (status == OM_STATUS_OK && descriptor->initialize_state != NULL) {
        context->pending_actor_initializer = descriptor->initialize_state;
        context->pending_actor_initializer_state = actor->state_payload;
        context->pending_actor_state_object = actor->state_object;
        context->pending_actor_initializer_status = OM_STATUS_OK;
        const int initializer_result = om_runtime_run(context,
            actor_state_initializer_entry);
        status = context->pending_actor_initializer_status;
        if (initializer_result != 0 && status == OM_STATUS_OK) {
            status = OM_STATUS_CANCELLED;
        }
        context->pending_actor_initializer = NULL;
        context->pending_actor_initializer_state = NULL;
        context->pending_actor_state_object = NULL;
    }
    return status;
}

static int actor_state_initializer_entry(Om_Execution_Context *context)
{
    if (context == NULL || context->pending_actor_initializer == NULL) {
        return 1;
    }
    context->pending_actor_initializer_status =
        context->pending_actor_initializer(context,
            context->pending_actor_initializer_state);
    return context->pending_actor_initializer_status == OM_STATUS_OK ? 0 : 1;
}

Om_Status om_runtime_distribution_attach(
    Om_Execution_Context *context,
    const Om_Runtime_Distribution_Hooks *hooks,
    void *state)
{
    if (context == NULL || context->actor_system == NULL || hooks == NULL
        || hooks->spawn == NULL || hooks->send == NULL
        || hooks->watch == NULL || hooks->notify_watcher == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    if (system->distribution_hooks != NULL) {
        actor_mutex_unlock(system);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    system->distribution_hooks = hooks;
    system->distribution_state = state;
    actor_mutex_unlock(system);
    return OM_STATUS_OK;
}

Om_Status om_runtime_distribution_detach(
    Om_Execution_Context *context,
    void *state)
{
    if (context == NULL || context->actor_system == NULL || state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    const Om_Status status = system->distribution_state != state
        || system->distribution_hooks == NULL
        ? OM_STATUS_NOT_FOUND : OM_STATUS_OK;
    if (status == OM_STATUS_OK) {
        system->distribution_hooks = NULL;
        system->distribution_state = NULL;
        while (system->distribution_hooks_active != 0) {
            actor_condition_wait(system);
        }
    }
    actor_mutex_unlock(system);
    return status;
}

static Om_Status actor_spawn_with_id(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    bool has_assigned_id,
    uint64_t assigned_id,
    Om_Runtime_Actor_Ref *result)
{
    if (context == NULL || context->actor_system == NULL
        || descriptor == NULL || result == NULL
        || !actor_program_contains(context->actor_system->program, descriptor)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    Om_Runtime_Actor *actor = om_alloc(sizeof(*actor));
    if (actor == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *actor = (Om_Runtime_Actor){.descriptor = descriptor};
    actor->context = om_runtime_context_create(
        system->program->collection_threshold);
    if (actor->context == NULL) {
        om_free(actor);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    actor->context->actor_system = system;
    Om_Status status = actor_context_initialize_state(actor);
    if (status != OM_STATUS_OK) {
        om_runtime_context_destroy(actor->context);
        om_free(actor);
        return status;
    }
    actor_mutex_lock(system);
    if (system->shutting_down) {
        status = OM_STATUS_CANCELLED;
    } else if (has_assigned_id
               && (assigned_id < OM_RUNTIME_REMOTE_ACTOR_ID_BIT
                   || actor_find_locked(system,
                       (Om_Runtime_Actor_Ref){assigned_id}) != NULL)) {
        status = OM_STATUS_INVALID_ARGUMENT;
    } else if (!has_assigned_id
               && (system->next_actor_id == 0
                   || system->next_actor_id
                       >= OM_RUNTIME_REMOTE_ACTOR_ID_BIT)) {
        status = OM_STATUS_OUT_OF_RANGE;
    } else if (has_assigned_id) {
        actor->reference.id = assigned_id;
        actor->next = system->actors;
        system->actors = actor;
        *result = actor->reference;
    } else {
        actor->reference.id = system->next_actor_id++;
        actor->next = system->actors;
        system->actors = actor;
        *result = actor->reference;
    }
    actor_mutex_unlock(system);
    if (status != OM_STATUS_OK) {
        om_runtime_context_destroy(actor->context);
        om_free(actor);
    }
    return status;
}

Om_Status om_runtime_actor_spawn(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    Om_Runtime_Actor_Ref *result)
{
    return actor_spawn_with_id(context, descriptor, false, 0, result);
}

Om_Status om_runtime_actor_spawn_assigned(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t assigned_id,
    Om_Runtime_Actor_Ref *result)
{
    if (assigned_id < OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return actor_spawn_with_id(context, descriptor, true, assigned_id, result);
}

Om_Status om_runtime_actor_spawn_at(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t node_ref,
    Om_Runtime_Actor_Ref *result)
{
    if (context == NULL || context->actor_system == NULL
        || descriptor == NULL || result == NULL
        || !actor_program_contains(context->actor_system->program, descriptor)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    const Om_Runtime_Distribution_Hooks *hooks = system->distribution_hooks;
    void *state = system->distribution_state;
    if (hooks == NULL || hooks->spawn == NULL) {
        actor_mutex_unlock(system);
        return OM_STATUS_NOT_FOUND;
    }
    ++system->distribution_hooks_active;
    actor_mutex_unlock(system);
    const Om_Status status = hooks->spawn(state, context, descriptor,
                                          node_ref, result);
    actor_mutex_lock(system);
    --system->distribution_hooks_active;
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_spawn_id(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t *result_id)
{
    if (result_id == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Actor_Ref reference = {0};
    const Om_Status status = om_runtime_actor_spawn(context, descriptor,
                                                     &reference);
    if (status == OM_STATUS_OK) *result_id = reference.id;
    return status;
}

Om_Status om_runtime_actor_self(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref *result)
{
    if (context == NULL || context->current_actor == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = context->current_actor->reference;
    return OM_STATUS_OK;
}

Om_Status om_runtime_actor_self_id(Om_Execution_Context *context,
                                   uint64_t *result_id)
{
    if (result_id == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Actor_Ref reference = {0};
    const Om_Status status = om_runtime_actor_self(context, &reference);
    if (status == OM_STATUS_OK) *result_id = reference.id;
    return status;
}

Om_Status om_runtime_actor_state_object(Om_Execution_Context *context,
                                        void **result_object)
{
    if (context == NULL || result_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (context->current_actor != NULL) {
        *result_object = context->current_actor->state_object;
    } else if (context->pending_actor_initializer != NULL) {
        *result_object = context->pending_actor_state_object;
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return *result_object == NULL ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
}

Om_Status om_runtime_actor_message_object(Om_Execution_Context *context,
                                          void **result_object)
{
    if (context == NULL || result_object == NULL
        || context->current_actor == NULL
        || context->current_actor_message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result_object = context->current_actor_message_object;
    return OM_STATUS_OK;
}

Om_Status om_runtime_actor_stop(Om_Execution_Context *context)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    context->current_actor->stop_requested = true;
    actor_mutex_unlock(system);
    return OM_STATUS_OK;
}
