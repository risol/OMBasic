#include "ombasic/module_graph/module_loader.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/source/import_path.h"

typedef struct Om_Import_Declaration {
    char *path;
    size_t path_length;
    Om_Source_Span span;
} Om_Import_Declaration;

typedef enum Om_Module_Load_State {
    OM_MODULE_UNSEEN = 0,
    OM_MODULE_LOADING,
    OM_MODULE_LOADED
} Om_Module_Load_State;

typedef struct Om_Module_Source_Record {
    Om_Source_Text source;
    Om_Vector imports;
    Om_Module_Load_State state;
} Om_Module_Source_Record;

typedef struct Om_Module_Project_Storage {
    Om_Vector sources;
} Om_Module_Project_Storage;

typedef struct Om_Module_Load_Frame {
    Om_Module_Id module;
    size_t next_import;
} Om_Module_Load_Frame;

static bool is_ascii_alpha(uint8_t byte)
{
    return (byte >= (uint8_t)'a' && byte <= (uint8_t)'z')
           || (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z');
}

static bool is_identifier_continue(uint8_t byte)
{
    return is_ascii_alpha(byte) || (byte >= (uint8_t)'0' && byte <= (uint8_t)'9')
           || byte == (uint8_t)'_';
}

static bool is_horizontal_space(uint8_t byte)
{
    return byte == (uint8_t)' ' || byte == (uint8_t)'\t';
}

static bool is_line_ending(uint8_t byte)
{
    return byte == (uint8_t)'\n' || byte == (uint8_t)'\r';
}

static void set_error_span(
    const Om_Source_Text *source,
    size_t start,
    size_t end,
    Om_Source_Span *error_span
)
{
    if (error_span == NULL) {
        return;
    }
    *error_span = (Om_Source_Span){
        .source = source->id,
        .start = (uint32_t)start,
        .end = (uint32_t)end
    };
}

static Om_Status skip_line_comment(const Om_Source_Text *source, size_t *cursor)
{
    while (*cursor < source->bytes.length
           && !is_line_ending(source->bytes.data[*cursor])) {
        ++*cursor;
    }
    return OM_STATUS_OK;
}

static Om_Status skip_module_prefix_trivia(
    const Om_Source_Text *source,
    size_t *cursor,
    Om_Source_Span *error_span
)
{
    while (*cursor < source->bytes.length) {
        const uint8_t byte = source->bytes.data[*cursor];
        if (is_horizontal_space(byte)) {
            ++*cursor;
        } else if (byte == (uint8_t)'\n') {
            ++*cursor;
        } else if (byte == (uint8_t)'\r') {
            if (*cursor + 1 >= source->bytes.length
                || source->bytes.data[*cursor + 1] != (uint8_t)'\n') {
                set_error_span(source, *cursor, *cursor + 1, error_span);
                return OM_STATUS_INVALID_ARGUMENT;
            }
            *cursor += 2;
        } else if (byte == (uint8_t)'\'') {
            (void)skip_line_comment(source, cursor);
        } else {
            break;
        }
    }
    return OM_STATUS_OK;
}

static int hex_value(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return (int)(byte - (uint8_t)'0');
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return (int)(byte - (uint8_t)'a') + 10;
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return (int)(byte - (uint8_t)'A') + 10;
    }
    return -1;
}

