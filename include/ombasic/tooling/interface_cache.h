#ifndef OMBASIC_TOOLING_INTERFACE_CACHE_H
#define OMBASIC_TOOLING_INTERFACE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"

typedef struct Om_Interface_Symbol {
    Om_Symbol_Id symbol_id;
    /* Borrowed from the resolution result. */
    Om_String_View name;
    Om_Symbol_Kind kind;
    Om_Source_Span declaration_span;
    const Om_Type_Scheme *type_scheme;
    uint32_t effects;
    bool has_serializability;
    bool serializable;
} Om_Interface_Symbol;

typedef struct Om_Module_Interface {
    Om_Module_Id module_id;
    /* Borrowed from the module project. */
    Om_String_View path;
    uint64_t source_hash;
    uint64_t interface_hash;
    const Om_Interface_Symbol *symbols;
    size_t symbol_count;
    const Om_Actor_Protocol_Entry *actor_protocol;
    size_t actor_protocol_count;
} Om_Module_Interface;

typedef struct Om_Interface_Cache {
    const Om_Module_Interface *modules;
    size_t module_count;
    /* Destroy the cache before its source project, resolution, and inference.
       Type IDs/schemes borrow the inference store; names and paths borrow the
       resolution and project respectively. */
    const Om_Type_Store *type_store;
    void *owned_storage;
} Om_Interface_Cache;

/* Builds an in-memory view of exported declarations and Actor protocol metadata. */
Om_Status om_interface_cache_build(
    const Om_Module_Project *project,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    Om_Interface_Cache *cache
);
void om_interface_cache_destroy(Om_Interface_Cache *cache);
const Om_Module_Interface *om_interface_cache_module(
    const Om_Interface_Cache *cache,
    Om_Module_Id module_id
);

#endif
