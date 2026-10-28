#include "actor_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

static Om_Status system_task_run(void *context);
static void system_file_complete(void *context, Om_Status task_status);
static void system_task_destroy(void *context);

static const Om_Runtime_Type_Descriptor *actor_system_message_type(
    const Om_Runtime_Actor_System *system,
    Om_System_Message_Id message_id)
{
    if (system == NULL || system->program == NULL) return NULL;
    for (size_t index = 0; index < system->program->type_count; ++index) {
        const Om_Runtime_Type_Descriptor *type = system->program->types[index];
        if (type != NULL && type->system_message_id == message_id) {
            return type;
        }
    }
    return NULL;
}

static Om_Status actor_deliver_result_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient_id,
    Om_System_Message_Id completion_id,
    bool succeeded,
    const uint8_t *success_bytes,
    size_t success_length,
    const char *error_text)
{

    Om_Runtime_Actor *recipient = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){recipient_id});
    if (recipient == NULL || recipient->terminated
        || recipient->stop_requested) {
        return OM_STATUS_OK;
    }
    const Om_Runtime_Type_Descriptor *completion_type =
        actor_system_message_type(system, completion_id);
    if (completion_type == NULL || completion_type->kind != OM_RUNTIME_TYPE_MESSAGE
        || completion_type->field_count != 1
        || completion_type->fields[0].type == NULL
        || !result_descriptor_valid(completion_type->fields[0].type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *result_type =
        completion_type->fields[0].type;
    const Om_Runtime_Field_Descriptor *payload_field =
        &result_type->fields[succeeded ? 1 : 2];
    if ((succeeded && payload_field->type->kind != OM_RUNTIME_TYPE_UNIT
         && payload_field->type->kind != OM_RUNTIME_TYPE_STRING)
        || (!succeeded && payload_field->type->kind != OM_RUNTIME_TYPE_STRING)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    void *completion_object = NULL;
    Om_Status status = om_runtime_record_create(system->mailbox_context,
        completion_type, &completion_object);

    if (status != OM_STATUS_OK) return status;
    void *roots[] = {completion_object, NULL};
    Om_Runtime_Root_Frame *root = om_runtime_roots_push(
        system->mailbox_context, roots, 2);

    if (root == NULL) return OM_STATUS_OUT_OF_MEMORY;

    unsigned char *result_value = om_alloc(result_type->size);
    if (result_value == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    } else {
        (void)memset(result_value, 0, result_type->size);
        (void)memcpy(result_value + result_type->fields[0].offset,
                     &succeeded, sizeof(succeeded));
        if (payload_field->type->kind == OM_RUNTIME_TYPE_STRING) {
            const uint8_t *text = success_bytes;
            size_t length = success_length;
            static const uint8_t fallback_error[] = "system operation failed";
            if (!succeeded) {
                text = error_text == NULL ? fallback_error
                    : (const uint8_t *)error_text;
                length = strlen((const char *)text);
            }
            status = om_runtime_string_create_utf8_default(
                system->mailbox_context, text, length, &roots[1]);

            if (status == OM_STATUS_OK) {
                (void)memcpy(result_value + payload_field->offset, &roots[1],
                             sizeof(roots[1]));
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set_value(system->mailbox_context,
                completion_object, 0, result_value);

        }
    }
    om_free(result_value);
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, recipient,
                                              completion_object);

    }
    const Om_Status pop_status = om_runtime_roots_pop(
        system->mailbox_context, root);

    if (status == OM_STATUS_OK) status = pop_status;
    return status;
}

static Om_Status actor_deliver_message_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient_id,
    Om_Execution_Context *source_context,
    const void *message_object)
{
    Om_Runtime_Actor *recipient = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){recipient_id});
    if (recipient == NULL || recipient->terminated
        || recipient->stop_requested) {
        return OM_STATUS_OK;
    }
    const Om_Runtime_Object_Header *source = find_object(source_context,
                                                          message_object);
    if (source == NULL || source->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *mailbox_object = NULL;
    Om_Status status = om_runtime_object_clone(source_context,
        system->mailbox_context, message_object, &mailbox_object);
    if (status == OM_STATUS_OK
        && actor_handler_for(recipient->descriptor, source->fields.type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, recipient, mailbox_object);
    }
    return status;
}

