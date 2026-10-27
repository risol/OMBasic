#include "test.h"

#include <string.h>

#include "ombasic/module_graph/module_graph.h"

bool om_test_module_graph_order(void)
{
    Om_Module_Graph graph = {0};
    Om_Module_Graph_Order order = {0};
    Om_Module_Id a = OM_MODULE_ID_INVALID;
    Om_Module_Id b = OM_MODULE_ID_INVALID;
    Om_Module_Id c = OM_MODULE_ID_INVALID;
    Om_Module_Id d = OM_MODULE_ID_INVALID;
    Om_Module_Id duplicate = OM_MODULE_ID_INVALID;

    OM_TEST_ASSERT(om_module_graph_init(&graph) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"a.bas", 5}, &a)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"b.bas", 5}, &b)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"c.bas", 5}, &c)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"d.bas", 5}, &d)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"b.bas", 5}, &duplicate)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(duplicate.value == b.value);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, a, b) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, a, b) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, b, c) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, (Om_Module_Id){99}, c)
                   == OM_STATUS_INVALID_ARGUMENT);
    OM_TEST_ASSERT(om_module_graph_order(&graph, &order) == OM_STATUS_OK);
    OM_TEST_ASSERT(order.length == 4 && order.cycle_length == 0);
    OM_TEST_ASSERT(order.items[0].value == c.value);
    OM_TEST_ASSERT(order.items[1].value == b.value);
    OM_TEST_ASSERT(order.items[2].value == a.value);
    OM_TEST_ASSERT(order.items[3].value == d.value);
    const Om_String_View path = om_module_graph_path(&graph, c);
    OM_TEST_ASSERT(path.length == 5 && memcmp(path.data, "c.bas", 5) == 0);
    om_module_graph_order_destroy(&order);
    om_module_graph_destroy(&graph);
    return true;
}

bool om_test_module_graph_cycle(void)
{
    Om_Module_Graph graph = {0};
    Om_Module_Graph_Order order = {0};
    Om_Module_Graph_Cycle_Message message = {0};
    Om_Module_Id a = OM_MODULE_ID_INVALID;
    Om_Module_Id b = OM_MODULE_ID_INVALID;
    Om_Module_Id c = OM_MODULE_ID_INVALID;

    OM_TEST_ASSERT(om_module_graph_init(&graph) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"a.bas", 5}, &a)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"b.bas", 5}, &b)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add(&graph, (Om_String_View){"c.bas", 5}, &c)
                   == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, a, b) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, b, c) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_add_import(&graph, c, a) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_module_graph_order(&graph, &order) == OM_STATUS_CYCLE_DETECTED);
    OM_TEST_ASSERT(order.items == NULL && order.length == 0);
    OM_TEST_ASSERT(order.cycle_length == 4);
    OM_TEST_ASSERT(order.cycle[0].value == a.value);
    OM_TEST_ASSERT(order.cycle[1].value == b.value);
    OM_TEST_ASSERT(order.cycle[2].value == c.value);
    OM_TEST_ASSERT(order.cycle[3].value == a.value);
    OM_TEST_ASSERT(om_module_graph_cycle_message(&graph, &order, &message) == OM_STATUS_OK);
    OM_TEST_ASSERT(message.length == strlen("a.bas -> b.bas -> c.bas -> a.bas"));
    OM_TEST_ASSERT(strcmp(message.data, "a.bas -> b.bas -> c.bas -> a.bas") == 0);
    om_module_graph_cycle_message_destroy(&message);
    om_module_graph_order_destroy(&order);
    om_module_graph_destroy(&graph);
    return true;
}
