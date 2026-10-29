#include "ombasic/tooling/interface_cache.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_Interface_Cache_Storage {
    Om_Module_Interface *modules;
    Om_Interface_Symbol *symbols;
    Om_Actor_Protocol_Entry *protocol;
    size_t symbol_capacity;
    size_t protocol_capacity;
} Om_Interface_Cache_Storage;

typedef struct Om_Type_Hash_Context {
    const Om_Type_Store *store;
    const Om_Resolution_Result *resolution;
    const Om_Type_Scheme *scheme;
    Om_Type_Id variables[256];
    size_t variable_count;
} Om_Type_Hash_Context;

static uint64_t hash_byte(uint64_t hash, uint8_t byte)
{
    return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash = hash_byte(hash, (uint8_t)(value >> shift));
    }
    return hash;
}

static uint64_t hash_view(uint64_t hash, Om_String_View view)
{
    hash = hash_u32(hash, view.length > UINT32_MAX ? UINT32_MAX : (uint32_t)view.length);
    for (size_t index = 0; index < view.length; ++index) {
        hash = hash_byte(hash, (uint8_t)view.data[index]);
    }
    return hash;
}

static const Om_Symbol *symbol_by_id(const Om_Resolution_Result *resolution,
                                     Om_Symbol_Id id)
{
    return om_resolution_symbol_by_id(resolution, id);
}

static bool scheme_variable_index(const Om_Type_Scheme *scheme,
                                  Om_Type_Id variable, uint32_t *index)
{
    if (scheme == NULL || index == NULL) {
        return false;
    }
    for (size_t current = 0; current < scheme->quantified_variable_count; ++current) {
        if (scheme->quantified_variables[current].value == variable.value) {
            *index = current > UINT32_MAX ? UINT32_MAX : (uint32_t)current;
            return true;
        }
    }
    return false;
}

