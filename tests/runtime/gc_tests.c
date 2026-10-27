#include "vm_internal.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_VM_GC_Test_Fixture {
    Om_VM_State vm;
    Om_VM_Actor_State actor;
    Om_VM_Heap heap;
    Om_VM_Value root;
} Om_VM_GC_Test_Fixture;

#define GC_TEST_ASSERT(expression)                                              \
    do {                                                                        \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",           \
                          __FILE__, __LINE__, #expression);                     \
            goto cleanup;                                                       \
        }                                                                       \
    } while (false)

static void init_gc_fixture(Om_VM_GC_Test_Fixture *fixture)
{
    (void)memset(&fixture->vm, 0, sizeof(fixture->vm));
    fixture->vm.limits.max_heap_bytes = SIZE_MAX;
    atomic_init(&fixture->vm.heap_bytes_allocated, 0);
    atomic_init(&fixture->vm.gc_collection_count, 0);
    atomic_init(&fixture->vm.instruction_count, 0);
    atomic_init(&fixture->vm.actor_count, 0);
    atomic_init(&fixture->vm.run_actors_spawned, 0);
    atomic_init(&fixture->vm.run_messages_processed, 0);
    atomic_init(&fixture->vm.run_actors_stopped, 0);
    atomic_init(&fixture->vm.scheduler_done, false);
    atomic_init(&fixture->vm.scheduler_failed, false);
    fixture->heap = (Om_VM_Heap){
        .vm = &fixture->vm,
        .execution_state = &fixture->vm,
        .actor_id = 1,
        .max_bytes = SIZE_MAX,
        .gc_threshold = 64,
        .last_status = OM_STATUS_OK
    };
    fixture->root = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    fixture->actor = (Om_VM_Actor_State){
        .id = 1,
        .heap = &fixture->heap,
        .state_values = &fixture->root,
        .state_count = 1
    };
    fixture->vm.actors = &fixture->actor;
    atomic_store(&fixture->vm.actor_count, 1);
}

static bool make_list(Om_VM_Heap *heap, const Om_VM_Value *items,
                      size_t count, Om_VM_Value *value)
{
    return om_vm_heap_make_list(heap, items, count, value) == OM_STATUS_OK;
}

bool om_test_vm_gc_object_graphs(void)
{
    Om_VM_GC_Test_Fixture fixture = {0};
    bool passed = false;
    init_gc_fixture(&fixture);

    Om_VM_Value chain = om_vm_value_null();
    for (size_t index = 0; index < 4096; ++index) {
        Om_VM_Value next = {0};
        GC_TEST_ASSERT(make_list(&fixture.heap, &chain, 1, &next));
        chain = next;
    }
    fixture.root = chain;
    GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
    GC_TEST_ASSERT(fixture.heap.object_count == 4096);

    Om_VM_Value cycle_left = {0};
    Om_VM_Value cycle_right = {0};
    GC_TEST_ASSERT(make_list(&fixture.heap,
                             &(Om_VM_Value){.tag = OM_VM_VALUE_NULL}, 1,
                             &cycle_left));
    GC_TEST_ASSERT(make_list(&fixture.heap,
                             &(Om_VM_Value){.tag = OM_VM_VALUE_NULL}, 1,
                             &cycle_right));
    ((Om_VM_List_Object *)cycle_left.as.object)->items[0] = cycle_right;
    ((Om_VM_List_Object *)cycle_right.as.object)->items[0] = cycle_left;
    GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
    GC_TEST_ASSERT(fixture.heap.object_count == 4096);

    fixture.root = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
    GC_TEST_ASSERT(fixture.heap.object_count == 0);
    GC_TEST_ASSERT(fixture.vm.heap_bytes_allocated == 0);

    cycle_left = (Om_VM_Value){0};
    cycle_right = (Om_VM_Value){0};
    GC_TEST_ASSERT(make_list(&fixture.heap,
                             &(Om_VM_Value){.tag = OM_VM_VALUE_NULL}, 1,
                             &cycle_left));
    GC_TEST_ASSERT(make_list(&fixture.heap,
                             &(Om_VM_Value){.tag = OM_VM_VALUE_NULL}, 1,
                             &cycle_right));
    ((Om_VM_List_Object *)cycle_left.as.object)->items[0] = cycle_right;
    ((Om_VM_List_Object *)cycle_right.as.object)->items[0] = cycle_left;
    fixture.root = cycle_left;
    GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
    GC_TEST_ASSERT(fixture.heap.object_count == 2);
    fixture.root = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    for (size_t repeat = 0; repeat < 8; ++repeat) {
        GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
        GC_TEST_ASSERT(fixture.heap.object_count == 0);
    }

    Om_VM_Value leaf = {0};
    const char text[] = "nested leaf";
    GC_TEST_ASSERT(om_vm_heap_make_string(
        &fixture.heap, (Om_String_View){text, sizeof(text) - 1}, &leaf)
        == OM_STATUS_OK);
    Om_VM_Value list = {0};
    GC_TEST_ASSERT(make_list(&fixture.heap, &leaf, 1, &list));
    Om_VM_Value map_key = {0};
    const char key[] = "key";
    GC_TEST_ASSERT(om_vm_heap_make_string(
        &fixture.heap, (Om_String_View){key, sizeof(key) - 1}, &map_key)
        == OM_STATUS_OK);
    Om_VM_Map_Entry entry = {.key = map_key, .value = list};
    Om_VM_Value map = {0};
    GC_TEST_ASSERT(om_vm_heap_make_map(&fixture.heap, &entry, 1, &map)
                   == OM_STATUS_OK);
    Om_VM_Value record = {0};
    GC_TEST_ASSERT(om_vm_heap_make_record(&fixture.heap, OM_VM_OBJECT_RECORD,
                                           0, &map, 1, &record)
                   == OM_STATUS_OK);
    Om_VM_Value result = {0};
    GC_TEST_ASSERT(om_vm_heap_make_result(&fixture.heap, true, record, &result)
                   == OM_STATUS_OK);
    Om_VM_Value iterator = {0};
    GC_TEST_ASSERT(om_vm_heap_make_iterator(
        &fixture.heap, OM_VM_ITERATOR_MAP, map.as.object, &iterator)
        == OM_STATUS_OK);
    Om_VM_Value roots[2] = {result, iterator};
    Om_VM_Value outer = {0};
    GC_TEST_ASSERT(make_list(&fixture.heap, roots, 2, &outer));
    Om_VM_Value function = {0};
    GC_TEST_ASSERT(om_vm_heap_make_function(&fixture.heap, 0, &function)
                   == OM_STATUS_OK);
    fixture.root = outer;
    GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
    GC_TEST_ASSERT(fixture.heap.object_count == 8);
    GC_TEST_ASSERT(fixture.heap.bytes_allocated
                   == fixture.vm.heap_bytes_allocated);
    passed = true;

cleanup:
    om_vm_heap_destroy(&fixture.heap);
    return passed;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

bool om_test_vm_gc_graph_fuzz(void)
{
    enum { NODE_COUNT = 96, ROOT_COUNT = 12, ROUND_COUNT = 24 };
    Om_VM_GC_Test_Fixture fixture = {0};
    Om_VM_Value nodes[NODE_COUNT] = {0};
    Om_VM_Value *roots = NULL;
    bool edges[NODE_COUNT][2] = {{false}};
    size_t targets[NODE_COUNT][2] = {{0}};
    bool reachable[NODE_COUNT] = {false};
    size_t queue[NODE_COUNT] = {0};
    bool passed = false;
    init_gc_fixture(&fixture);
    roots = om_alloc_array(ROOT_COUNT, sizeof(*roots));
    GC_TEST_ASSERT(roots != NULL);
    fixture.actor.state_values = roots;
    fixture.actor.state_count = ROOT_COUNT;

    for (uint32_t round = 1; round <= ROUND_COUNT; ++round) {
        uint32_t random_state = UINT32_C(0x9e3779b9) * round;
        for (size_t index = 0; index < ROOT_COUNT; ++index) {
            roots[index] = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
        }
        for (size_t index = 0; index < NODE_COUNT; ++index) {
            Om_VM_Value items[2] = {
                {.tag = OM_VM_VALUE_NULL}, {.tag = OM_VM_VALUE_NULL}
            };
            GC_TEST_ASSERT(make_list(&fixture.heap, items, 2, &nodes[index]));
        }
        for (size_t index = 0; index < NODE_COUNT; ++index) {
            Om_VM_List_Object *list =
                (Om_VM_List_Object *)nodes[index].as.object;
            for (size_t slot = 0; slot < 2; ++slot) {
                const uint32_t choice = next_random(&random_state);
                if ((choice & 3U) != 0) {
                    const size_t target =
                        (size_t)(next_random(&random_state) % NODE_COUNT);
                    edges[index][slot] = true;
                    targets[index][slot] = target;
                    list->items[slot] = nodes[target];
                } else {
                    edges[index][slot] = false;
                    list->items[slot] = om_vm_value_int((int64_t)choice);
                }
            }
        }
        size_t tail = 0;
        for (size_t root_index = 0; root_index < ROOT_COUNT; ++root_index) {
            const size_t node_index =
                (size_t)(next_random(&random_state) % NODE_COUNT);
            roots[root_index] = nodes[node_index];
            if (!reachable[node_index]) {
                reachable[node_index] = true;
                queue[tail++] = node_index;
            }
        }
        for (size_t head = 0; head < tail; ++head) {
            const size_t source = queue[head];
            for (size_t slot = 0; slot < 2; ++slot) {
                if (!edges[source][slot]) {
                    continue;
                }
                const size_t target = targets[source][slot];
                if (!reachable[target]) {
                    reachable[target] = true;
                    queue[tail++] = target;
                }
            }
        }
        size_t expected_count = 0;
        for (size_t index = 0; index < NODE_COUNT; ++index) {
            expected_count += reachable[index] ? 1 : 0;
        }
        GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
        GC_TEST_ASSERT(fixture.heap.object_count == expected_count);
        size_t actual_count = 0;
        for (Om_VM_Object *object = fixture.heap.objects;
             object != NULL; object = object->next) {
            size_t node_index = NODE_COUNT;
            for (size_t index = 0; index < NODE_COUNT; ++index) {
                if (nodes[index].as.object == object) {
                    node_index = index;
                    break;
                }
            }
            GC_TEST_ASSERT(node_index < NODE_COUNT && reachable[node_index]);
            actual_count += 1;
        }
        GC_TEST_ASSERT(actual_count == expected_count);

        for (size_t index = 0; index < ROOT_COUNT; ++index) {
            roots[index] = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
        }
        GC_TEST_ASSERT(om_vm_heap_collect(&fixture.heap) == OM_STATUS_OK);
        GC_TEST_ASSERT(fixture.heap.object_count == 0);
        (void)memset(reachable, 0, sizeof(reachable));
        (void)memset(edges, 0, sizeof(edges));
    }
    passed = true;

cleanup:
    om_vm_heap_destroy(&fixture.heap);
    om_free(roots);
    return passed;
}
