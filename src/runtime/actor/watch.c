#include "actor_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

static Om_Status actor_make_down_message_for_id_locked(
    Om_Runtime_Actor_System *system,
    uint64_t actor_id,
    Om_Runtime_Actor_Down_Reason reason,
    void **message_object)
{
    const Om_Runtime_Type_Descriptor *type =
        system->program->actor_down_message_type;
    if (type == NULL || message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *object = NULL;
    Om_Status status = om_runtime_record_create(system->mailbox_context, type,
                                                &object);
    if (status != OM_STATUS_OK) {
        return status;
    }
    unsigned char *payload = object_payload(find_object(system->mailbox_context,
                                                        object));
    const int64_t reason_value = (int64_t)reason;
    (void)memcpy(payload + system->program->actor_down_actor_id_offset,
                 &actor_id, sizeof(actor_id));
    (void)memcpy(payload + system->program->actor_down_reason_offset,
                 &reason_value, sizeof(reason_value));
    *message_object = object;
    return OM_STATUS_OK;
}

static Om_Status actor_make_down_message_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *target,
    Om_Runtime_Actor_Down_Reason reason,
    void **message_object)
{
    if (target == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return actor_make_down_message_for_id_locked(system,
        target->reference.id, reason, message_object);
}

static Om_Status actor_notify_one_watcher_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *target,
    Om_Runtime_Actor_Ref watcher_reference,
    bool remote_watcher)
{
    if (remote_watcher) {
        if (system->distribution_hooks == NULL
            || system->distribution_hooks->notify_watcher == NULL) {
            return OM_STATUS_NOT_FOUND;
        }
        return system->distribution_hooks->notify_watcher(
            system->distribution_state, system->root_context,
            watcher_reference.id, target->reference.id,
            target->failed ? OM_RUNTIME_ACTOR_DOWN_FAILED
                           : OM_RUNTIME_ACTOR_DOWN_NORMAL);
    }
    Om_Runtime_Actor *watcher = actor_find_locked(system, watcher_reference);
    if (watcher == NULL || watcher->terminated || watcher->stop_requested) {
        return OM_STATUS_OK;
    }
    void *message_object = NULL;
    Om_Status status = actor_make_down_message_locked(system, target,
        target->failed ? OM_RUNTIME_ACTOR_DOWN_FAILED
                       : OM_RUNTIME_ACTOR_DOWN_NORMAL,
        &message_object);
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, watcher, message_object);
    }
    return status;
}

Om_Status actor_notify_watchers_locked(Om_Runtime_Actor_System *system,
                                              Om_Runtime_Actor *actor)
{
    Om_Status status = OM_STATUS_OK;
    for (Om_Runtime_Actor_Watcher *watcher = actor->watchers;
         watcher != NULL; watcher = watcher->next) {
        if (!watcher->notified) {
            const Om_Status notify_status = actor_notify_one_watcher_locked(
                system, actor, watcher->actor, watcher->remote);
            watcher->notified = true;
            if (status == OM_STATUS_OK) {
                status = notify_status;
            }
        }
    }
    return status;
}

Om_Status om_runtime_actor_watch(Om_Execution_Context *context,
                                 Om_Runtime_Actor_Ref target)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *target_actor = actor_find_locked(system, target);
    if (target_actor == NULL
        && target.id >= OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        const Om_Runtime_Distribution_Hooks *hooks =
            system->distribution_hooks;
        void *distribution_state = system->distribution_state;
        if (hooks != NULL && hooks->watch != NULL) {
            ++system->distribution_hooks_active;
        }
        actor_mutex_unlock(system);
        if (hooks == NULL || hooks->watch == NULL) return OM_STATUS_NOT_FOUND;
        const Om_Status hook_status = hooks->watch(distribution_state,
            context, target.id);
        actor_mutex_lock(system);
        --system->distribution_hooks_active;
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        return hook_status;
    }
    Om_Status status = target_actor == NULL
        ? OM_STATUS_NOT_FOUND
        : system->program->actor_down_message_type == NULL
            ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    Om_Runtime_Actor *watcher = context->current_actor;
    bool already_watching = false;
    if (status == OM_STATUS_OK) {
        for (Om_Runtime_Actor_Watcher *item = target_actor->watchers;
             item != NULL; item = item->next) {
            if (!item->remote
                && item->actor.id == watcher->reference.id) {
                already_watching = true;
                break;
            }
        }
    }
    if (status == OM_STATUS_OK && !already_watching) {
        Om_Runtime_Actor_Watcher *item = om_alloc(sizeof(*item));
        if (item == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            item->actor = watcher->reference;
            item->remote = false;
            item->notified = false;
            item->next = target_actor->watchers;
            target_actor->watchers = item;
            if (target_actor->terminated) {
                status = actor_notify_one_watcher_locked(
                    system, target_actor, watcher->reference, false);
                item->notified = true;
            }
        }
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_watch_remote(
    Om_Execution_Context *context,
    uint64_t target_local_actor_id,
    uint64_t remote_watcher_ref)
{
    if (context == NULL || context->actor_system == NULL
        || remote_watcher_ref < OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *target = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){target_local_actor_id});
    Om_Status status = target == NULL || target->stop_requested
        ? OM_STATUS_NOT_FOUND
        : system->program->actor_down_message_type == NULL
            ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    bool already_watching = false;
    if (status == OM_STATUS_OK) {
        for (Om_Runtime_Actor_Watcher *item = target->watchers;
             item != NULL; item = item->next) {
            if (item->remote && item->actor.id == remote_watcher_ref) {
                already_watching = true;
                break;
            }
        }
    }
    if (status == OM_STATUS_OK && !already_watching) {
        Om_Runtime_Actor_Watcher *item = om_alloc(sizeof(*item));
        if (item == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            *item = (Om_Runtime_Actor_Watcher){
                .actor = {remote_watcher_ref},
                .remote = true,
                .next = target->watchers,
            };
            target->watchers = item;
            if (target->terminated) {
                status = actor_notify_one_watcher_locked(system, target,
                    item->actor, true);
                item->notified = true;
            }
        }
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_post_remote_down(
    Om_Execution_Context *context,
    uint64_t watcher_local_actor_id,
    uint64_t terminated_actor_id,
    Om_Runtime_Actor_Down_Reason reason)
{
    if (context == NULL || context->actor_system == NULL
        || (reason != OM_RUNTIME_ACTOR_DOWN_NORMAL
            && reason != OM_RUNTIME_ACTOR_DOWN_FAILED
            && reason != OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *watcher = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){watcher_local_actor_id});
    Om_Status status = watcher == NULL || watcher->terminated
        || watcher->stop_requested ? OM_STATUS_NOT_FOUND
        : system->program->actor_down_message_type == NULL
            ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    void *message_object = NULL;
    if (status == OM_STATUS_OK) {
        status = actor_make_down_message_for_id_locked(system,
            terminated_actor_id, reason, &message_object);
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, watcher, message_object);
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_watch_id(Om_Execution_Context *context,
                                    uint64_t target_id)
{
    return om_runtime_actor_watch(context,
        (Om_Runtime_Actor_Ref){target_id});
}
