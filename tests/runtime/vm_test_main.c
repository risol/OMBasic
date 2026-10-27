#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

bool om_test_vm_runs_compiled_program(void);
bool om_test_vm_distribution_handshake_info(void);
bool om_test_vm_node_ref_values(void);
bool om_test_vm_distribution_routes_actor_messages(void);
bool om_test_vm_propagates_result_error(void);
bool om_test_vm_formats_interpolation(void);
bool om_test_vm_reports_runtime_error(void);
bool om_test_vm_indirect_calls(void);
bool om_test_vm_float_operations(void);
bool om_test_vm_reports_integer_overflow(void);
bool om_test_vm_loader_rejects_invalid_image(void);
bool om_test_vm_enforces_call_depth_limit(void);
bool om_test_vm_unwraps_error_pattern(void);
bool om_test_vm_constructs_message(void);
bool om_test_vm_schedules_actors_deterministically(void);
bool om_test_vm_system_actors_complete_through_messages(void);
bool om_test_vm_failure_cancels_pending_timer(void);
bool om_test_vm_stop_discards_queued_messages(void);
bool om_test_vm_actor_state_isolation(void);
bool om_test_vm_enforces_actor_limit(void);
bool om_test_vm_enforces_mailbox_limit(void);
bool om_test_vm_reports_failed_actor_to_watchers(void);
bool om_test_vm_watches_actor_that_already_stopped(void);
bool om_test_vm_gc_object_graphs(void);
bool om_test_vm_gc_graph_fuzz(void);
bool om_test_vm_actor_gc_stress(void);
bool om_test_vm_actor_instruction_quantum_fairness(void);
bool om_test_vm_actor_message_stress_concurrency(void);
bool om_test_vm_native_callback_enters_actor_mailbox(void);
bool om_test_vm_calls_omi_native_functions(void);
bool om_test_vm_compiles_and_calls_omi_native_function(void);
bool om_test_vm_native_callback_reaches_actor_mailbox(void);
bool om_test_vm_blocking_native_call_releases_actor_worker(void);

typedef bool (*Om_VM_Test_Function)(void);

typedef struct Om_VM_Test_Case {
    const char *name;
    Om_VM_Test_Function function;
} Om_VM_Test_Case;

int main(void)
{
    static const Om_VM_Test_Case tests[] = {
        {"vm_runs_compiled_program", om_test_vm_runs_compiled_program},
        {"vm_distribution_handshake_info",
         om_test_vm_distribution_handshake_info},
        {"vm_node_ref_values", om_test_vm_node_ref_values},
        {"vm_distribution_routes_actor_messages",
         om_test_vm_distribution_routes_actor_messages},
        {"vm_propagates_result_error", om_test_vm_propagates_result_error},
        {"vm_formats_interpolation", om_test_vm_formats_interpolation},
        {"vm_reports_runtime_error", om_test_vm_reports_runtime_error},
        {"vm_indirect_calls", om_test_vm_indirect_calls},
        {"vm_float_operations", om_test_vm_float_operations},
        {"vm_reports_integer_overflow", om_test_vm_reports_integer_overflow},
        {"vm_loader_rejects_invalid_image",
         om_test_vm_loader_rejects_invalid_image},
        {"vm_enforces_call_depth_limit", om_test_vm_enforces_call_depth_limit},
        {"vm_unwraps_error_pattern", om_test_vm_unwraps_error_pattern},
        {"vm_constructs_message", om_test_vm_constructs_message},
        {"vm_schedules_actors_deterministically",
         om_test_vm_schedules_actors_deterministically},
        {"vm_system_actors_complete_through_messages",
         om_test_vm_system_actors_complete_through_messages},
        {"vm_failure_cancels_pending_timer",
         om_test_vm_failure_cancels_pending_timer},
        {"vm_stop_discards_queued_messages",
         om_test_vm_stop_discards_queued_messages},
        {"vm_actor_state_isolation", om_test_vm_actor_state_isolation},
        {"vm_enforces_actor_limit", om_test_vm_enforces_actor_limit},
        {"vm_enforces_mailbox_limit", om_test_vm_enforces_mailbox_limit},
        {"vm_reports_failed_actor_to_watchers",
         om_test_vm_reports_failed_actor_to_watchers},
        {"vm_watches_actor_that_already_stopped",
         om_test_vm_watches_actor_that_already_stopped},
        {"vm_gc_object_graphs", om_test_vm_gc_object_graphs},
        {"vm_gc_graph_fuzz", om_test_vm_gc_graph_fuzz},
        {"vm_actor_gc_stress", om_test_vm_actor_gc_stress},
        {"vm_actor_instruction_quantum_fairness",
         om_test_vm_actor_instruction_quantum_fairness},
        {"vm_actor_message_stress_concurrency",
         om_test_vm_actor_message_stress_concurrency},
        {"vm_native_callback_enters_actor_mailbox",
         om_test_vm_native_callback_enters_actor_mailbox},
        {"vm_calls_omi_native_functions",
         om_test_vm_calls_omi_native_functions},
        {"vm_compiles_and_calls_omi_native_function",
         om_test_vm_compiles_and_calls_omi_native_function},
        {"vm_native_callback_reaches_actor_mailbox",
         om_test_vm_native_callback_reaches_actor_mailbox},
        {"vm_blocking_native_call_releases_actor_worker",
         om_test_vm_blocking_native_call_releases_actor_worker}
    };
    size_t failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            failures += 1;
        }
    }
    (void)printf("%zu tests, %zu failures\n",
                 sizeof(tests) / sizeof(tests[0]), failures);
    return failures == 0 ? 0 : 1;
}