static Om_Status append_utf8_scalar(uint32_t scalar, char *output, size_t capacity, size_t *length)
{
    uint8_t encoded[4] = {0};
    size_t encoded_length = 0;
    if (scalar <= UINT32_C(0x7f)) {
        encoded[0] = (uint8_t)scalar;
        encoded_length = 1;
    } else if (scalar <= UINT32_C(0x7ff)) {
        encoded[0] = (uint8_t)(UINT32_C(0xc0) | (scalar >> 6));
        encoded[1] = (uint8_t)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        encoded_length = 2;
    } else if (scalar <= UINT32_C(0xffff)) {
        encoded[0] = (uint8_t)(UINT32_C(0xe0) | (scalar >> 12));
        encoded[1] = (uint8_t)(UINT32_C(0x80) | ((scalar >> 6) & UINT32_C(0x3f)));
        encoded[2] = (uint8_t)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        encoded_length = 3;
    } else if (scalar <= UINT32_C(0x10ffff)) {
        encoded[0] = (uint8_t)(UINT32_C(0xf0) | (scalar >> 18));
        encoded[1] = (uint8_t)(UINT32_C(0x80) | ((scalar >> 12) & UINT32_C(0x3f)));
        encoded[2] = (uint8_t)(UINT32_C(0x80) | ((scalar >> 6) & UINT32_C(0x3f)));
        encoded[3] = (uint8_t)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        encoded_length = 4;
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if ((scalar >= UINT32_C(0xd800) && scalar <= UINT32_C(0xdfff))
        || *length > capacity || encoded_length > capacity - *length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < encoded_length; ++index) {
        ((unsigned char *)output)[*length + index] = encoded[index];
    }
    *length += encoded_length;
    return OM_STATUS_OK;
}

static Om_Status decode_import_path(
    const Om_Source_Text *source,
    size_t raw_start,
    size_t raw_end,
    char **path,
    size_t *path_length
)
{
    size_t allocation_size = 0;
    size_t output_length = 0;
    char *output = NULL;
    if (path == NULL || path_length == NULL || raw_end < raw_start
        || !om_checked_size_add(raw_end - raw_start, 1, &allocation_size)) {
        return OM_STATUS_OVERFLOW;
    }
    *path = NULL;
    *path_length = 0;
    output = om_alloc(allocation_size);
    if (output == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t cursor = raw_start;
    while (cursor < raw_end) {
        uint8_t byte = source->bytes.data[cursor++];
        if (byte != (uint8_t)'\\') {
            if (byte == (uint8_t)'\n' || byte == (uint8_t)'\r') {
                om_free(output);
                return OM_STATUS_INVALID_ARGUMENT;
            }
            output[output_length++] = (char)byte;
            continue;
        }
        if (cursor >= raw_end) {
            om_free(output);
            return OM_STATUS_INVALID_ARGUMENT;
        }
        byte = source->bytes.data[cursor++];
        switch (byte) {
        case (uint8_t)'\\':
            output[output_length++] = '\\';
            break;
        case (uint8_t)'"':
            output[output_length++] = '"';
            break;
        case (uint8_t)'n':
            output[output_length++] = '\n';
            break;
        case (uint8_t)'r':
            output[output_length++] = '\r';
            break;
        case (uint8_t)'t':
            output[output_length++] = '\t';
            break;
        case (uint8_t)'0':
            output[output_length++] = '\0';
            break;
        case (uint8_t)'u': {
            if (cursor >= raw_end || source->bytes.data[cursor++] != (uint8_t)'{') {
                om_free(output);
                return OM_STATUS_INVALID_ARGUMENT;
            }
            uint32_t scalar = 0;
            size_t digits = 0;
            while (cursor < raw_end && source->bytes.data[cursor] != (uint8_t)'}') {
                const int digit = hex_value(source->bytes.data[cursor]);
                if (digit < 0 || digits == 6
                    || scalar > (UINT32_C(0x10ffff) - (uint32_t)digit) / UINT32_C(16)) {
                    om_free(output);
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                scalar = scalar * UINT32_C(16) + (uint32_t)digit;
                ++digits;
                ++cursor;
            }
            if (cursor >= raw_end || source->bytes.data[cursor++] != (uint8_t)'}'
                || digits == 0
                || append_utf8_scalar(scalar, output, raw_end - raw_start, &output_length)
                       != OM_STATUS_OK) {
                om_free(output);
                return OM_STATUS_INVALID_ARGUMENT;
            }
            break;
        }
        default:
            om_free(output);
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    output[output_length] = '\0';
    *path = output;
    *path_length = output_length;
    return OM_STATUS_OK;
}

static Om_Status append_import(
    const Om_Source_Text *source,
    Om_Vector *imports,
    size_t statement_start,
    size_t *cursor,
    Om_Source_Span *error_span
)
{
    const size_t length = source->bytes.length;
    const size_t token_end = *cursor;
    while (*cursor < length && is_horizontal_space(source->bytes.data[*cursor])) {
        ++*cursor;
    }
    if (*cursor >= length || source->bytes.data[*cursor] != (uint8_t)'"') {
        set_error_span(source, statement_start, *cursor < length ? *cursor + 1 : length, error_span);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t quoted_start = *cursor;
    const size_t raw_start = quoted_start + 1;
    size_t raw_end = raw_start;
    while (raw_end < length) {
        const uint8_t byte = source->bytes.data[raw_end];
        if (byte == (uint8_t)'"') {
            break;
        }
        if (byte == (uint8_t)'\n' || byte == (uint8_t)'\r') {
            set_error_span(source, quoted_start, raw_end + 1, error_span);
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (byte == (uint8_t)'\\') {
            if (raw_end + 1 >= length) {
                break;
            }
            raw_end += 2;
        } else {
            ++raw_end;
        }
    }
    if (raw_end >= length || source->bytes.data[raw_end] != (uint8_t)'"') {
        set_error_span(source, quoted_start, length, error_span);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    char *path = NULL;
    size_t path_length = 0;
    Om_Status status = decode_import_path(source, raw_start, raw_end, &path, &path_length);
    if (status != OM_STATUS_OK) {
        set_error_span(source, quoted_start, raw_end + 1, error_span);
        return status;
    }
    status = om_import_path_validate((Om_String_View){path, path_length});
    if (status != OM_STATUS_OK) {
        om_free(path);
        set_error_span(source, quoted_start, raw_end + 1, error_span);
        return status;
    }
    *cursor = raw_end + 1;
    while (*cursor < length && is_horizontal_space(source->bytes.data[*cursor])) {
        ++*cursor;
    }
    if (*cursor < length && source->bytes.data[*cursor] == (uint8_t)'\'') {
        (void)skip_line_comment(source, cursor);
    }
    if (*cursor < length && source->bytes.data[*cursor] != (uint8_t)'\n'
        && !(source->bytes.data[*cursor] == (uint8_t)'\r' && *cursor + 1 < length
             && source->bytes.data[*cursor + 1] == (uint8_t)'\n')) {
        om_free(path);
        set_error_span(source, *cursor, *cursor + 1, error_span);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Import_Declaration declaration = {
        .path = path,
        .path_length = path_length,
        .span = {
            .source = source->id,
            .start = (uint32_t)quoted_start,
            .end = (uint32_t)(raw_end + 1)
        }
    };
    status = om_vector_push(imports, &declaration);
    if (status != OM_STATUS_OK) {
        om_free(path);
        set_error_span(source, statement_start, token_end, error_span);
        return status;
    }
    return OM_STATUS_OK;
}

static Om_Status extract_imports(
    const Om_Source_Text *source,
    Om_Vector *imports,
    Om_Source_Span *error_span
)
{
    size_t cursor = 0;
    Om_Status status = OM_STATUS_OK;
    if (source == NULL || imports == NULL || imports->item_size != sizeof(Om_Import_Declaration)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (error_span != NULL) {
        *error_span = (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0};
    }
    while (cursor < source->bytes.length) {
        size_t token_start = 0;
        status = skip_module_prefix_trivia(source, &cursor, error_span);
        if (status != OM_STATUS_OK || cursor >= source->bytes.length) {
            return status;
        }
        token_start = cursor;
        if (!is_ascii_alpha(source->bytes.data[cursor])
            && source->bytes.data[cursor] != (uint8_t)'_') {
            return OM_STATUS_OK;
        }
        while (cursor < source->bytes.length
               && is_identifier_continue(source->bytes.data[cursor])) {
            ++cursor;
        }
        if (cursor - token_start != 6
            || memcmp(source->bytes.data + token_start, "IMPORT", 6) != 0) {
            return OM_STATUS_OK;
        }
        status = append_import(source, imports, token_start, &cursor, error_span);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static void module_source_record_destroy(Om_Module_Source_Record *record)
{
    if (record == NULL) {
        return;
    }
    for (size_t index = 0; index < record->imports.length; ++index) {
        Om_Import_Declaration *declaration = om_vector_at(&record->imports, index);
        om_free(declaration->path);
    }
    om_vector_destroy(&record->imports);
    om_source_text_destroy(&record->source);
    *record = (Om_Module_Source_Record){0};
}

static void project_storage_destroy(Om_Module_Project_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    for (size_t index = 0; index < storage->sources.length; ++index) {
        module_source_record_destroy(om_vector_at(&storage->sources, index));
    }
    om_vector_destroy(&storage->sources);
    om_free(storage);
}

void om_module_project_destroy(Om_Module_Project *project)
{
    if (project == NULL) {
        return;
    }
    om_module_graph_order_destroy(&project->order);
    om_module_graph_destroy(&project->graph);
    project_storage_destroy(project->owned_storage);
    *project = (Om_Module_Project){0};
    project->failed_module = OM_MODULE_ID_INVALID;
    project->error_span.source = OM_SOURCE_ID_INVALID;
}

const Om_Source_Text *om_module_project_source(
    const Om_Module_Project *project,
    Om_Module_Id module_id
)
{
    if (project == NULL || project->owned_storage == NULL
        || module_id.value == UINT32_MAX) {
        return NULL;
    }
    const Om_Module_Project_Storage *storage = project->owned_storage;
    const Om_Module_Source_Record *record = om_vector_at_const(&storage->sources, module_id.value);
    if (record == NULL || record->source.owned_storage == NULL) {
        return NULL;
    }
    return &record->source;
}

size_t om_module_project_module_count(const Om_Module_Project *project)
{
    if (project == NULL || project->owned_storage == NULL) {
        return 0;
    }
    return project->graph.modules.length;
}

size_t om_module_project_import_count(
    const Om_Module_Project *project,
    Om_Module_Id importer
)
{
    if (project == NULL || project->owned_storage == NULL
        || importer.value == UINT32_MAX) {
        return 0;
    }
    const Om_Module_Project_Storage *storage = project->owned_storage;
    const Om_Module_Source_Record *record = om_vector_at_const(
        &storage->sources, importer.value);
    return record == NULL ? 0 : record->imports.length;
}

Om_Status om_module_project_import_at(
    const Om_Module_Project *project,
    Om_Module_Id importer,
    size_t index,
    Om_Module_Import_Info *import_info
)
{
    if (project == NULL || project->owned_storage == NULL || import_info == NULL
        || importer.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *import_info = (Om_Module_Import_Info){
        .target_module = OM_MODULE_ID_INVALID
    };
    const Om_Module_Project_Storage *storage = project->owned_storage;
    const Om_Module_Source_Record *record = om_vector_at_const(
        &storage->sources, importer.value);
    if (record == NULL || index >= record->imports.length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Import_Declaration *declaration = om_vector_at_const(
        &record->imports, index);
    const Om_String_View path = {declaration->path, declaration->path_length};
    for (size_t module_index = 0; module_index < project->graph.modules.length;
         ++module_index) {
        const Om_Module_Id candidate = {(uint32_t)module_index};
        if (om_string_view_equal(om_module_graph_path(&project->graph, candidate), path)) {
            import_info->target_module = candidate;
            import_info->path = path;
            import_info->span = declaration->span;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

static Om_Status ensure_source_record(
    Om_Module_Project_Storage *storage,
    Om_Module_Id module,
    bool *is_new
)
{
    Om_Module_Source_Record record = {0};
    Om_Status status = OM_STATUS_OK;
    if (storage == NULL || is_new == NULL || module.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *is_new = false;
    if ((size_t)module.value < storage->sources.length) {
        return OM_STATUS_OK;
    }
    if ((size_t)module.value != storage->sources.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = om_vector_init(&record.imports, sizeof(Om_Import_Declaration));
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = om_vector_push(&storage->sources, &record);
    if (status != OM_STATUS_OK) {
        module_source_record_destroy(&record);
        return status;
    }
    *is_new = true;
    return OM_STATUS_OK;
}

static Om_Status module_project_load(
    const Om_Source_Root *source_root,
    Om_String_View entry_path,
    Om_Const_Byte_Slice entry_source,
    bool use_entry_source,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
)
{
    Om_Module_Project_Storage *storage = NULL;
    Om_Vector frames = {0};
    Om_Module_Id entry_module = OM_MODULE_ID_INVALID;
    Om_Status status = OM_STATUS_OK;
    bool is_new = false;
    if (project == NULL || source_root == NULL || source_root->owned_storage == NULL
        || project->owned_storage != NULL || project->graph.modules.data != NULL
        || project->order.items != NULL || project->order.cycle != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    project->failed_module = OM_MODULE_ID_INVALID;
    project->error_span.source = OM_SOURCE_ID_INVALID;
    status = om_module_graph_init(&project->graph);
    if (status != OM_STATUS_OK) {
        return status;
    }
    storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Module_Project_Storage){0};
    status = om_vector_init(&storage->sources, sizeof(Om_Module_Source_Record));
    if (status != OM_STATUS_OK) {
        om_free(storage);
        return status;
    }
    project->owned_storage = storage;
    status = om_vector_init(&frames, sizeof(Om_Module_Load_Frame));
    if (status != OM_STATUS_OK) {
        goto cleanup;
    }
    status = om_module_graph_add(&project->graph, entry_path, &entry_module);
    if (status != OM_STATUS_OK) {
        goto cleanup;
    }
    status = ensure_source_record(storage, entry_module, &is_new);
    if (status != OM_STATUS_OK || !is_new) {
        if (status == OM_STATUS_OK) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        goto cleanup;
    }
    Om_Module_Source_Record *entry_record = om_vector_at(&storage->sources, entry_module.value);
    const Om_String_View entry_canonical_path = om_module_graph_path(&project->graph, entry_module);
    if (use_entry_source) {
        status = om_source_text_init(
            &entry_record->source,
            (Om_Source_Id){entry_module.value},
            entry_canonical_path,
            entry_source,
            maximum_source_byte_length
        );
    } else {
        status = om_source_root_load(
            source_root,
            (Om_Source_Id){entry_module.value},
            entry_canonical_path,
            maximum_source_byte_length,
            &entry_record->source
        );
    }
    if (status != OM_STATUS_OK) {
        project->failed_module = entry_module;
        goto cleanup;
    }
    status = extract_imports(&entry_record->source, &entry_record->imports, &project->error_span);
    if (status != OM_STATUS_OK) {
        project->failed_module = entry_module;
        goto cleanup;
    }
    entry_record->state = OM_MODULE_LOADING;
    status = om_vector_push(&frames, &(Om_Module_Load_Frame){entry_module, 0});
    if (status != OM_STATUS_OK) {
        goto cleanup;
    }

    while (frames.length != 0) {
        Om_Module_Load_Frame *frame = om_vector_at(&frames, frames.length - 1);
        Om_Module_Source_Record *record = om_vector_at(&storage->sources, frame->module.value);
        if (record == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        if (frame->next_import >= record->imports.length) {
            record->state = OM_MODULE_LOADED;
            --frames.length;
            continue;
        }
        const Om_Import_Declaration import = *(
            const Om_Import_Declaration *)om_vector_at_const(&record->imports, frame->next_import);
        const Om_Module_Id importer = frame->module;
        ++frame->next_import;

        Om_Module_Id imported = OM_MODULE_ID_INVALID;
        status = om_module_graph_add(
            &project->graph,
            (Om_String_View){import.path, import.path_length},
            &imported
        );
        if (status != OM_STATUS_OK) {
            project->failed_module = importer;
            project->error_span = import.span;
            goto cleanup;
        }
        status = ensure_source_record(storage, imported, &is_new);
        if (status != OM_STATUS_OK) {
            project->failed_module = importer;
            project->error_span = import.span;
            goto cleanup;
        }
        status = om_module_graph_add_import(&project->graph, importer, imported);
        if (status != OM_STATUS_OK) {
            project->failed_module = importer;
            project->error_span = import.span;
            goto cleanup;
        }

        Om_Module_Source_Record *imported_record = om_vector_at(
            &storage->sources,
            imported.value
        );
        if (imported_record == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        if (imported_record->state == OM_MODULE_UNSEEN) {
            const Om_String_View imported_path = om_module_graph_path(&project->graph, imported);
            status = om_source_root_load(
                source_root,
                (Om_Source_Id){imported.value},
                imported_path,
                maximum_source_byte_length,
                &imported_record->source
            );
            if (status != OM_STATUS_OK) {
                project->failed_module = importer;
                project->error_span = import.span;
                goto cleanup;
            }
            Om_Source_Span import_syntax_error = {OM_SOURCE_ID_INVALID, 0, 0};
            status = extract_imports(
                &imported_record->source,
                &imported_record->imports,
                &import_syntax_error
            );
            if (status != OM_STATUS_OK) {
                project->failed_module = imported;
                project->error_span = import_syntax_error;
                goto cleanup;
            }
            imported_record->state = OM_MODULE_LOADING;
            status = om_vector_push(&frames, &(Om_Module_Load_Frame){imported, 0});
            if (status != OM_STATUS_OK) {
                project->failed_module = importer;
                project->error_span = import.span;
                goto cleanup;
            }
        }
    }

    status = om_module_graph_order(&project->graph, &project->order);

cleanup:
    om_vector_destroy(&frames);
    return status;
}

Om_Status om_module_project_load(
    const Om_Source_Root *source_root,
    Om_String_View entry_path,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
)
{
    return module_project_load(source_root, entry_path, (Om_Const_Byte_Slice){0},
                               false, maximum_source_byte_length, project);
}

Om_Status om_module_project_load_with_entry_source(
    const Om_Source_Root *source_root,
    Om_String_View entry_path,
    Om_Const_Byte_Slice entry_source,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
)
{
    return module_project_load(source_root, entry_path, entry_source, true,
                               maximum_source_byte_length, project);
}