static Om_Status hash_type(Om_Type_Hash_Context *context, Om_Type_Id type,
                           size_t depth, uint64_t *hash)
{
    if (depth >= 256 || context == NULL || hash == NULL) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = om_type_store_resolve(context->store, type, &resolved);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(context->store, resolved, &view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    *hash = hash_byte(*hash, (uint8_t)view.kind);
    if (view.kind == OM_TYPE_VARIABLE) {
        uint32_t variable_index = 0;
        if (scheme_variable_index(context->scheme, resolved, &variable_index)) {
            *hash = hash_byte(*hash, UINT8_C(1));
            *hash = hash_u32(*hash, variable_index);
            const uint32_t constraints = context->scheme->quantified_constraints == NULL
                ? 0 : context->scheme->quantified_constraints[variable_index];
            *hash = hash_u32(*hash, constraints);
            return OM_STATUS_OK;
        }
        for (size_t index = 0; index < context->variable_count; ++index) {
            if (context->variables[index].value == resolved.value) {
                *hash = hash_byte(*hash, UINT8_C(1));
                *hash = hash_u32(*hash, (uint32_t)index);
                *hash = hash_u32(*hash, view.constraints);
                return OM_STATUS_OK;
            }
        }
        if (context->variable_count >= sizeof(context->variables)
                                          / sizeof(context->variables[0])) {
            return OM_STATUS_OVERFLOW;
        }
        const size_t index = context->variable_count++;
        context->variables[index] = resolved;
        *hash = hash_byte(*hash, UINT8_C(1));
        *hash = hash_u32(*hash, (uint32_t)index);
        *hash = hash_u32(*hash, view.constraints);
        return OM_STATUS_OK;
    }
    if (view.nominal_symbol.value != UINT32_MAX) {
        const Om_Symbol *symbol = symbol_by_id(context->resolution,
                                               view.nominal_symbol);
        if (symbol == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *hash = hash_view(*hash, symbol->name);
    }
    *hash = hash_u32(*hash, view.child_count > UINT32_MAX
        ? UINT32_MAX : (uint32_t)view.child_count);
    for (size_t index = 0; index < view.child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(context->store, resolved, index, &child);
        if (status == OM_STATUS_OK) {
            status = hash_type(context, child, depth + 1, hash);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static uint32_t effect_for(const Om_Semantic_Analysis_Result *semantic,
                           Om_Symbol_Id symbol)
{
    for (size_t index = 0; semantic != NULL && index < semantic->symbol_effect_count;
         ++index) {
        if (semantic->symbol_effects[index].symbol_id.value == symbol.value) {
            return semantic->symbol_effects[index].effects;
        }
    }
    return 0;
}

static bool symbol_is_exported(const Om_Resolution_Result *resolution,
                               Om_Symbol_Id symbol_id)
{
    const Om_Symbol *symbol = symbol_by_id(resolution, symbol_id);
    return symbol != NULL && symbol->exported;
}

static Om_Status interface_hash(const Om_Module_Interface *module,
                                const Om_Resolution_Result *resolution,
                                const Om_Type_Inference_Result *inference,
                                uint64_t *out)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    Om_Status status = OM_STATUS_OK;
    hash = hash_view(hash, module->path);
    for (size_t index = 0; status == OM_STATUS_OK && index < module->symbol_count;
         ++index) {
        const Om_Interface_Symbol *item = &module->symbols[index];
        hash = hash_view(hash, item->name);
        hash = hash_byte(hash, (uint8_t)item->kind);
        hash = hash_u32(hash, item->effects);
        if (item->type_scheme != NULL) {
            Om_Type_Hash_Context context = {
                .store = &inference->type_store,
                .resolution = resolution,
                .scheme = item->type_scheme
            };
            status = hash_type(&context, item->type_scheme->type_id, 0, &hash);
        } else {
            hash = hash_byte(hash, UINT8_C(0xff));
        }
        hash = hash_byte(hash, item->has_serializability ? UINT8_C(1) : UINT8_C(0));
        if (item->has_serializability) {
            hash = hash_byte(hash, item->serializable ? UINT8_C(1) : UINT8_C(0));
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < module->actor_protocol_count; ++index) {
        const Om_Actor_Protocol_Entry *entry = &module->actor_protocol[index];
        const Om_Symbol *actor = symbol_by_id(resolution, entry->actor_symbol);
        const Om_Symbol *message = symbol_by_id(resolution, entry->message_symbol);
        if (actor == NULL || message == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        hash = hash_view(hash, actor->name);
        hash = hash_view(hash, message->name);
    }
    if (status == OM_STATUS_OK) {
        *out = hash;
    }
    return status;
}

Om_Status om_interface_cache_build(
    const Om_Module_Project *project,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    Om_Interface_Cache *cache
)
{
    Om_Interface_Cache_Storage *storage = NULL;
    size_t module_count = 0;
    Om_Status status = OM_STATUS_OK;
    if (project == NULL || resolution == NULL || inference == NULL
        || semantic == NULL || cache == NULL || cache->owned_storage != NULL
        || cache->modules != NULL || inference->type_store.owned_storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    module_count = om_module_project_module_count(project);
    if (module_count == 0 || module_count > SIZE_MAX / sizeof(*storage->modules)
        || resolution->export_count > SIZE_MAX / sizeof(*storage->symbols)
        || semantic->actor_protocol_count > SIZE_MAX / sizeof(*storage->protocol)) {
        return OM_STATUS_OVERFLOW;
    }
    storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Interface_Cache_Storage){0};
    storage->modules = om_alloc_array(module_count, sizeof(*storage->modules));
    storage->symbol_capacity = resolution->export_count;
    storage->protocol_capacity = semantic->actor_protocol_count;
    storage->symbols = om_alloc_array(storage->symbol_capacity,
                                      sizeof(*storage->symbols));
    storage->protocol = om_alloc_array(storage->protocol_capacity,
                                       sizeof(*storage->protocol));
    if (storage->modules == NULL
        || (storage->symbol_capacity != 0 && storage->symbols == NULL)
        || (storage->protocol_capacity != 0 && storage->protocol == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    (void)memset(storage->modules, 0, module_count * sizeof(*storage->modules));
    for (size_t module_index = 0; status == OM_STATUS_OK
         && module_index < module_count; ++module_index) {
        const Om_Module_Id module_id = {(uint32_t)module_index};
        const Om_Source_Text *source = om_module_project_source(project, module_id);
        Om_Module_Interface *module = &storage->modules[module_index];
        if (source == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        module->module_id = module_id;
        module->path = om_module_graph_path(&project->graph, module_id);
        module->source_hash = source->content_hash;
        Om_Interface_Symbol *first_symbol = storage->symbols == NULL
            ? NULL : &storage->symbols[0];
        size_t symbol_start = 0;
        if (first_symbol != NULL) {
            if (module_index != 0) {
                const ptrdiff_t prior_offset = storage->modules[module_index - 1]
                    .symbols - storage->symbols;
                if (prior_offset < 0) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                symbol_start = (size_t)prior_offset
                               + storage->modules[module_index - 1].symbol_count;
            }
        }
        module->symbols = first_symbol == NULL ? NULL : first_symbol + symbol_start;
        Om_Interface_Symbol *module_symbols = (Om_Interface_Symbol *)module->symbols;
        for (size_t symbol_index = 0;
             symbol_index < om_resolution_module_export_count(resolution, module_id);
             ++symbol_index) {
            const Om_Module_Export *exported = om_resolution_module_export_at(
                resolution, module_id, symbol_index);
            const Om_Symbol *symbol = exported != NULL
                ? symbol_by_id(resolution, exported->symbol_id) : NULL;
            if (symbol == NULL) { status = OM_STATUS_INVALID_ARGUMENT; break; }
            if (symbol_start + module->symbol_count >= storage->symbol_capacity) {
                status = OM_STATUS_OVERFLOW;
                break;
            }
            Om_Interface_Symbol *entry = &module_symbols[module->symbol_count++];
            entry->symbol_id = symbol->id;
            entry->name = exported->name;
            entry->kind = symbol->kind;
            entry->declaration_span = symbol->span;
            entry->type_scheme = om_type_inference_scheme_for_symbol(inference,
                                                                      symbol->id);
            entry->effects = effect_for(semantic, symbol->id);
            if (entry->type_scheme != NULL) {
                bool serializable = false;
                const Om_Status serialization_status = om_semantic_type_is_serializable(
                    semantic, entry->type_scheme->type_id, &serializable);
                entry->has_serializability = serialization_status == OM_STATUS_OK;
                entry->serializable = entry->has_serializability && serializable;
            }
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        size_t protocol_start = 0;
        for (size_t prior = 0; prior < module_index; ++prior) {
            protocol_start += storage->modules[prior].actor_protocol_count;
        }
        module->actor_protocol = storage->protocol == NULL
            ? NULL : &storage->protocol[protocol_start];
        for (size_t protocol_index = 0;
             protocol_index < semantic->actor_protocol_count; ++protocol_index) {
            const Om_Actor_Protocol_Entry *entry = &semantic->actor_protocol[protocol_index];
            const Om_Symbol *actor = symbol_by_id(resolution, entry->actor_symbol);
            const Om_Symbol *message = symbol_by_id(resolution, entry->message_symbol);
            if (actor == NULL || message == NULL
                || actor->module_id.value != module_id.value
                || message->module_id.value != module_id.value
                || !symbol_is_exported(resolution, entry->actor_symbol)
                || !symbol_is_exported(resolution, entry->message_symbol)) {
                continue;
            }
            if (protocol_start + module->actor_protocol_count
                >= storage->protocol_capacity) {
                status = OM_STATUS_OVERFLOW;
                break;
            }
            storage->protocol[protocol_start + module->actor_protocol_count++] = *entry;
        }
        if (status == OM_STATUS_OK) {
            status = interface_hash(module, resolution, inference,
                                    &module->interface_hash);
        }
    }
    if (status != OM_STATUS_OK) {
        goto cleanup;
    }
    cache->modules = storage->modules;
    cache->module_count = module_count;
    cache->type_store = &inference->type_store;
    cache->owned_storage = storage;
    return OM_STATUS_OK;

cleanup:
    if (storage != NULL) {
        om_free(storage->protocol);
        om_free(storage->symbols);
        om_free(storage->modules);
        om_free(storage);
    }
    return status;
}

void om_interface_cache_destroy(Om_Interface_Cache *cache)
{
    if (cache == NULL) {
        return;
    }
    Om_Interface_Cache_Storage *storage = cache->owned_storage;
    if (storage != NULL) {
        om_free(storage->protocol);
        om_free(storage->symbols);
        om_free(storage->modules);
        om_free(storage);
    }
    *cache = (Om_Interface_Cache){0};
}

const Om_Module_Interface *om_interface_cache_module(
    const Om_Interface_Cache *cache,
    Om_Module_Id module_id
)
{
    if (cache == NULL || cache->modules == NULL
        || module_id.value >= cache->module_count) {
        return NULL;
    }
    return &cache->modules[module_id.value];
}
