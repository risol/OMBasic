#ifndef OMBASIC_SEMANTIC_TYPE_STORE_H
#define OMBASIC_SEMANTIC_TYPE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/status.h"

typedef enum Om_Type_Kind {
    OM_TYPE_INT = 0,
    OM_TYPE_FLOAT,
    OM_TYPE_BOOL,
    OM_TYPE_STRING,
    OM_TYPE_NULL,
    OM_TYPE_UNIT,
    OM_TYPE_NODE_REF,
    OM_TYPE_ACTOR_ID,
    OM_TYPE_DOWN_REASON,
    OM_TYPE_NATIVE_POINTER,
    OM_TYPE_LIST,
    OM_TYPE_MAP,
    OM_TYPE_RECORD,
    OM_TYPE_MESSAGE,
    OM_TYPE_ACTOR_PROTOCOL,
    OM_TYPE_ACTOR_REF,
    OM_TYPE_FUNCTION,
    OM_TYPE_ACTION,
    OM_TYPE_RESULT,
    OM_TYPE_VARIABLE
} Om_Type_Kind;

typedef enum Om_Type_Constraint {
    OM_TYPE_CONSTRAINT_NONE = 0,
    OM_TYPE_CONSTRAINT_NUMERIC = 1u << 0,
    OM_TYPE_CONSTRAINT_INTEGRAL = 1u << 1,
    OM_TYPE_CONSTRAINT_COMPARABLE = 1u << 2,
    OM_TYPE_CONSTRAINT_ORDERED = 1u << 3,
    OM_TYPE_CONSTRAINT_PRINTABLE = 1u << 4,
    OM_TYPE_CONSTRAINT_HASH_KEY = 1u << 5,
    OM_TYPE_CONSTRAINT_ACTOR_REF = 1u << 6,
    OM_TYPE_CONSTRAINT_MESSAGE = 1u << 7,
    OM_TYPE_CONSTRAINT_ADDITIVE = 1u << 8
} Om_Type_Constraint;

#define OM_TYPE_PRIMITIVE_COUNT 10u

/* Pass a zero-initialized store to init. Treat an initialized store as move-only. */
typedef struct Om_Type_Store {
    Om_Type_Id primitive_types[OM_TYPE_PRIMITIVE_COUNT];
    void *owned_storage;
} Om_Type_Store;

/* Function/Action children are parameters then return; Map is key/value;
 * Result is value/error; List and ActorRef each have one child. */
typedef struct Om_Type_View {
    Om_Type_Kind kind;
    size_t child_count;
    Om_Symbol_Id nominal_symbol;
    uint32_t type_variable_level;
    uint32_t constraints;
    bool type_variable_unbound;
} Om_Type_View;

typedef struct Om_Type_Scheme {
    /* type_id belongs to the source Type_Store, which must outlive the scheme. */
    Om_Type_Id type_id;
    const Om_Type_Id *quantified_variables;
    const uint32_t *quantified_constraints;
    size_t quantified_variable_count;
    void *owned_quantified_variables;
    void *owned_quantified_constraints;
} Om_Type_Scheme;

Om_Status om_type_store_init(Om_Type_Store *store);
void om_type_store_destroy(Om_Type_Store *store);
Om_Status om_type_store_primitive(
    const Om_Type_Store *store,
    Om_Type_Kind kind,
    Om_Type_Id *type_id
);
Om_Status om_type_store_variable(
    Om_Type_Store *store,
    uint32_t level,
    Om_Type_Id *type_id
);
Om_Status om_type_store_require(
    Om_Type_Store *store,
    Om_Type_Id type_id,
    uint32_t constraints,
    bool *satisfied
);
Om_Status om_type_store_list(
    Om_Type_Store *store,
    Om_Type_Id element_type,
    Om_Type_Id *type_id
);
Om_Status om_type_store_map(
    Om_Type_Store *store,
    Om_Type_Id key_type,
    Om_Type_Id value_type,
    Om_Type_Id *type_id
);
Om_Status om_type_store_nominal(
    Om_Type_Store *store,
    Om_Type_Kind kind,
    Om_Symbol_Id symbol_id,
    Om_Type_Id *type_id
);
Om_Status om_type_store_actor_ref(
    Om_Type_Store *store,
    Om_Symbol_Id actor_symbol,
    Om_Type_Id *type_id
);
/* Also permits an unresolved protocol variable during inference. */
Om_Status om_type_store_actor_ref_type(
    Om_Type_Store *store,
    Om_Type_Id protocol_type,
    Om_Type_Id *type_id
);
Om_Status om_type_store_function(
    Om_Type_Store *store,
    Om_Type_Kind kind,
    const Om_Type_Id *parameter_types,
    size_t parameter_count,
    Om_Type_Id return_type,
    Om_Type_Id *type_id
);
Om_Status om_type_store_result(
    Om_Type_Store *store,
    Om_Type_Id value_type,
    Om_Type_Id error_type,
    Om_Type_Id *type_id
);

/* Resolves variable instances without changing the type graph. */
Om_Status om_type_store_resolve(
    const Om_Type_Store *store,
    Om_Type_Id type_id,
    Om_Type_Id *resolved_type
);
Om_Status om_type_store_get(
    const Om_Type_Store *store,
    Om_Type_Id type_id,
    Om_Type_View *view
);
Om_Status om_type_store_child_at(
    const Om_Type_Store *store,
    Om_Type_Id type_id,
    size_t index,
    Om_Type_Id *child_type
);

/* A mismatch is reported as OM_STATUS_OK with unified=false. */
Om_Status om_type_store_unify(
    Om_Type_Store *store,
    Om_Type_Id left,
    Om_Type_Id right,
    bool *unified
);
Om_Status om_type_store_generalize(
    const Om_Type_Store *store,
    Om_Type_Id type_id,
    uint32_t environment_level,
    Om_Type_Scheme *scheme
);
Om_Status om_type_store_instantiate(
    Om_Type_Store *store,
    const Om_Type_Scheme *scheme,
    uint32_t level,
    Om_Type_Id *type_id
);
void om_type_scheme_destroy(Om_Type_Scheme *scheme);

const char *om_type_kind_name(Om_Type_Kind kind);

#endif