Om_Status actor_send_console_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL
        || message_type->system_message_id != OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
        || message_type->field_count != 2) {
        return OM_STATUS_NOT_FOUND;
    }

    void *text_object = NULL;
    uint64_t callback_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                             &text_object);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &callback_id);
    }
    Om_String_View text = {0};
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(source_context, text_object, &text);
    }
    if (status != OM_STATUS_OK) return status;

    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE,
        .recipient_id = callback_id,
        .input_length = text.length,
    };
    if (text.length != 0) {
        task->input_bytes = om_alloc(text.length);
        if (task->input_bytes == NULL) {
            system_task_destroy(task);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(task->input_bytes, text.data, text.length);
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

static Om_Status system_task_copy_string(Om_Execution_Context *context,
                                        const void *string_object,
                                        char **result,
                                        size_t *result_length)
{
    if (context == NULL || string_object == NULL || result == NULL
        || result_length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    *result_length = 0;
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string_object, &view);
    if (status != OM_STATUS_OK) return status;
    if (view.length != 0 && memchr(view.data, '\0', view.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(view.length, 1, &allocation_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    char *copy = om_alloc(allocation_size);
    if (copy == NULL) return OM_STATUS_OUT_OF_MEMORY;
    if (view.length != 0) (void)memcpy(copy, view.data, view.length);
    copy[view.length] = '\0';
    *result = copy;
    *result_length = view.length;
    return OM_STATUS_OK;
}

static FILE *system_file_open(const char *path,
                              bool write,
                              Om_Status *status)
{
    if (path == NULL || status == NULL) return NULL;
#if defined(_WIN32)
    const size_t path_length = strlen(path);
    if (path_length >= (size_t)INT_MAX) {
        *status = OM_STATUS_OUT_OF_RANGE;
        return NULL;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        *status = OM_STATUS_IO_ERROR;
        return NULL;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        *status = OM_STATUS_OUT_OF_MEMORY;
        return NULL;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        *status = OM_STATUS_IO_ERROR;
        return NULL;
    }
    FILE *file = NULL;
    const errno_t open_status = _wfopen_s(&file, wide_path,
                                          write ? L"wb" : L"rb");
    om_free(wide_path);
    *status = open_status != 0 || file == NULL
        ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
    return file;
#else
    FILE *file = fopen(path, write ? "wb" : "rb");
    *status = file == NULL ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
    return file;
#endif
}

static Om_Status system_file_read(Om_Runtime_System_Task *task)
{

    Om_Status status = OM_STATUS_OK;
    FILE *file = system_file_open(task->path, false, &status);

    if (file == NULL) return status;
    size_t capacity = 0;
    for (;;) {
        if (task->result_length == capacity) {
            size_t next_capacity = capacity == 0 ? 8192 : capacity;
            if (capacity != 0) {
                if (capacity > SIZE_MAX / 2) {
                    status = OM_STATUS_OUT_OF_RANGE;
                    break;
                }
                next_capacity = capacity * 2;
            }
            uint8_t *next = om_realloc_array(task->result_bytes,
                next_capacity, sizeof(*next));
            if (next == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            task->result_bytes = next;
            capacity = next_capacity;
        }
        const size_t available = capacity - task->result_length;
        const size_t received = fread(task->result_bytes + task->result_length,
            1, available, file);
        task->result_length += received;
        if (ferror(file)) {
            status = OM_STATUS_IO_ERROR;
            break;
        }
        if (feof(file)) break;
        if (received == 0) {
            status = OM_STATUS_IO_ERROR;
            break;
        }
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        status = OM_STATUS_IO_ERROR;
    }

    return status;
}

static Om_Status system_file_write(Om_Runtime_System_Task *task)
{
    Om_Status status = OM_STATUS_OK;
    FILE *file = system_file_open(task->path, true, &status);
    if (file == NULL) return status;
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, file)
            != task->input_length) {
        status = OM_STATUS_IO_ERROR;
    }
    if (status == OM_STATUS_OK && fflush(file) != 0) {
        status = OM_STATUS_IO_ERROR;
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        status = OM_STATUS_IO_ERROR;
    }
    return status;
}

static Om_Status system_console_write(Om_Runtime_System_Task *task)
{
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, stdout)
            != task->input_length) {
        return OM_STATUS_IO_ERROR;
    }
    return fflush(stdout) == 0 ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}

static Om_Status system_task_run(void *context)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if (task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE) {
        return system_console_write(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ) {
        return system_file_read(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE) {
        return system_file_write(task);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static void system_file_complete(void *context, Om_Status task_status)
{

    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->system == NULL) return;
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);

    if (!system->shutting_down) {
        if (task_status == OM_STATUS_OK
            && task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
            && om_utf8_validate((Om_Const_Byte_Slice){task->result_bytes,
                task->result_length}) != OM_STATUS_OK) {
            task_status = OM_STATUS_IO_ERROR;
        }
        const Om_System_Message_Id completion_id =
            task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
                ? OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? OM_SYSTEM_MESSAGE_ID_FILE_READ_COMPLETED
                : OM_SYSTEM_MESSAGE_ID_FILE_WRITE_COMPLETED;
        const char *error_text =
            task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
                ? "console write failed"
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? "file read failed or contents are not valid UTF-8"
                : "file write failed";
        const Om_Status delivery_status = actor_deliver_result_locked(system,
            task->recipient_id, completion_id, task_status == OM_STATUS_OK,
            task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? task->result_bytes : NULL,
            task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? task->result_length : 0,
            error_text);

        if (delivery_status != OM_STATUS_OK) {
            actor_fail_locked(system, task->recipient_id);
        }
    }
    if (system->outstanding_messages != 0) {
        --system->outstanding_messages;
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

}

static void timer_task_complete(void *context, Om_Status task_status)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->system == NULL) return;
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);
    if (task_status == OM_STATUS_OK && !system->shutting_down) {
        const Om_Status delivery_status = actor_deliver_message_locked(system,
            task->recipient_id, task->payload_context, task->payload_object);
        if (delivery_status != OM_STATUS_OK) {
            actor_fail_locked(system, task->recipient_id);
        }
    }
    if (system->outstanding_messages != 0) {
        --system->outstanding_messages;
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);
}

static void system_task_destroy(void *context)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL) return;

    om_free(task->path);

    om_free(task->input_bytes);

    om_free(task->result_bytes);

    if (task->payload_context != NULL) {
        om_runtime_context_destroy(task->payload_context);
    }
    om_free(task);

}

