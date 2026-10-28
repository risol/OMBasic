#include <stddef.h>
#include <stdint.h>

#include "ombasic/ir/hir.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_store.h"

static uint8_t byte_at(const uint8_t *data, size_t size, size_t index)
{
    return index < size ? data[index] : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    enum { MAX_NODES = 16 };
    static const char canonical_path[] = "fuzz.bas";
    static const Om_Hir_Kind leaf_kinds[] = {
        OM_HIR_INTEGER,
        OM_HIR_FLOAT,
        OM_HIR_STRING,
        OM_HIR_STRING_TEXT,
        OM_HIR_BOOLEAN,
        OM_HIR_NULL,
        OM_HIR_SELF,
        OM_HIR_NOMINAL_VALUE,
    };
    const Om_Type_Kind leaf_types[] = {
        OM_TYPE_INT,
        OM_TYPE_FLOAT,
        OM_TYPE_STRING,
        OM_TYPE_STRING,
        OM_TYPE_BOOL,
        OM_TYPE_NULL,
        OM_TYPE_ACTOR_ID,
        OM_TYPE_DOWN_REASON,
    };
    Om_Type_Store types = {0};
    if (om_type_store_init(&types) != OM_STATUS_OK) return 0;

    Om_Hir_Node nodes[MAX_NODES] = {0};
    Om_Hir_Node_Id children[MAX_NODES - 1] = {0};
    Om_Hir_Module module = {
        .id = {0},
        .source_id = {0},
        .canonical_path = {canonical_path, sizeof(canonical_path) - 1},
        .source_length = byte_at(data, size, 0),
        .root = {0},
    };
    Om_Symbol symbols[1] = {0};
    Om_Scope scopes[1] = {0};
    Om_Resolution_Result resolution = {
        .symbols = symbols,
        .scopes = scopes,
    };
    Om_Semantic_Analysis_Result semantic = {0};
    int owned_storage_marker = 0;
    Om_Hir_Project project = {
        .modules = &module,
        .module_count = 1,
        .nodes = nodes,
        .children = children,
        .type_store = &types,
        .resolution = &resolution,
        .semantic = &semantic,
        .owned_storage = &owned_storage_marker,
    };
    Om_Hir_Verification_Result verification = {0};

    const size_t node_count = 1u
        + (size_t)(byte_at(data, size, 1) % (MAX_NODES - 1));
    project.node_count = node_count;
    project.child_count = node_count - 1;
    module.entity_start = 0;
    module.entity_count = 0;
    nodes[0] = (Om_Hir_Node){
        .id = {0},
        .kind = OM_HIR_PROGRAM,
        .module_id = {0},
        .source_node = OM_AST_NODE_ID_INVALID,
        .span = {{0}, 0, module.source_length},
        .symbol_id = OM_SYMBOL_ID_INVALID,
        .related_symbol_id = OM_SYMBOL_ID_INVALID,
        .child_start = 0,
        .child_count = (uint32_t)(node_count - 1),
    };
    for (size_t index = 1; index < node_count; ++index) {
        const size_t kind_index = byte_at(data, size, 2 + index)
            % (sizeof(leaf_kinds) / sizeof(leaf_kinds[0]));
        Om_Type_Id type_id = OM_TYPE_ID_INVALID;
        if (om_type_store_primitive(&types, leaf_types[kind_index], &type_id)
            != OM_STATUS_OK) {
            break;
        }
        nodes[index] = (Om_Hir_Node){
            .id = {(uint32_t)index},
            .kind = leaf_kinds[kind_index],
            .module_id = {0},
            .source_node = OM_AST_NODE_ID_INVALID,
            .span = {{0}, 0, module.source_length},
            .type_id = type_id,
            .symbol_id = OM_SYMBOL_ID_INVALID,
            .related_symbol_id = OM_SYMBOL_ID_INVALID,
            .child_start = (uint32_t)(node_count - 1),
        };
        children[index - 1] = (Om_Hir_Node_Id){(uint32_t)index};
    }

    /* Mutate bounded, constructed fields so the verifier sees both coherent
       and malformed node/child/type relationships. */
    if (size > 0 && node_count > 1) {
        const size_t selected = 1u + (size_t)(data[0] % (node_count - 1));
        if ((byte_at(data, size, 2) & 1u) != 0) {
            nodes[selected].type_id.value =
                (uint32_t)byte_at(data, size, 3);
        }
        if ((byte_at(data, size, 2) & 2u) != 0) {
            children[selected - 1].value =
                (uint32_t)byte_at(data, size, 4) % (uint32_t)(node_count + 1);
        }
        if ((byte_at(data, size, 2) & 4u) != 0) {
            nodes[0].child_count = byte_at(data, size, 5)
                % (uint8_t)(node_count + 1);
        }
    }

    (void)om_hir_verify_project(&project, &verification);
    om_hir_verification_result_destroy(&verification);
    om_type_store_destroy(&types);
    return 0;
}
