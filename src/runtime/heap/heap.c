#include "../vm_internal.h"

#include <stddef.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

Om_VM_Object *om_vm_heap_allocate(Om_VM_Heap *heap, size_t size,
                                  Om_VM_Object_Kind kind)
{
    if (heap == NULL || size < sizeof(Om_VM_Object)) {
        if (heap != NULL) {
            heap->last_status = OM_STATUS_INVALID_ARGUMENT;
        }
        return NULL;
    }
    size_t new_total = 0;
    if (!om_checked_size_add(heap->bytes_allocated, size, &new_total)
        || new_total > heap->max_bytes) {
        heap->gc_pending = true;
        heap->last_status = OM_STATUS_OUT_OF_RANGE;
        return NULL;
    }
    bool vm_bytes_reserved = false;
    if (heap->vm != NULL) {
        size_t current = atomic_load_explicit(
            &heap->vm->heap_bytes_allocated, memory_order_relaxed);
        for (;;) {
            if (size > heap->vm->limits.max_heap_bytes
                || current > heap->vm->limits.max_heap_bytes - size) {
                heap->gc_pending = true;
                heap->last_status = OM_STATUS_OUT_OF_RANGE;
                return NULL;
            }
            if (atomic_compare_exchange_weak_explicit(
                    &heap->vm->heap_bytes_allocated, &current, current + size,
                    memory_order_acq_rel, memory_order_relaxed)) {
                vm_bytes_reserved = true;
                break;
            }
        }
    }
    Om_VM_Object *object = om_alloc(size);
    if (object == NULL) {
        if (vm_bytes_reserved) {
            (void)atomic_fetch_sub_explicit(&heap->vm->heap_bytes_allocated,
                                            size, memory_order_acq_rel);
        }
        heap->last_status = OM_STATUS_OUT_OF_MEMORY;
        return NULL;
    }
    (void)memset(object, 0, size);
    object->kind = kind;
    object->owner = heap;
    object->next = heap->objects;
    object->allocation_size = size;
    heap->objects = object;
    heap->bytes_allocated = new_total;
    heap->object_count += 1;
    if (heap->bytes_allocated >= heap->gc_threshold) {
        heap->gc_pending = true;
    }
    heap->last_status = OM_STATUS_OK;
    return object;
}

void om_vm_heap_destroy(Om_VM_Heap *heap)
{
    if (heap == NULL) {
        return;
    }
    if (heap->vm != NULL) {
        (void)atomic_fetch_sub_explicit(&heap->vm->heap_bytes_allocated,
                                        heap->bytes_allocated,
                                        memory_order_acq_rel);
    }
    Om_VM_Object *object = heap->objects;
    while (object != NULL) {
        Om_VM_Object *next = object->next;
        om_free(object);
        object = next;
    }
    *heap = (Om_VM_Heap){0};
}

static void mark_value(Om_VM_Heap *heap, Om_VM_Value value,
                       Om_VM_Object **gray)
{
    if (value.tag != OM_VM_VALUE_OBJECT || value.as.object == NULL
        || value.as.object->owner != heap || value.as.object->marked) {
        return;
    }
    value.as.object->marked = true;
    value.as.object->mark_next = *gray;
    *gray = value.as.object;
}

static void trace_object(Om_VM_Heap *heap, Om_VM_Object *object,
                         Om_VM_Object **gray)
{
    switch (object->kind) {
    case OM_VM_OBJECT_LIST: {
        Om_VM_List_Object *list = (Om_VM_List_Object *)object;
        for (size_t index = 0; index < list->length; ++index) {
            mark_value(heap, list->items[index], gray);
        }
        break;
    }
    case OM_VM_OBJECT_MAP: {
        Om_VM_Map_Object *map = (Om_VM_Map_Object *)object;
        for (size_t index = 0; index < map->length; ++index) {
            mark_value(heap, map->entries[index].key, gray);
            mark_value(heap, map->entries[index].value, gray);
        }
        break;
    }
    case OM_VM_OBJECT_RECORD:
    case OM_VM_OBJECT_MESSAGE: {
        Om_VM_Record_Object *record = (Om_VM_Record_Object *)object;
        for (size_t index = 0; index < record->field_count; ++index) {
            mark_value(heap, record->fields[index], gray);
        }
        break;
    }
    case OM_VM_OBJECT_RESULT:
        mark_value(heap, ((Om_VM_Result_Object *)object)->payload, gray);
        break;
    case OM_VM_OBJECT_ITERATOR:
        mark_value(heap, (Om_VM_Value){
            .tag = OM_VM_VALUE_OBJECT,
            .as.object = ((Om_VM_Iterator_Object *)object)->collection
        }, gray);
        break;
    case OM_VM_OBJECT_STRING:
    case OM_VM_OBJECT_FUNCTION:
        break;
    }
}