Om_Status actor_send_file_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL) return OM_STATUS_NOT_FOUND;
    const Om_System_Message_Id operation = message_type->system_message_id;
    const size_t expected_fields = operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        ? 2 : operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE ? 3 : 0;
    if (expected_fields == 0 || message_type->field_count != expected_fields) {
        return OM_STATUS_NOT_FOUND;
    }
    void *path_object = NULL;
    void *contents_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                              &path_object);
    if (status == OM_STATUS_OK && operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &contents_object);
    }
    const size_t recipient_field = operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        ? 1 : 2;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object,
            recipient_field, &recipient_id);
    }
    if (status != OM_STATUS_OK) return status;

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = operation,
        .recipient_id = recipient_id,
    };
    size_t path_length = 0;
    status = system_task_copy_string(source_context, path_object, &task->path,
                                     &path_length);
    if (status == OM_STATUS_OK
        && operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE) {
        char *contents = NULL;
        status = system_task_copy_string(source_context, contents_object,
                                         &contents, &task->input_length);
        task->input_bytes = (uint8_t *)contents;
    }
    (void)path_length;
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        system_task_destroy(task);
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

Om_Status actor_send_timer_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL
        || message_type->system_message_id != OM_SYSTEM_MESSAGE_ID_TIMER_AFTER
        || message_type->field_count != 3) {
        return OM_STATUS_NOT_FOUND;
    }
    int64_t delay = 0;
    void *payload_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                              &delay);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &payload_object);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 2,
                                       &recipient_id);
    }
    if (status != OM_STATUS_OK || delay < 0) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    if (system->timer_service == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->timer_service == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = OM_SYSTEM_MESSAGE_ID_TIMER_AFTER,
        .recipient_id = recipient_id,
    };
    task->payload_context = om_runtime_context_create(
        system->program->collection_threshold);
    if (task->payload_context == NULL) {
        system_task_destroy(task);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_Runtime_Object_Header *payload = find_object(source_context,
                                                           payload_object);
    if (payload == NULL || payload->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        system_task_destroy(task);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = om_runtime_object_clone(source_context, task->payload_context,
                                     payload_object, &task->payload_object);
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    uint64_t timer_id = 0;
    ++system->outstanding_messages;
    status = om_timer_service_schedule(system->timer_service,
        (Om_Timer_Task){
            .delay_milliseconds = (uint64_t)delay,
            .complete = timer_task_complete,
            .destroy = system_task_destroy,
            .context = task,
        }, &timer_id);
    (void)timer_id;
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}
