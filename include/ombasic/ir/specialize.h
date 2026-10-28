#ifndef OMBASIC_IR_SPECIALIZE_H
#define OMBASIC_IR_SPECIALIZE_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/ir/hir.h"

typedef enum Om_Function_Instance_Kind {
    OM_FUNCTION_INSTANCE_FUNCTION = 0,
    OM_FUNCTION_INSTANCE_ACTION,
    OM_FUNCTION_INSTANCE_RECEIVE,
    OM_FUNCTION_INSTANCE_ACTOR_STATE_INIT
} Om_Function_Instance_Kind;

typedef enum Om_Specialization_Diagnostic_Code {
    OM_SPECIALIZATION_DIAGNOSTIC_E1001 = 1001,
    OM_SPECIALIZATION_DIAGNOSTIC_E1002,
    OM_SPECIALIZATION_DIAGNOSTIC_E1003
} Om_Specialization_Diagnostic_Code;

typedef struct Om_Function_Instance {
    Om_Function_Instance_Id id;
    Om_Function_Instance_Kind kind;
    Om_Hir_Entity_Id entity_id;
    Om_Module_Id module_id;
    Om_Hir_Node_Id root_node;
    Om_Symbol_Id symbol_id;
    Om_Function_Instance_Id parent_instance_id;
    uint32_t type_argument_start;
    uint32_t type_argument_count;
} Om_Function_Instance;

typedef struct Om_Specialization_Diagnostic {
    Om_Specialization_Diagnostic_Code code;
    Om_Module_Id module_id;
    Om_Hir_Node_Id node_id;
    Om_Symbol_Id symbol_id;
} Om_Specialization_Diagnostic;

typedef struct Om_Hir_Specialization {
    const Om_Function_Instance *instances;
    size_t instance_count;
    const Om_Type_Id *type_arguments;
    size_t type_argument_count;
    /* Concrete List, Map, and Result shapes reachable from specialized HIR. */
    const Om_Type_Id *descriptor_types;
    size_t descriptor_type_count;
    const Om_Specialization_Diagnostic *diagnostics;
    size_t diagnostic_count;
    /* The type store is borrowed and may receive canonical specialized types. */
    Om_Type_Store *type_store;
    void *owned_storage;
} Om_Hir_Specialization;

/*
 * Enumerate concrete reachable function instances from Actions, receives,
 * and Actor-state initializers. The mutable type store must belong to the HIR
 * project's inference result and remain alive through specialization cleanup.
 */
Om_Status om_hir_specialize_project(
    const Om_Hir_Project *project,
    Om_Type_Store *type_store,
    Om_Hir_Specialization *result
);
Om_Status om_hir_specialization_resolve_type(
    const Om_Hir_Project *project,
    const Om_Hir_Specialization *specialization,
    Om_Function_Instance_Id instance_id,
    Om_Type_Id source_type,
    Om_Type_Id *specialized_type_id
);
void om_hir_specialization_destroy(Om_Hir_Specialization *specialization);
const char *om_specialization_diagnostic_code_name(
    Om_Specialization_Diagnostic_Code code
);

#endif