static void mark_roots(Om_VM_Heap *heap, Om_VM_Object **gray)
{
    Om_VM_State *vm = heap->vm;
    Om_VM_State *execution = heap->execution_state;
    if (vm == NULL || execution == NULL) {
        return;
    }
    if (heap->actor_id != 0
        && heap->actor_id <= atomic_load_explicit(&vm->actor_count,
                                                  memory_order_acquire)) {
        Om_VM_Actor_State *actor = &vm->actors[(size_t)(heap->actor_id - 1)];
        if (actor->id == heap->actor_id && actor->heap == heap) {
            for (size_t index = 0; index < actor->state_count; ++index) {
                mark_value(heap, actor->state_values[index], gray);
            }
        }
    }
    for (size_t index = 0; index < execution->stack_length; ++index) {
        mark_value(heap, execution->stack[index], gray);
    }
    for (size_t frame_index = 0; frame_index < execution->frame_count;
         ++frame_index) {
        Om_VM_Call_Frame *frame = &execution->frames[frame_index];
        if (frame->actor_id != heap->actor_id
            || frame->function_index >= execution->image->function_count) {
            continue;
        }
        const size_t local_count =
            execution->image->functions[frame->function_index].local_count;
        for (size_t local_index = 0; local_index < local_count; ++local_index) {
            mark_value(heap, frame->locals[local_index], gray);
        }
    }
    if (execution->handler_actor_id == heap->actor_id) {
        mark_value(heap, execution->handler_result, gray);
    }
}

