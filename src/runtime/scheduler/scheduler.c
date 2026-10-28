#include "actor_internal.h"

#include "ombasic/foundation/memory.h"

static Om_Status actor_program_validate(
    const Om_Runtime_Program_Descriptor *program)
{
    if (program == NULL
        || (program->type_count != 0 && program->types == NULL)
        || (program->actor_count != 0 && program->actors == NULL)
        || (program->source_span_count != 0 && program->source_spans == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (program->actor_down_message_type != NULL) {
        const Om_Runtime_Type_Descriptor *down =
            program->actor_down_message_type;
        if (down->kind != OM_RUNTIME_TYPE_MESSAGE
            || !descriptor_valid_for_record(down)
            || program->actor_down_actor_id_offset > down->size
            || sizeof(uint64_t) > down->size
                - program->actor_down_actor_id_offset
            || program->actor_down_reason_offset > down->size
            || sizeof(int64_t) > down->size
                - program->actor_down_reason_offset) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    for (size_t actor_index = 0; actor_index < program->actor_count;
         ++actor_index) {
        const Om_Runtime_Actor_Descriptor *actor = program->actors[actor_index];
        if (actor == NULL || actor->canonical_name == NULL
            || actor->state_type == NULL
            || actor->state_type->kind != OM_RUNTIME_TYPE_RECORD
            || !descriptor_valid_for_record(actor->state_type)
            || (actor->handler_count != 0 && actor->handlers == NULL)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t handler_index = 0; handler_index < actor->handler_count;
             ++handler_index) {
            const Om_Runtime_Actor_Handler_Descriptor *handler =
                &actor->handlers[handler_index];
            if (handler->message_type == NULL || handler->handler == NULL
                || handler->message_type->kind != OM_RUNTIME_TYPE_MESSAGE
                || !descriptor_valid_for_record(handler->message_type)) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            for (size_t prior = 0; prior < handler_index; ++prior) {
                if (actor->handlers[prior].message_type->fingerprint
                    == handler->message_type->fingerprint) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
            }
        }
        for (size_t prior = 0; prior < actor_index; ++prior) {
            if (program->actors[prior]->fingerprint == actor->fingerprint) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    return OM_STATUS_OK;
}

static int actor_handler_entry(Om_Execution_Context *context)
{
    Om_Runtime_Actor *actor = context->current_actor;
    if (actor == NULL || context->pending_actor_handler == NULL) {
        context->pending_actor_status = OM_STATUS_INVALID_ARGUMENT;
        return 1;
    }
    context->pending_actor_status = context->pending_actor_handler(context,
        context->pending_actor_state, context->pending_actor_message);
    return context->pending_actor_status == OM_STATUS_OK ? 0 : 1;
}

static void actor_worker_loop(Om_Runtime_Actor_System *system)
{
    for (;;) {
        Om_Execution_Context *reclaim_context = NULL;
        bool reclaim_actor_context = false;
        actor_mutex_lock(system);
        while (system->ready_head == NULL && !system->stopping) {
            actor_condition_wait(system);
        }
        if (system->ready_head == NULL && system->stopping) {
            actor_mutex_unlock(system);
            return;
        }
        Om_Runtime_Ready_Actor *ready = system->ready_head;
        system->ready_head = ready->next;
        if (system->ready_head == NULL) {
            system->ready_tail = NULL;
        }
        Om_Runtime_Actor *actor = ready->actor;

        actor->scheduled = false;
        om_free(ready);
        if (actor->terminated) {
            actor_mutex_unlock(system);
            continue;
        }
        Om_Runtime_Actor_Message *message = actor_pop_message_locked(actor);
        if (message == NULL && !actor->stop_requested) {
            actor_mutex_unlock(system);
            continue;
        }
        void *message_object = NULL;
        const Om_Runtime_Actor_Handler_Descriptor *handler = NULL;
        Om_Runtime_Root_Frame *message_root = NULL;
        bool handler_succeeded = true;
        if (message != NULL) {
            Om_Status status = om_runtime_object_clone(system->mailbox_context,
                actor->context, message->object, &message_object);
            if (status == OM_STATUS_OK) {
                message_root = om_runtime_roots_push(actor->context,
                    &message_object, 1);
                if (message_root == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
            const Om_Runtime_Object_Header *object = status == OM_STATUS_OK
                ? find_object(actor->context, message_object) : NULL;
            if (status == OM_STATUS_OK && object == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                handler = actor_handler_for(actor->descriptor,
                                             object->fields.type);
                if (handler == NULL) {
                    status = OM_STATUS_NOT_FOUND;
                }
            }
            om_free(message);
            if (system->outstanding_messages != 0) {
                --system->outstanding_messages;
            } else {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                actor->context->current_actor = actor;
                actor->context->current_actor_message_object = message_object;
                actor->context->pending_actor_handler = handler->handler;
                actor->context->pending_actor_state = actor->state_payload;
                actor->context->pending_actor_message =
                    object_payload_const(find_object(actor->context,
                                                     message_object));
                actor->context->pending_actor_status = OM_STATUS_OK;
            } else {
                handler_succeeded = false;
            }
        }
        actor->running = true;
        ++system->running_handlers;
        actor_mutex_unlock(system);

        if (message != NULL && handler_succeeded) {

            const int handler_result = om_runtime_run(actor->context,
                                                       actor_handler_entry);

            handler_succeeded = handler_result == 0
                && actor->context->pending_actor_status == OM_STATUS_OK;
        }
        actor->context->current_actor = NULL;
        actor->context->current_actor_message_object = NULL;
        actor->context->pending_actor_handler = NULL;
        actor->context->pending_actor_state = NULL;
        actor->context->pending_actor_message = NULL;
        if (message_root != NULL) {
            (void)om_runtime_roots_pop(actor->context, message_root);
        }

        actor_mutex_lock(system);
        if (!handler_succeeded) {
            actor->failed = true;
            actor->stop_requested = true;
        }
        Om_Status finish_status = OM_STATUS_OK;
        if (actor->stop_requested || system->shutting_down) {
            (void)actor_discard_mailbox_locked(system, actor);
            reclaim_actor_context = true;
        } else {
            actor->running = false;
            if (actor->mailbox_head != NULL) {
                finish_status = actor_schedule_locked(system, actor);
                if (finish_status != OM_STATUS_OK) {
                    actor->running = true;
                    actor->failed = true;
                    actor->stop_requested = true;
                    (void)actor_discard_mailbox_locked(system, actor);
                    reclaim_actor_context = true;
                }
            }
        }
        if (finish_status != OM_STATUS_OK) {
            actor->failed = true;
        }
        if (reclaim_actor_context) {
            reclaim_context = actor->context;
            actor->context = NULL;
            actor->state_object = NULL;
            actor->state_payload = NULL;
            actor->state_root = NULL;
        } else {
            if (system->running_handlers != 0) {
                --system->running_handlers;
            }
        }
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        if (reclaim_actor_context) {
            /* Native release functions may run here; do not hold the Actor
               system mutex while reclaiming the stopped Actor heap. */

            om_runtime_context_destroy(reclaim_context);

            actor_mutex_lock(system);
            if (system->running_handlers != 0) {
                --system->running_handlers;
            }
            actor->running = false;
            actor->terminated = true;
            if (!system->shutting_down) {
                finish_status = actor_notify_watchers_locked(system, actor);
                if (finish_status != OM_STATUS_OK) {
                    actor->failed = true;
                }
            }
            actor_condition_broadcast(system);
            actor_mutex_unlock(system);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI actor_worker_entry(LPVOID argument)
{
    actor_worker_loop((Om_Runtime_Actor_System *)argument);
    return 0;
}
#else
static void *actor_worker_entry(void *argument)
{
    actor_worker_loop((Om_Runtime_Actor_System *)argument);
    return NULL;
}
#endif

static bool actor_all_terminated_locked(Om_Runtime_Actor_System *system)
{
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        if (!actor->terminated) {
            return false;
        }
    }
    return system->ready_head == NULL && system->running_handlers == 0
        && system->outstanding_messages == 0;
}

void destroy_actor_system(Om_Runtime_Actor_System *system)
{
    if (system == NULL) {
        return;
    }
    actor_mutex_lock(system);
    system->shutting_down = true;
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        if (actor->terminated) {
            continue;
        }
        actor->stop_requested = true;
        (void)actor_discard_mailbox_locked(system, actor);
        if (!actor->running) {
            const Om_Status status = actor_schedule_locked(system, actor);
            if (status != OM_STATUS_OK) {
                actor->terminated = true;
            }
        }
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

    /* Stop asynchronous producers before waiting for their outstanding work.
       Their cancellation callbacks release the operation counts and do not
       deliver new messages once shutting_down has been set. */
    om_timer_service_destroy(system->timer_service);
    system->timer_service = NULL;
    om_blocking_pool_destroy(system->blocking_pool);
    system->blocking_pool = NULL;

    actor_mutex_lock(system);
    while (!actor_all_terminated_locked(system)) {
        actor_condition_wait(system);
    }
    system->stopping = true;
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

    if (!system->joined) {
        for (size_t index = 0; index < system->worker_count; ++index) {
#if defined(_WIN32)
            (void)WaitForSingleObject(system->threads[index].native, INFINITE);
            (void)CloseHandle(system->threads[index].native);
#else
            (void)pthread_join(system->threads[index].native, NULL);
#endif
        }
        system->joined = true;
    }
    Om_Runtime_Actor *actor = system->actors;
    while (actor != NULL) {
        Om_Runtime_Actor *next = actor->next;
        Om_Runtime_Actor_Watcher *watcher = actor->watchers;
        while (watcher != NULL) {
            Om_Runtime_Actor_Watcher *next_watcher = watcher->next;
            om_free(watcher);
            watcher = next_watcher;
        }
        om_runtime_context_destroy(actor->context);
        om_free(actor);
        actor = next;
    }
    Om_Runtime_Ready_Actor *ready = system->ready_head;
    while (ready != NULL) {
        Om_Runtime_Ready_Actor *next = ready->next;
        om_free(ready);
        ready = next;
    }
    om_runtime_context_destroy(system->mailbox_context);
    om_free(system->threads);
    actor_sync_destroy(system);
    om_free(system);
}

Om_Status om_runtime_actor_system_start(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    size_t worker_count)
{
    if (context == NULL || context->actor_system != NULL
        || context->current_actor != NULL || worker_count > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = om_runtime_validate_program(context, program);
    if (status == OM_STATUS_OK) {
        status = actor_program_validate(program);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (worker_count == 0) {
        worker_count = 4;
    }
    Om_Runtime_Actor_System *system = om_alloc(sizeof(*system));
    if (system == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *system = (Om_Runtime_Actor_System){
        .root_context = context,
        .program = program,
        .worker_count = worker_count,
        .next_actor_id = 1,
    };
    if (!actor_sync_init(system)) {
        om_free(system);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    system->mailbox_context = om_runtime_context_create(
        program->collection_threshold);
    if (system->mailbox_context == NULL) {
        actor_sync_destroy(system);
        om_free(system);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    system->mailbox_context->actor_system = system;
    system->mailbox_context->is_actor_mailbox = true;
    system->mailbox_context->external_root_provider =
        actor_mailbox_root_provider;
    status = om_runtime_set_source_spans(system->mailbox_context,
        program->source_spans, program->source_span_count);
    if (status == OM_STATUS_OK) {
        status = om_blocking_pool_create(&system->blocking_pool,
            worker_count, OM_RUNTIME_SYSTEM_QUEUE_CAPACITY);
    }
    if (status == OM_STATUS_OK) {
        status = om_timer_service_create(&system->timer_service,
            OM_RUNTIME_SYSTEM_QUEUE_CAPACITY);
    }
    system->threads = om_alloc_array(worker_count, sizeof(*system->threads));
    if (status == OM_STATUS_OK && system->threads == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status != OM_STATUS_OK) {
        om_timer_service_destroy(system->timer_service);
        om_blocking_pool_destroy(system->blocking_pool);
        om_runtime_context_destroy(system->mailbox_context);
        actor_sync_destroy(system);
        om_free(system->threads);
        om_free(system);
        return status;
    }
    context->actor_system = system;
    context->owns_actor_system = true;
    if (program->source_spans != NULL || program->source_span_count != 0) {
        status = om_runtime_set_source_spans(context, program->source_spans,
                                             program->source_span_count);
    }
    size_t started = 0;
    for (; status == OM_STATUS_OK && started < worker_count; ++started) {
#if defined(_WIN32)
        system->threads[started].native = CreateThread(NULL, 0,
            actor_worker_entry, system, 0, NULL);
        if (system->threads[started].native == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
#else
        if (pthread_create(&system->threads[started].native, NULL,
                           actor_worker_entry, system) != 0) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
#endif
    }
    if (status != OM_STATUS_OK) {
        actor_mutex_lock(system);
        system->stopping = true;
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        for (size_t index = 0; index < started; ++index) {
#if defined(_WIN32)
            (void)WaitForSingleObject(system->threads[index].native, INFINITE);
            (void)CloseHandle(system->threads[index].native);
#else
            (void)pthread_join(system->threads[index].native, NULL);
#endif
        }
        context->actor_system = NULL;
        context->owns_actor_system = false;
        om_timer_service_destroy(system->timer_service);
        om_blocking_pool_destroy(system->blocking_pool);
        om_runtime_context_destroy(system->mailbox_context);
        actor_sync_destroy(system);
        om_free(system->threads);
        om_free(system);
        return status;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_actor_system_wait(Om_Execution_Context *context)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    while (system->outstanding_messages != 0
           || system->running_handlers != 0) {
        actor_condition_wait(system);
    }
    actor_mutex_unlock(system);
    return OM_STATUS_OK;
}
