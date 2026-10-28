#include "actor_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_Runtime_Native_Callback_Registration {
    Om_Runtime_Native_Callback_Function callback;
    Om_Runtime_Actor_System *system;
    uint64_t actor_id;
    const Om_Runtime_Type_Descriptor *message_type;
    size_t in_flight;
    Om_Status first_failure;
    bool active;
    struct Om_Runtime_Native_Callback_Registration *next;
} Om_Runtime_Native_Callback_Registration;

#if defined(_WIN32)
static SRWLOCK native_callback_mutex = SRWLOCK_INIT;
static CONDITION_VARIABLE native_callback_condition =
    CONDITION_VARIABLE_INIT;
#else
static pthread_mutex_t native_callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t native_callback_condition = PTHREAD_COND_INITIALIZER;
#endif
static Om_Runtime_Native_Callback_Registration *native_callback_registrations;

static void native_callback_lock(void)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(&native_callback_mutex);
#else
    (void)pthread_mutex_lock(&native_callback_mutex);
#endif
}

static void native_callback_unlock(void)
{
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&native_callback_mutex);
#else
    (void)pthread_mutex_unlock(&native_callback_mutex);
#endif
}

static void native_callback_wait(void)
{
#if defined(_WIN32)
    (void)SleepConditionVariableSRW(&native_callback_condition,
        &native_callback_mutex, INFINITE, 0);
#else
    (void)pthread_cond_wait(&native_callback_condition,
                            &native_callback_mutex);
#endif
}

static void native_callback_broadcast(void)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&native_callback_condition);
#else
    (void)pthread_cond_broadcast(&native_callback_condition);
#endif
}

static bool native_callback_message_supported(
    const Om_Runtime_Type_Descriptor *message_type)
{
    if (!descriptor_valid_for_record(message_type)
        || message_type->kind != OM_RUNTIME_TYPE_MESSAGE
        || (message_type->field_count != 0 && message_type->fields == NULL)) {
        return false;
    }
    for (size_t index = 0; index < message_type->field_count; ++index) {
        const Om_Runtime_Type_Descriptor *field_type =
            message_type->fields[index].type;
        if (field_type == NULL
            || (field_type->kind != OM_RUNTIME_TYPE_BOOL
                && field_type->kind != OM_RUNTIME_TYPE_INT
                && field_type->kind != OM_RUNTIME_TYPE_FLOAT)) {
            return false;
        }
    }
    return true;
}

static bool native_callback_address_in_list_locked(
    Om_Runtime_Native_Callback_Function callback,
    const Om_Runtime_Native_Callback_Registration *staged)
{
    for (const Om_Runtime_Native_Callback_Registration *item =
             native_callback_registrations;
         item != NULL; item = item->next) {
        if (item->callback == callback) return true;
    }
    for (const Om_Runtime_Native_Callback_Registration *item = staged;
         item != NULL; item = item->next) {
        if (item->callback == callback) return true;
    }
    return false;
}

