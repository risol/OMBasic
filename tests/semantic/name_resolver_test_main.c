#include "test.h"

bool om_test_name_resolution_visibility(void);
bool om_test_name_resolution_private_member(void);
bool om_test_name_resolution_alias_collision(void);
bool om_test_name_resolution_declaration_diagnostics(void);
bool om_test_name_resolution_bindings_and_scopes(void);
bool om_test_name_resolution_patterns_loops_and_actor(void);
bool om_test_name_resolution_undefined_name(void);
bool om_test_type_store_structural_unification(void);
bool om_test_type_store_occurs_check_and_rollback(void);
bool om_test_type_store_nominal_identity(void);
bool om_test_type_store_generalize_and_instantiate(void);
bool om_test_type_store_scheme_constraints(void);
bool om_test_type_store_actor_ref_protocol_variable(void);
bool om_test_type_inference_project(void);
bool om_test_semantic_constraints(void);

int main(void)
{
    const Om_Test_Case tests[] = {
        {"name_resolution_visibility", om_test_name_resolution_visibility},
        {"name_resolution_private_member", om_test_name_resolution_private_member},
        {"name_resolution_alias_collision", om_test_name_resolution_alias_collision},
        {"name_resolution_declaration_diagnostics",
         om_test_name_resolution_declaration_diagnostics},
        {"name_resolution_bindings_and_scopes",
         om_test_name_resolution_bindings_and_scopes},
        {"name_resolution_patterns_loops_and_actor",
         om_test_name_resolution_patterns_loops_and_actor},
        {"name_resolution_undefined_name", om_test_name_resolution_undefined_name},
        {"type_store_structural_unification",
         om_test_type_store_structural_unification},
        {"type_store_occurs_check_and_rollback",
         om_test_type_store_occurs_check_and_rollback},
        {"type_store_nominal_identity", om_test_type_store_nominal_identity},
        {"type_store_generalize_and_instantiate",
         om_test_type_store_generalize_and_instantiate},
        {"type_store_scheme_constraints",
         om_test_type_store_scheme_constraints},
        {"type_store_actor_ref_protocol_variable",
         om_test_type_store_actor_ref_protocol_variable},
        {"type_inference_project", om_test_type_inference_project},
        {"semantic_constraints", om_test_semantic_constraints},
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t failures = 0;
    for (size_t index = 0; index < test_count; ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            ++failures;
        }
    }
    (void)printf("%zu tests, %zu failures\n", test_count, failures);
    return failures == 0 ? 0 : 1;
}
