#ifndef OMBASIC_MODULE_GRAPH_MODULE_GRAPH_H
#define OMBASIC_MODULE_GRAPH_MODULE_GRAPH_H

#include <stddef.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/vector.h"

typedef struct Om_Module_Graph {
    Om_Vector modules;
} Om_Module_Graph;

/* The result owns its arrays; cycle includes the repeated starting module. */
typedef struct Om_Module_Graph_Order {
    Om_Module_Id *items;
    size_t length;
    Om_Module_Id *cycle;
    size_t cycle_length;
} Om_Module_Graph_Order;

typedef struct Om_Module_Graph_Cycle_Message {
    char *data;
    size_t length;
} Om_Module_Graph_Cycle_Message;

Om_Status om_module_graph_init(Om_Module_Graph *graph);
void om_module_graph_destroy(Om_Module_Graph *graph);
Om_Status om_module_graph_add(
    Om_Module_Graph *graph,
    Om_String_View canonical_path,
    Om_Module_Id *module_id
);
Om_Status om_module_graph_add_import(
    Om_Module_Graph *graph,
    Om_Module_Id importer,
    Om_Module_Id imported
);
Om_String_View om_module_graph_path(const Om_Module_Graph *graph, Om_Module_Id module_id);
Om_Status om_module_graph_order(
    const Om_Module_Graph *graph,
    Om_Module_Graph_Order *order
);
void om_module_graph_order_destroy(Om_Module_Graph_Order *order);
Om_Status om_module_graph_cycle_message(
    const Om_Module_Graph *graph,
    const Om_Module_Graph_Order *order,
    Om_Module_Graph_Cycle_Message *message
);
void om_module_graph_cycle_message_destroy(Om_Module_Graph_Cycle_Message *message);

#endif