Om_Status om_runtime_native_callback_register_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *const *candidate_sets,
    const uint64_t *candidate_counts,
    const Om_Runtime_Type_Descriptor *const *message_types,
    Om_Runtime_Native_Callback_Function *registered_functions)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL || callback_count == 0
        || callback_count > SIZE_MAX
        || candidate_sets == NULL || candidate_counts == NULL
        || message_types == NULL || registered_functions == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t count = (size_t)callback_count;
    if (count > SIZE_MAX / sizeof(Om_Runtime_Native_Callback_Registration *)
        || count > SIZE_MAX / sizeof(Om_Runtime_Native_Callback_Function)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *actor = actor_find_locked(system,
        context->current_actor->reference);
    Om_Status status = system->shutting_down || actor == NULL
            || actor != context->current_actor || actor->terminated
            || actor->stop_requested
        ? OM_STATUS_CANCELLED : OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        const Om_Runtime_Type_Descriptor *message_type = message_types[index];
        if (candidate_sets[index] == NULL || candidate_counts[index] == 0
            || candidate_counts[index] > 4096
            || !native_callback_message_supported(message_type)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else if (actor_handler_for(actor->descriptor, message_type) == NULL) {
            status = OM_STATUS_NOT_FOUND;
        }
    }
    actor_mutex_unlock(system);
    if (status != OM_STATUS_OK) return status;

    Om_Runtime_Native_Callback_Registration **entries = om_alloc_array(
        count, sizeof(*entries));
    Om_Runtime_Native_Callback_Function *selected = om_alloc_array(
        count, sizeof(*selected));
    if (entries == NULL || selected == NULL) {
        om_free(entries);
        om_free(selected);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(entries, 0, count * sizeof(*entries));
    for (size_t index = 0; index < count; ++index) {
        entries[index] = om_alloc(sizeof(*entries[index]));
        if (entries[index] == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        *entries[index] = (Om_Runtime_Native_Callback_Registration){
            .system = system,
            .actor_id = context->current_actor->reference.id,
            .message_type = message_types[index],
            .first_failure = OM_STATUS_OK,
            .active = true,
        };
    }

    native_callback_lock();
    Om_Runtime_Native_Callback_Registration *staged = NULL;
    Om_Runtime_Native_Callback_Registration *staged_tail = NULL;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        Om_Runtime_Native_Callback_Function callback = NULL;
        for (uint64_t candidate_index = 0;
             candidate_index < candidate_counts[index]; ++candidate_index) {
            Om_Runtime_Native_Callback_Function candidate =
                candidate_sets[index][candidate_index];
            if (candidate != NULL
                && !native_callback_address_in_list_locked(candidate, staged)) {
                callback = candidate;
                break;
            }
        }
        if (callback == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        entries[index]->callback = callback;
        selected[index] = callback;
        if (staged_tail == NULL) staged = entries[index];
        else staged_tail->next = entries[index];
        staged_tail = entries[index];
    }
    if (status == OM_STATUS_OK) {
        staged_tail->next = native_callback_registrations;
        native_callback_registrations = staged;
        (void)memcpy(registered_functions, selected,
                     count * sizeof(*registered_functions));
    }
    native_callback_unlock();

    if (status != OM_STATUS_OK) {
        for (size_t index = 0; index < count; ++index) {
            om_free(entries[index]);
        }
    }
    om_free(entries);
    om_free(selected);
    return status;
}

Om_Status om_runtime_native_callback_unregister_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *registered_functions)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL || callback_count == 0
        || callback_count > SIZE_MAX || registered_functions == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t count = (size_t)callback_count;
    if (count > SIZE_MAX / sizeof(Om_Runtime_Native_Callback_Registration *)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Native_Callback_Registration **entries = om_alloc_array(
        count, sizeof(*entries));
    if (entries == NULL) return OM_STATUS_OUT_OF_MEMORY;
    (void)memset(entries, 0, count * sizeof(*entries));

    Om_Status status = OM_STATUS_OK;
    bool registrations_removed = false;
    native_callback_lock();
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        for (Om_Runtime_Native_Callback_Registration *item =
                 native_callback_registrations;
             item != NULL; item = item->next) {
            if (item->active && item->callback == registered_functions[index]
                && item->system == context->actor_system
                && item->actor_id == context->current_actor->reference.id) {
                entries[index] = item;
                break;
            }
        }
        if (entries[index] == NULL) status = OM_STATUS_NOT_FOUND;
        for (size_t previous = 0; status == OM_STATUS_OK && previous < index;
             ++previous) {
            if (entries[previous] == entries[index]) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < count; ++index) {
            entries[index]->active = false;
        }
        bool in_flight = false;
        do {
            in_flight = false;
            for (size_t index = 0; index < count; ++index) {
                in_flight = in_flight || entries[index]->in_flight != 0;
            }
            if (in_flight) native_callback_wait();
        } while (in_flight);

        for (size_t index = 0; index < count; ++index) {
            if (status == OM_STATUS_OK
                && entries[index]->first_failure != OM_STATUS_OK) {
                status = entries[index]->first_failure;
            }
            Om_Runtime_Native_Callback_Registration **link =
                &native_callback_registrations;
            while (*link != NULL && *link != entries[index]) {
                link = &(*link)->next;
            }
            if (*link == entries[index]) *link = entries[index]->next;
        }
        registrations_removed = true;
    }
    native_callback_unlock();
    if (registrations_removed) {
        for (size_t index = 0; index < count; ++index) {
            om_free(entries[index]);
        }
    }
    om_free(entries);
    return status;
}

Om_Status om_runtime_native_callback_post(
    Om_Runtime_Native_Callback_Function callback,
    const uint64_t *value_bits,
    uint64_t value_count)
{
    if (callback == NULL || value_count > SIZE_MAX
        || (value_count != 0 && value_bits == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    native_callback_lock();
    Om_Runtime_Native_Callback_Registration *registration =
        native_callback_registrations;
    while (registration != NULL
        && (!registration->active || registration->callback != callback)) {
        registration = registration->next;
    }
    if (registration == NULL) {
        native_callback_unlock();
        return OM_STATUS_NOT_FOUND;
    }
    if (registration->in_flight == SIZE_MAX) {
        native_callback_unlock();
        return OM_STATUS_OUT_OF_RANGE;
    }
    ++registration->in_flight;
    Om_Runtime_Actor_System *system = registration->system;
    const uint64_t actor_id = registration->actor_id;
    const Om_Runtime_Type_Descriptor *message_type =
        registration->message_type;
    native_callback_unlock();

    Om_Status status = OM_STATUS_OK;
    actor_mutex_lock(system);
    Om_Runtime_Actor *actor = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){actor_id});
    if (system->shutting_down || actor == NULL || actor->terminated
        || actor->stop_requested) {
        status = OM_STATUS_CANCELLED;
    } else if (!native_callback_message_supported(message_type)
        || message_type->field_count != (size_t)value_count
        || actor_handler_for(actor->descriptor, message_type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    void *message_object = NULL;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(system->mailbox_context,
            message_type, &message_object);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < (size_t)value_count; ++index) {
        Om_Runtime_Value value = {0};
        switch (message_type->fields[index].type->kind) {
        case OM_RUNTIME_TYPE_BOOL:
            value.boolean = value_bits[index] != 0;
            break;
        case OM_RUNTIME_TYPE_INT:
            (void)memcpy(&value.integer, &value_bits[index],
                         sizeof(value.integer));
            break;
        case OM_RUNTIME_TYPE_FLOAT:
            (void)memcpy(&value.floating, &value_bits[index],
                         sizeof(value.floating));
            break;
        default:
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set(system->mailbox_context,
                message_object, index, value);
        }
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, actor, message_object);
    }
    actor_mutex_unlock(system);

    native_callback_lock();
    if (status != OM_STATUS_OK && registration->first_failure == OM_STATUS_OK) {
        registration->first_failure = status;
    }
    --registration->in_flight;
    native_callback_broadcast();
    native_callback_unlock();
    return status;
}