Om_Status om_vm_heap_collect(Om_VM_Heap *heap)
{
    if (heap == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Object *gray = NULL;
    mark_roots(heap, &gray);
    while (gray != NULL) {
        Om_VM_Object *object = gray;
        gray = object->mark_next;
        object->mark_next = NULL;
        trace_object(heap, object, &gray);
    }

    Om_VM_Object **link = &heap->objects;
    size_t live_bytes = 0;
    size_t live_objects = 0;
    while (*link != NULL) {
        Om_VM_Object *object = *link;
        if (!object->marked) {
            *link = object->next;
            if (heap->bytes_allocated >= object->allocation_size) {
                heap->bytes_allocated -= object->allocation_size;
            } else {
                heap->bytes_allocated = 0;
            }
            if (heap->vm != NULL) {
                (void)atomic_fetch_sub_explicit(
                    &heap->vm->heap_bytes_allocated,
                    object->allocation_size, memory_order_acq_rel);
            }
            if (heap->object_count != 0) {
                heap->object_count -= 1;
            }
            om_free(object);
            continue;
        }
        object->marked = false;
        object->mark_next = NULL;
        live_bytes += object->allocation_size;
        live_objects += 1;
        link = &object->next;
    }
    heap->bytes_allocated = live_bytes;
    heap->object_count = live_objects;
    size_t doubled_live = 0;
    if (!om_checked_size_add(live_bytes, live_bytes, &doubled_live)) {
        doubled_live = heap->max_bytes;
    }
    if (doubled_live < heap->gc_threshold) {
        doubled_live = heap->gc_threshold;
    }
    if (doubled_live > heap->max_bytes) {
        doubled_live = heap->max_bytes;
    }
    heap->gc_threshold = doubled_live;
    heap->gc_pending = heap->bytes_allocated >= heap->gc_threshold;
    heap->last_status = OM_STATUS_OK;
    if (heap->vm != NULL) {
        (void)atomic_fetch_add_explicit(&heap->vm->gc_collection_count, 1,
                                        memory_order_relaxed);
    }
    return OM_STATUS_OK;
}

static Om_Status allocate_array_object_size(size_t base_size,
                                            size_t item_count,
                                            size_t item_size,
                                            size_t *size)
{
    size_t item_bytes = 0;
    if (size == NULL
        || !om_checked_size_mul(item_count, item_size, &item_bytes)
        || !om_checked_size_add(base_size, item_bytes, size)) {
        return OM_STATUS_OVERFLOW;
    }
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_string(Om_VM_Heap *heap, Om_String_View view,
                                Om_VM_Value *value)
{
    if (heap == NULL || value == NULL
        || (view.length != 0 && view.data == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t size = 0;
    if (!om_checked_size_add(offsetof(Om_VM_String_Object, bytes),
                             view.length, &size)
        || !om_checked_size_add(size, 1, &size)) {
        return OM_STATUS_OVERFLOW;
    }
    Om_VM_String_Object *string = (Om_VM_String_Object *)
        om_vm_heap_allocate(heap, size, OM_VM_OBJECT_STRING);
    if (string == NULL) {
        return heap->last_status;
    }
    string->length = view.length;
    if (view.length != 0) {
        (void)memcpy(string->bytes, view.data, view.length);
    }
    string->bytes[view.length] = '\0';
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &string->base};
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_list(Om_VM_Heap *heap, const Om_VM_Value *items,
                               size_t item_count, Om_VM_Value *value)
{
    if (heap == NULL || value == NULL
        || (item_count != 0 && items == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t size = 0;
    const Om_Status size_status = allocate_array_object_size(
        offsetof(Om_VM_List_Object, items), item_count, sizeof(*items), &size);
    if (size_status != OM_STATUS_OK) {
        return size_status;
    }
    Om_VM_List_Object *list = (Om_VM_List_Object *)
        om_vm_heap_allocate(heap, size, OM_VM_OBJECT_LIST);
    if (list == NULL) {
        return heap->last_status;
    }
    list->length = item_count;
    if (item_count != 0) {
        (void)memcpy(list->items, items, item_count * sizeof(*items));
    }
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &list->base};
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_map(Om_VM_Heap *heap,
                              const Om_VM_Map_Entry *entries,
                              size_t entry_count, Om_VM_Value *value)
{
    if (heap == NULL || value == NULL
        || (entry_count != 0 && entries == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t size = 0;
    const Om_Status size_status = allocate_array_object_size(
        offsetof(Om_VM_Map_Object, entries), entry_count, sizeof(*entries),
        &size);
    if (size_status != OM_STATUS_OK) {
        return size_status;
    }
    Om_VM_Map_Object *map = (Om_VM_Map_Object *)
        om_vm_heap_allocate(heap, size, OM_VM_OBJECT_MAP);
    if (map == NULL) {
        return heap->last_status;
    }
    map->capacity = entry_count;
    map->length = entry_count;
    if (entry_count != 0) {
        (void)memcpy(map->entries, entries, entry_count * sizeof(*entries));
    }
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &map->base};
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_record(Om_VM_Heap *heap,
                                 Om_VM_Object_Kind kind,
                                 uint32_t schema_index,
                                 const Om_VM_Value *fields,
                                 size_t field_count,
                                 Om_VM_Value *value)
{
    if (heap == NULL || value == NULL
        || (kind != OM_VM_OBJECT_RECORD && kind != OM_VM_OBJECT_MESSAGE)
        || (field_count != 0 && fields == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t size = 0;
    const Om_Status size_status = allocate_array_object_size(
        offsetof(Om_VM_Record_Object, fields), field_count, sizeof(*fields),
        &size);
    if (size_status != OM_STATUS_OK) {
        return size_status;
    }
    Om_VM_Record_Object *record = (Om_VM_Record_Object *)
        om_vm_heap_allocate(heap, size, kind);
    if (record == NULL) {
        return heap->last_status;
    }
    record->schema_index = schema_index;
    record->field_count = field_count;
    if (field_count != 0) {
        (void)memcpy(record->fields, fields, field_count * sizeof(*fields));
    }
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &record->base};
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_result(Om_VM_Heap *heap, bool is_ok,
                                 Om_VM_Value payload, Om_VM_Value *value)
{
    if (heap == NULL || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Result_Object *result = (Om_VM_Result_Object *)
        om_vm_heap_allocate(heap, sizeof(*result), OM_VM_OBJECT_RESULT);
    if (result == NULL) {
        return heap->last_status;
    }
    result->is_ok = is_ok;
    result->payload = payload;
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &result->base};
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_function(Om_VM_Heap *heap, uint32_t function_index,
                                   Om_VM_Value *value)
{
    if (heap == NULL || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Function_Object *function = (Om_VM_Function_Object *)
        om_vm_heap_allocate(heap, sizeof(*function), OM_VM_OBJECT_FUNCTION);
    if (function == NULL) {
        return heap->last_status;
    }
    function->function_index = function_index;
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &function->base};
    return OM_STATUS_OK;
}

Om_Status om_vm_heap_make_iterator(Om_VM_Heap *heap,
                                   Om_VM_Iterator_Kind kind,
                                   Om_VM_Object *collection,
                                   Om_VM_Value *value)
{
    if (heap == NULL || value == NULL || collection == NULL
        || (kind == OM_VM_ITERATOR_LIST
            && collection->kind != OM_VM_OBJECT_LIST)
        || (kind == OM_VM_ITERATOR_MAP
            && collection->kind != OM_VM_OBJECT_MAP)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Iterator_Object *iterator = (Om_VM_Iterator_Object *)
        om_vm_heap_allocate(heap, sizeof(*iterator), OM_VM_OBJECT_ITERATOR);
    if (iterator == NULL) {
        return heap->last_status;
    }
    iterator->kind = kind;
    iterator->collection = collection;
    *value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT,
                           .as.object = &iterator->base};
    return OM_STATUS_OK;
}
