#include "ombasic/module_graph/module_graph.h"

#include <stdbool.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/source/import_path.h"

typedef struct Om_Module_Node {
    Om_Module_Id id;
    char *path;
    size_t path_length;
    Om_Vector imports;
} Om_Module_Node;

typedef struct Om_Module_Frame {
    size_t module_index;
    size_t next_import;
} Om_Module_Frame;

static Om_Module_Node *node_at(Om_Module_Graph *graph, Om_Module_Id id)
{
    if (graph == NULL || id.value >= graph->modules.length) {
        return NULL;
    }
    return om_vector_at(&graph->modules, id.value);
}

static const Om_Module_Node *node_at_const(const Om_Module_Graph *graph, Om_Module_Id id)
{
    if (graph == NULL || id.value >= graph->modules.length) {
        return NULL;
    }
    return om_vector_at_const(&graph->modules, id.value);
}

static void node_destroy(Om_Module_Node *node)
{
    if (node == NULL) {
        return;
    }
    om_free(node->path);
    om_vector_destroy(&node->imports);
    *node = (Om_Module_Node){.id = OM_MODULE_ID_INVALID};
}

Om_Status om_module_graph_init(Om_Module_Graph *graph)
{
    if (graph == NULL || graph->modules.data != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_vector_init(&graph->modules, sizeof(Om_Module_Node));
}

void om_module_graph_destroy(Om_Module_Graph *graph)
{
    if (graph == NULL) {
        return;
    }
    for (size_t index = 0; index < graph->modules.length; ++index) {
        node_destroy(om_vector_at(&graph->modules, index));
    }
    om_vector_destroy(&graph->modules);
}

Om_Status om_module_graph_add(
    Om_Module_Graph *graph,
    Om_String_View canonical_path,
    Om_Module_Id *module_id
)
{
    size_t path_storage_size = 0;
    Om_Module_Node node = {.id = OM_MODULE_ID_INVALID};
    Om_Status status = OM_STATUS_OK;
    if (graph == NULL || module_id == NULL || graph->modules.item_size != sizeof(Om_Module_Node)
        || om_import_path_validate(canonical_path) != OM_STATUS_OK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *module_id = OM_MODULE_ID_INVALID;
    for (size_t index = 0; index < graph->modules.length; ++index) {
        const Om_Module_Node *existing = om_vector_at_const(&graph->modules, index);
        const Om_String_View existing_path = {existing->path, existing->path_length};
        if (om_string_view_equal(existing_path, canonical_path)) {
            *module_id = existing->id;
            return OM_STATUS_OK;
        }
    }
    if (graph->modules.length >= UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    if (!om_checked_size_add(canonical_path.length, 1, &path_storage_size)) {
        return OM_STATUS_OVERFLOW;
    }
    node.path = om_alloc(path_storage_size);
    if (node.path == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(node.path, canonical_path.data, canonical_path.length);
    node.path[canonical_path.length] = '\0';
    node.path_length = canonical_path.length;
    node.id.value = (uint32_t)graph->modules.length;
    status = om_vector_init(&node.imports, sizeof(Om_Module_Id));
    if (status != OM_STATUS_OK) {
        node_destroy(&node);
        return status;
    }
    status = om_vector_push(&graph->modules, &node);
    if (status != OM_STATUS_OK) {
        node_destroy(&node);
        return status;
    }
    *module_id = node.id;
    return OM_STATUS_OK;
}

Om_Status om_module_graph_add_import(
    Om_Module_Graph *graph,
    Om_Module_Id importer,
    Om_Module_Id imported
)
{
    Om_Module_Node *node = node_at(graph, importer);
    if (node == NULL || node_at_const(graph, imported) == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < node->imports.length; ++index) {
        const Om_Module_Id *existing = om_vector_at_const(&node->imports, index);
        if (existing->value == imported.value) {
            return OM_STATUS_OK;
        }
    }
    return om_vector_push(&node->imports, &imported);
}

Om_String_View om_module_graph_path(const Om_Module_Graph *graph, Om_Module_Id module_id)
{
    const Om_Module_Node *node = node_at_const(graph, module_id);
    if (node == NULL) {
        return (Om_String_View){0};
    }
    return (Om_String_View){node->path, node->path_length};
}

void om_module_graph_order_destroy(Om_Module_Graph_Order *order)
{
    if (order == NULL) {
        return;
    }
    om_free(order->items);
    om_free(order->cycle);
    *order = (Om_Module_Graph_Order){0};
}

void om_module_graph_cycle_message_destroy(Om_Module_Graph_Cycle_Message *message)
{
    if (message == NULL) {
        return;
    }
    om_free(message->data);
    *message = (Om_Module_Graph_Cycle_Message){0};
}

Om_Status om_module_graph_cycle_message(
    const Om_Module_Graph *graph,
    const Om_Module_Graph_Order *order,
    Om_Module_Graph_Cycle_Message *message
)
{
    static const char separator[] = " -> ";
    size_t text_length = 0;
    size_t allocation_size = 0;
    char *text = NULL;
    if (graph == NULL || order == NULL || message == NULL || message->data != NULL
        || order->cycle == NULL || order->cycle_length < 2
        || order->cycle[0].value != order->cycle[order->cycle_length - 1].value) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *message = (Om_Module_Graph_Cycle_Message){0};
    for (size_t index = 0; index < order->cycle_length; ++index) {
        const Om_String_View path = om_module_graph_path(graph, order->cycle[index]);
        if (path.data == NULL || path.length == 0
            || !om_checked_size_add(text_length, path.length, &text_length)) {
            return path.data == NULL || path.length == 0 ? OM_STATUS_INVALID_ARGUMENT
                                                        : OM_STATUS_OVERFLOW;
        }
        if (index + 1 < order->cycle_length
            && !om_checked_size_add(text_length, sizeof(separator) - 1, &text_length)) {
            return OM_STATUS_OVERFLOW;
        }
    }
    if (!om_checked_size_add(text_length, 1, &allocation_size)) {
        return OM_STATUS_OVERFLOW;
    }
    text = om_alloc(allocation_size);
    if (text == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t offset = 0;
    for (size_t index = 0; index < order->cycle_length; ++index) {
        const Om_String_View path = om_module_graph_path(graph, order->cycle[index]);
        (void)memcpy(text + offset, path.data, path.length);
        offset += path.length;
        if (index + 1 < order->cycle_length) {
            (void)memcpy(text + offset, separator, sizeof(separator) - 1);
            offset += sizeof(separator) - 1;
        }
    }
    text[offset] = '\0';
    message->data = text;
    message->length = text_length;
    return OM_STATUS_OK;
}

Om_Status om_module_graph_order(
    const Om_Module_Graph *graph,
    Om_Module_Graph_Order *order
)
{
    size_t count = 0;
    size_t color_bytes = 0;
    size_t frame_bytes = 0;
    size_t order_bytes = 0;
    uint8_t *colors = NULL;
    Om_Module_Frame *frames = NULL;
    Om_Module_Id *ordered = NULL;
    size_t ordered_count = 0;
    Om_Status status = OM_STATUS_OK;

    if (graph == NULL || order == NULL || order->items != NULL || order->cycle != NULL
        || graph->modules.item_size != sizeof(Om_Module_Node)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    count = graph->modules.length;
    if (count == 0) {
        *order = (Om_Module_Graph_Order){0};
        return OM_STATUS_OK;
    }
    if (!om_checked_size_mul(count, sizeof(*colors), &color_bytes)
        || !om_checked_size_mul(count, sizeof(*frames), &frame_bytes)
        || !om_checked_size_mul(count, sizeof(*ordered), &order_bytes)) {
        return OM_STATUS_OVERFLOW;
    }
    colors = om_alloc(color_bytes);
    frames = om_alloc(frame_bytes);
    ordered = om_alloc(order_bytes);
    if (colors == NULL || frames == NULL || ordered == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t index = 0; index < count; ++index) {
        colors[index] = 0;
    }

    for (size_t root = 0; root < count; ++root) {
        size_t depth = 0;
        if (colors[root] != 0) {
            continue;
        }
        colors[root] = 1;
        frames[depth++] = (Om_Module_Frame){root, 0};
        while (depth != 0) {
            Om_Module_Frame *frame = &frames[depth - 1];
            const Om_Module_Node *node = om_vector_at_const(&graph->modules, frame->module_index);
            if (node == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            if (frame->next_import < node->imports.length) {
                const Om_Module_Id *target = om_vector_at_const(&node->imports, frame->next_import);
                ++frame->next_import;
                if (target == NULL || target->value >= count) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    goto cleanup;
                }
                if (colors[target->value] == 1) {
                    size_t cycle_start = 0;
                    size_t cycle_length = 0;
                    size_t cycle_bytes = 0;
                    Om_Module_Id *cycle = NULL;
                    while (cycle_start < depth
                           && frames[cycle_start].module_index != target->value) {
                        ++cycle_start;
                    }
                    if (cycle_start == depth
                        || !om_checked_size_add(depth - cycle_start, 1, &cycle_length)
                        || !om_checked_size_mul(cycle_length, sizeof(*cycle), &cycle_bytes)) {
                        status = cycle_start == depth
                                     ? OM_STATUS_INVALID_ARGUMENT
                                     : OM_STATUS_OVERFLOW;
                        goto cleanup;
                    }
                    cycle = om_alloc(cycle_bytes);
                    if (cycle == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                    for (size_t index = cycle_start; index < depth; ++index) {
                        const Om_Module_Node *cycle_node = om_vector_at_const(
                            &graph->modules,
                            frames[index].module_index
                        );
                        cycle[index - cycle_start] = cycle_node->id;
                    }
                    cycle[cycle_length - 1] = *target;
                    order->cycle = cycle;
                    order->cycle_length = cycle_length;
                    status = OM_STATUS_CYCLE_DETECTED;
                    goto cleanup;
                }
                if (colors[target->value] == 0) {
                    colors[target->value] = 1;
                    frames[depth++] = (Om_Module_Frame){target->value, 0};
                }
                continue;
            }
            colors[frame->module_index] = 2;
            ordered[ordered_count++] = node->id;
            --depth;
        }
    }
    order->items = ordered;
    order->length = ordered_count;
    ordered = NULL;

cleanup:
    om_free(colors);
    om_free(frames);
    om_free(ordered);
    if (status != OM_STATUS_OK && status != OM_STATUS_CYCLE_DETECTED) {
        om_module_graph_order_destroy(order);
    }
    return status;
}
