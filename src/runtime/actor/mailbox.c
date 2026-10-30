#include "actor_internal.h"

#include "ombasic/foundation/memory.h"

Om_Status actor_schedule_locked(Om_Runtime_Actor_System *system,
                                       Om_Runtime_Actor *actor)
{
    if (actor->terminated || actor->scheduled || actor->running) {
        return OM_STATUS_OK;
    }
    Om_Runtime_Ready_Actor *ready = om_alloc(sizeof(*ready));
    if (ready == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    ready->actor = actor;
    ready->next = NULL;
    actor->scheduled = true;
    if (system->ready_tail == NULL) {
        system->ready_head = ready;
    } else {
        system->ready_tail->next = ready;
    }
    system->ready_tail = ready;
    actor_condition_signal(system);
    return OM_STATUS_OK;
}

Om_Status actor_enqueue_object_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *actor,
    void *message_object)
{
    if (actor == NULL || actor->terminated || actor->stop_requested
        || message_object == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_Message *message = om_alloc(sizeof(*message));
    if (message == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *message = (Om_Runtime_Actor_Message){.object = message_object};
    Om_Status status = actor_schedule_locked(system, actor);
    if (status != OM_STATUS_OK) {
        om_free(message);
        return status;
    }
    if (actor->mailbox_tail == NULL) {
        actor->mailbox_head = message;
    } else {
        actor->mailbox_tail->next = message;
    }
    actor->mailbox_tail = message;
    ++system->outstanding_messages;
    /* actor_schedule_locked signals a worker when this actor becomes ready.
       An already scheduled or running actor needs no second wakeup; the
       completion path broadcasts after it drains or reschedules its mailbox. */
    return OM_STATUS_OK;
}

Om_Status actor_mailbox_root_provider(
    Om_Execution_Context *context,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context)
{
    if (context == NULL || context->actor_system == NULL || visitor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        for (Om_Runtime_Actor_Message *message = actor->mailbox_head;
             message != NULL; message = message->next) {
            const Om_Status status = visitor(message->object,
                                             visitor_context);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return OM_STATUS_OK;
}

Om_Status actor_discard_mailbox_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *actor)
{
    Om_Status status = OM_STATUS_OK;
    Om_Runtime_Actor_Message *message = actor->mailbox_head;
    actor->mailbox_head = NULL;
    actor->mailbox_tail = NULL;
    while (message != NULL) {
        Om_Runtime_Actor_Message *next = message->next;
        if (system->outstanding_messages != 0) {
            --system->outstanding_messages;
        } else {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        om_free(message);
        message = next;
    }
    return status;
}

Om_Runtime_Actor_Message *actor_pop_message_locked(
    Om_Runtime_Actor *actor)
{
    Om_Runtime_Actor_Message *message = actor->mailbox_head;
    if (message != NULL) {
        actor->mailbox_head = message->next;
        if (actor->mailbox_head == NULL) {
            actor->mailbox_tail = NULL;
        }
        message->next = NULL;
    }
    return message;
}

Om_Status om_runtime_actor_send(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref target,
                                const void *message_object)
{
    if (context == NULL || context->actor_system == NULL
        || message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    const Om_Runtime_Object_Header *source = find_object(context,
                                                          message_object);
    if (source == NULL || source->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    actor_mutex_lock(system);
    if (system->shutting_down
        || (context->current_actor != NULL
            && context->current_actor->stop_requested)) {
        actor_mutex_unlock(system);
        return OM_STATUS_CANCELLED;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_CONSOLE) {
        Om_Status status = OM_STATUS_OK;
        status = actor_send_console_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_FILE_SYSTEM) {
        const Om_Status status = actor_send_file_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_TIMER) {
        const Om_Status status = actor_send_timer_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_CLOCK) {
        const Om_Status status = actor_send_clock_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_SECURE_RANDOM) {
        const Om_Status status = actor_send_secure_random_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_LOGGER) {
        const Om_Status status = actor_send_logger_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_NETWORK) {
        const Om_Status status = actor_send_network_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_PROCESS) {
        const Om_Status status = actor_send_process_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_ENVIRONMENT) {
        const Om_Status status = actor_send_environment_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    Om_Runtime_Actor *actor = actor_find_locked(system, target);
    if (actor == NULL && target.id >= OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        const Om_Runtime_Distribution_Hooks *hooks =
            system->distribution_hooks;
        void *distribution_state = system->distribution_state;
        if (hooks != NULL && hooks->send != NULL) {
            ++system->distribution_hooks_active;
        }
        actor_mutex_unlock(system);
        if (hooks == NULL || hooks->send == NULL) return OM_STATUS_NOT_FOUND;
        const Om_Status hook_status = hooks->send(distribution_state, context,
            target.id, message_object);
        actor_mutex_lock(system);
        --system->distribution_hooks_active;
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        return hook_status;
    }
    Om_Status status = actor == NULL || actor->terminated || actor->stop_requested
        ? OM_STATUS_NOT_FOUND : OM_STATUS_OK;
    if (status == OM_STATUS_OK && context->current_actor != NULL
        && context->current_actor->stop_requested) {
        status = OM_STATUS_CANCELLED;
    }
    if (status == OM_STATUS_OK
        && actor_handler_for(actor->descriptor, source->fields.type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    if (status == OM_STATUS_OK) {
        void *mailbox_object = NULL;
        status = om_runtime_object_clone(context, system->mailbox_context,
                                         message_object, &mailbox_object);
        if (status == OM_STATUS_OK) {
            status = actor_enqueue_object_locked(system, actor,
                                                  mailbox_object);
        }
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_send_id(Om_Execution_Context *context,
                                   uint64_t target_id,
                                   const void *message_object)
{
    return om_runtime_actor_send(context,
        (Om_Runtime_Actor_Ref){target_id}, message_object);
}
