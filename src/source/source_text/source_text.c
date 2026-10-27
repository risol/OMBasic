#include "ombasic/source/source_text.h"

#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

typedef struct Om_Source_Text_Storage {
    char *path;
    uint8_t *bytes;
    uint32_t *line_starts;
} Om_Source_Text_Storage;

static void storage_destroy(Om_Source_Text_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    om_free(storage->path);
    om_free(storage->bytes);
    om_free(storage->line_starts);
    om_free(storage);
}

static uint64_t hash_bytes(Om_Const_Byte_Slice bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0; index < bytes.length; ++index) {
        hash ^= (uint64_t)bytes.data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

Om_Status om_source_text_init(
    Om_Source_Text *source,
    Om_Source_Id id,
    Om_String_View canonical_path,
    Om_Const_Byte_Slice input,
    size_t maximum_byte_length
)
{
    size_t path_storage_size = 0;
    size_t byte_storage_size = 0;
    size_t line_storage_size = 0;
    size_t line_count = 1;
    Om_Source_Text_Storage *storage = NULL;
    Om_Status status = OM_STATUS_OK;

    if (source == NULL || source->owned_storage != NULL
        || id.value == UINT32_MAX || !om_string_view_is_valid(canonical_path)
        || canonical_path.length == 0
        || (input.data == NULL && input.length != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (memchr(canonical_path.data, '\0', canonical_path.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = om_utf8_validate((Om_Const_Byte_Slice){
        (const uint8_t *)canonical_path.data,
        canonical_path.length
    });
    if (status != OM_STATUS_OK) {
        return status;
    }

    if (maximum_byte_length == 0) {
        maximum_byte_length = OM_SOURCE_TEXT_DEFAULT_MAX_BYTES;
    }
    if (input.length > maximum_byte_length || input.length >= UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    for (size_t index = 0; index < input.length; ++index) {
        if (input.data[index] == UINT8_C('\n')
            && !om_checked_size_add(line_count, 1, &line_count)) {
            return OM_STATUS_OVERFLOW;
        }
    }
    if (!om_checked_size_add(canonical_path.length, 1, &path_storage_size)
        || !om_checked_size_add(input.length, 1, &byte_storage_size)
        || !om_checked_size_mul(line_count, sizeof(uint32_t), &line_storage_size)) {
        return OM_STATUS_OVERFLOW;
    }
    if (line_count > UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }

    storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Source_Text_Storage){0};
    storage->path = om_alloc(path_storage_size);
    storage->bytes = om_alloc(byte_storage_size);
    storage->line_starts = om_alloc(line_storage_size);
    if (storage->path == NULL || storage->bytes == NULL || storage->line_starts == NULL) {
        storage_destroy(storage);
        return OM_STATUS_OUT_OF_MEMORY;
    }

    (void)memcpy(storage->path, canonical_path.data, canonical_path.length);
    storage->path[canonical_path.length] = '\0';
    if (input.length != 0) {
        (void)memcpy(storage->bytes, input.data, input.length);
    }
    storage->bytes[input.length] = 0;
    storage->line_starts[0] = 0;
    size_t line_index = 1;
    for (size_t index = 0; index < input.length; ++index) {
        if (input.data[index] == UINT8_C('\n')) {
            storage->line_starts[line_index] = (uint32_t)(index + 1);
            ++line_index;
        }
    }

    *source = (Om_Source_Text){
        .id = id,
        .canonical_path = {storage->path, canonical_path.length},
        .bytes = {storage->bytes, input.length},
        .line_starts = storage->line_starts,
        .line_count = (uint32_t)line_count,
        .content_hash = hash_bytes(input),
        .owned_storage = storage
    };
    return OM_STATUS_OK;
}

void om_source_text_destroy(Om_Source_Text *source)
{
    if (source == NULL) {
        return;
    }
    storage_destroy(source->owned_storage);
    *source = (Om_Source_Text){.id = OM_SOURCE_ID_INVALID};
}
