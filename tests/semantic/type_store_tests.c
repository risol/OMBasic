#include "test.h"

#include "ombasic/semantic/type_store.h"

#define TYPE_STORE_ASSERT(expression)                                          \
    do {                                                                        \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",           \
                          __FILE__, __LINE__, #expression);                     \
            goto cleanup;                                                       \
        }                                                                       \
    } while (false)

static bool get_primitive(const Om_Type_Store *store, Om_Type_Kind kind,
                          Om_Type_Id *type_id)
{
    return om_type_store_primitive(store, kind, type_id) == OM_STATUS_OK;
}

bool om_test_type_store_structural_unification(void)
{
    Om_Type_Store store = {0};
    bool passed = false;
    TYPE_STORE_ASSERT(om_type_store_init(&store) == OM_STATUS_OK);
    Om_Type_Id int_type = OM_TYPE_ID_INVALID;
    Om_Type_Id string_type = OM_TYPE_ID_INVALID;
    Om_Type_Id bool_type = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_INT, &int_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_STRING, &string_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_BOOL, &bool_type));

    Om_Type_Id element = OM_TYPE_ID_INVALID;
    Om_Type_Id list = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 2, &element) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_list(&store, element, &list) == OM_STATUS_OK);
    Om_Type_Id list_again = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_list(&store, element, &list_again) == OM_STATUS_OK
                      && list_again.value == list.value);
    Om_Type_Id concrete_list = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_list(&store, int_type,
                                         &concrete_list) == OM_STATUS_OK);
    bool unified = false;
    TYPE_STORE_ASSERT(om_type_store_unify(&store, list, concrete_list,
                                          &unified) == OM_STATUS_OK
                      && unified);
    Om_Type_View view = {0};
    TYPE_STORE_ASSERT(om_type_store_get(&store, list, &view) == OM_STATUS_OK
                      && view.kind == OM_TYPE_LIST && view.child_count == 1);
    Om_Type_Id resolved_element = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, list, 0,
                                             &resolved_element) == OM_STATUS_OK);
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, resolved_element,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == int_type.value);

    Om_Type_Id map_key = OM_TYPE_ID_INVALID;
    Om_Type_Id map_value = OM_TYPE_ID_INVALID;
    Om_Type_Id error_type = OM_TYPE_ID_INVALID;
    Om_Type_Id generic_map = OM_TYPE_ID_INVALID;
    Om_Type_Id generic_result = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 2, &map_key) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 2, &map_value) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 2, &error_type) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_map(&store, map_key, map_value,
                                        &generic_map) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_result(&store, generic_map, error_type,
                                           &generic_result) == OM_STATUS_OK);
    Om_Type_Id expected_map = OM_TYPE_ID_INVALID;
    Om_Type_Id expected_result = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_map(&store, string_type, int_type,
                                        &expected_map) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_result(&store, expected_map, bool_type,
                                          &expected_result) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, generic_result, expected_result,
                                          &unified) == OM_STATUS_OK
                      && unified);
    Om_Type_Id resolved_key = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_value = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_error = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_map = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, generic_map, 0,
                                             &resolved_key) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, generic_map, 1,
                                             &resolved_value) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, resolved_key,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == string_type.value);
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, resolved_value,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == int_type.value);
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, generic_result, 0,
                                             &resolved_map) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, generic_result, 1,
                                             &resolved_error) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_get(&store, resolved_map, &view) == OM_STATUS_OK
                      && view.kind == OM_TYPE_MAP && view.child_count == 2);
    Om_Type_Id resolved_result_key = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_result_value = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, resolved_map, 0,
                                             &resolved_result_key) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, resolved_map, 1,
                                             &resolved_result_value) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, resolved_result_key,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == string_type.value);
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, resolved_result_value,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == int_type.value);
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, resolved_error,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == bool_type.value);
    passed = true;

cleanup:
    om_type_store_destroy(&store);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_type_store_occurs_check_and_rollback(void)
{
    Om_Type_Store store = {0};
    bool passed = false;
    TYPE_STORE_ASSERT(om_type_store_init(&store) == OM_STATUS_OK);
    Om_Type_Id recursive = OM_TYPE_ID_INVALID;
    Om_Type_Id recursive_list = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &recursive) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_list(&store, recursive,
                                         &recursive_list) == OM_STATUS_OK);
    bool unified = true;
    TYPE_STORE_ASSERT(om_type_store_unify(&store, recursive, recursive_list,
                                          &unified) == OM_STATUS_OK
                      && !unified);
    Om_Type_View view = {0};
    TYPE_STORE_ASSERT(om_type_store_get(&store, recursive, &view) == OM_STATUS_OK
                      && view.kind == OM_TYPE_VARIABLE
                      && view.type_variable_unbound);

    Om_Type_Id first = OM_TYPE_ID_INVALID;
    Om_Type_Id int_type = OM_TYPE_ID_INVALID;
    Om_Type_Id float_type = OM_TYPE_ID_INVALID;
    Om_Type_Id string_type = OM_TYPE_ID_INVALID;
    Om_Type_Id unit_type = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &first) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_INT, &int_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_FLOAT, &float_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_STRING, &string_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_UNIT, &unit_type));
    const Om_Type_Id left_parameters[2] = {first, int_type};
    const Om_Type_Id right_parameters[2] = {float_type, string_type};
    Om_Type_Id left_function = OM_TYPE_ID_INVALID;
    Om_Type_Id right_function = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_function(&store, OM_TYPE_FUNCTION,
                                             left_parameters, 2, unit_type,
                                             &left_function) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_function(&store, OM_TYPE_FUNCTION,
                                             right_parameters, 2, unit_type,
                                             &right_function) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, left_function, right_function,
                                          &unified) == OM_STATUS_OK
                      && !unified);
    TYPE_STORE_ASSERT(om_type_store_get(&store, first, &view) == OM_STATUS_OK
                      && view.kind == OM_TYPE_VARIABLE
                      && view.type_variable_unbound);
    passed = true;

cleanup:
    om_type_store_destroy(&store);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_type_store_nominal_identity(void)
{
    Om_Type_Store store = {0};
    bool passed = false;
    TYPE_STORE_ASSERT(om_type_store_init(&store) == OM_STATUS_OK);
    const Om_Symbol_Id user_symbol = {10};
    const Om_Symbol_Id order_symbol = {11};
    Om_Type_Id user = OM_TYPE_ID_INVALID;
    Om_Type_Id user_again = OM_TYPE_ID_INVALID;
    Om_Type_Id order = OM_TYPE_ID_INVALID;
    Om_Type_Id message = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_nominal(&store, OM_TYPE_RECORD,
                                            user_symbol, &user) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_nominal(&store, OM_TYPE_RECORD,
                                            user_symbol, &user_again) == OM_STATUS_OK
                      && user.value == user_again.value);
    TYPE_STORE_ASSERT(om_type_store_nominal(&store, OM_TYPE_RECORD,
                                            order_symbol, &order) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_nominal(&store, OM_TYPE_MESSAGE,
                                            user_symbol, &message) == OM_STATUS_OK);
    bool unified = true;
    TYPE_STORE_ASSERT(om_type_store_unify(&store, user, order,
                                          &unified) == OM_STATUS_OK
                      && !unified);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, user, message,
                                          &unified) == OM_STATUS_OK
                      && !unified);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, user, user_again,
                                          &unified) == OM_STATUS_OK
                      && unified);
    passed = true;

cleanup:
    om_type_store_destroy(&store);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_type_store_generalize_and_instantiate(void)
{
    Om_Type_Store store = {0};
    Om_Type_Scheme scheme = {0};
    bool passed = false;
    TYPE_STORE_ASSERT(om_type_store_init(&store) == OM_STATUS_OK);
    Om_Type_Id variable = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &variable) == OM_STATUS_OK);
    const Om_Type_Id parameters[1] = {variable};
    Om_Type_Id identity = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_function(&store, OM_TYPE_FUNCTION,
                                             parameters, 1, variable,
                                             &identity) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_generalize(&store, identity, 0,
                                               &scheme) == OM_STATUS_OK
                      && scheme.quantified_variable_count == 1);

    Om_Type_Id first_instance = OM_TYPE_ID_INVALID;
    Om_Type_Id second_instance = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &scheme, 0,
                                                &first_instance) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &scheme, 0,
                                                &second_instance) == OM_STATUS_OK);
    Om_Type_Id first_argument = OM_TYPE_ID_INVALID;
    Om_Type_Id second_argument = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, first_instance, 0,
                                             &first_argument) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, second_instance, 0,
                                             &second_argument) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(first_argument.value != second_argument.value);

    Om_Type_Id int_type = OM_TYPE_ID_INVALID;
    Om_Type_Id string_type = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_INT, &int_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_STRING, &string_type));
    const Om_Type_Id int_parameters[1] = {int_type};
    const Om_Type_Id string_parameters[1] = {string_type};
    Om_Type_Id int_identity = OM_TYPE_ID_INVALID;
    Om_Type_Id string_identity = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_function(&store, OM_TYPE_FUNCTION,
                                             int_parameters, 1, int_type,
                                             &int_identity) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_function(&store, OM_TYPE_FUNCTION,
                                             string_parameters, 1, string_type,
                                             &string_identity) == OM_STATUS_OK);
    bool unified = false;
    TYPE_STORE_ASSERT(om_type_store_unify(&store, first_instance, int_identity,
                                          &unified) == OM_STATUS_OK
                      && unified);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, second_instance, string_identity,
                                          &unified) == OM_STATUS_OK
                      && unified);

    Om_Type_Id alias = OM_TYPE_ID_INVALID;
    Om_Type_Id underlying = OM_TYPE_ID_INVALID;
    Om_Type_Id aliased_list = OM_TYPE_ID_INVALID;
    Om_Type_Scheme alias_scheme = {0};
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 2, &alias) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 2,
                                             &underlying) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, alias, underlying,
                                          &unified) == OM_STATUS_OK
                      && unified);
    TYPE_STORE_ASSERT(om_type_store_list(&store, alias,
                                         &aliased_list) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_generalize(&store, aliased_list, 0,
                                               &alias_scheme) == OM_STATUS_OK
                      && alias_scheme.quantified_variable_count == 1);
    Om_Type_Id alias_instance = OM_TYPE_ID_INVALID;
    Om_Type_Id fresh_alias = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &alias_scheme, 0,
                                                &alias_instance) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, alias_instance, 0,
                                             &fresh_alias) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_resolve(&store, fresh_alias,
                                            &resolved) == OM_STATUS_OK
                      && resolved.value == fresh_alias.value
                      && resolved.value != underlying.value);
    om_type_scheme_destroy(&alias_scheme);
    passed = true;

cleanup:
    om_type_scheme_destroy(&alias_scheme);
    om_type_scheme_destroy(&scheme);
    om_type_store_destroy(&store);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_type_store_scheme_constraints(void)
{
    Om_Type_Store store = {0};
    Om_Type_Scheme scheme = {0};
    Om_Type_Scheme actor_scheme = {0};
    Om_Type_Scheme message_scheme = {0};
    bool passed = false;
    TYPE_STORE_ASSERT(om_type_store_init(&store) == OM_STATUS_OK);
    Om_Type_Id numeric = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &numeric) == OM_STATUS_OK);
    bool satisfied = false;
    TYPE_STORE_ASSERT(om_type_store_require(
                          &store, numeric,
                          OM_TYPE_CONSTRAINT_NUMERIC, &satisfied)
                      == OM_STATUS_OK
                      && satisfied);
    TYPE_STORE_ASSERT(om_type_store_generalize(&store, numeric, 0,
                                               &scheme) == OM_STATUS_OK
                      && scheme.quantified_variable_count == 1
                      && scheme.quantified_constraints[0]
                         == (uint32_t)OM_TYPE_CONSTRAINT_NUMERIC);

    Om_Type_Id float_instance = OM_TYPE_ID_INVALID;
    Om_Type_Id string_instance = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &scheme, 0,
                                                &float_instance) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &scheme, 0,
                                                &string_instance) == OM_STATUS_OK);
    Om_Type_Id float_type = OM_TYPE_ID_INVALID;
    Om_Type_Id string_type = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_FLOAT, &float_type));
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_STRING, &string_type));
    bool unified = false;
    TYPE_STORE_ASSERT(om_type_store_unify(&store, float_instance, float_type,
                                          &unified) == OM_STATUS_OK
                      && unified);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, string_instance, string_type,
                                          &unified) == OM_STATUS_OK
                      && !unified);
    Om_Type_View view = {0};
    TYPE_STORE_ASSERT(om_type_store_get(&store, string_instance, &view)
                          == OM_STATUS_OK
                      && view.kind == OM_TYPE_VARIABLE
                      && view.constraints
                         == (uint32_t)OM_TYPE_CONSTRAINT_NUMERIC);

    Om_Type_Id actor_variable = OM_TYPE_ID_INVALID;
    Om_Type_Id actor_instance = OM_TYPE_ID_INVALID;
    Om_Type_Id actor_reference = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &actor_variable)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_require(
                          &store, actor_variable,
                          OM_TYPE_CONSTRAINT_ACTOR_REF, &satisfied)
                      == OM_STATUS_OK
                      && satisfied);
    TYPE_STORE_ASSERT(om_type_store_generalize(&store, actor_variable, 0,
                                               &actor_scheme) == OM_STATUS_OK
                      && actor_scheme.quantified_variable_count == 1
                      && actor_scheme.quantified_constraints[0]
                         == (uint32_t)OM_TYPE_CONSTRAINT_ACTOR_REF);
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &actor_scheme, 0,
                                                &actor_instance) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_actor_ref(
                          &store, (Om_Symbol_Id){17}, &actor_reference)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, actor_instance,
                                          actor_reference, &unified)
                      == OM_STATUS_OK
                      && unified);
    Om_Type_Id wrong_actor_instance = OM_TYPE_ID_INVALID;
    Om_Type_Id int_type = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &actor_scheme, 0,
                                                &wrong_actor_instance)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(get_primitive(&store, OM_TYPE_INT, &int_type));
    TYPE_STORE_ASSERT(om_type_store_unify(&store, wrong_actor_instance,
                                          int_type, &unified)
                      == OM_STATUS_OK
                      && !unified);

    Om_Type_Id message_variable = OM_TYPE_ID_INVALID;
    Om_Type_Id message_instance = OM_TYPE_ID_INVALID;
    Om_Type_Id message_type = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &message_variable)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_require(
                          &store, message_variable,
                          OM_TYPE_CONSTRAINT_MESSAGE, &satisfied)
                      == OM_STATUS_OK
                      && satisfied);
    TYPE_STORE_ASSERT(om_type_store_generalize(&store, message_variable, 0,
                                               &message_scheme) == OM_STATUS_OK
                      && message_scheme.quantified_variable_count == 1
                      && message_scheme.quantified_constraints[0]
                         == (uint32_t)OM_TYPE_CONSTRAINT_MESSAGE);
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &message_scheme, 0,
                                                &message_instance) == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_nominal(&store, OM_TYPE_MESSAGE,
                                            (Om_Symbol_Id){23}, &message_type)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, message_instance,
                                          message_type, &unified)
                      == OM_STATUS_OK
                      && unified);
    Om_Type_Id wrong_message_instance = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_instantiate(&store, &message_scheme, 0,
                                                &wrong_message_instance)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_unify(&store, wrong_message_instance,
                                          int_type, &unified)
                      == OM_STATUS_OK
                      && !unified);
    passed = true;

cleanup:
    om_type_scheme_destroy(&message_scheme);
    om_type_scheme_destroy(&actor_scheme);
    om_type_scheme_destroy(&scheme);
    om_type_store_destroy(&store);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_type_store_actor_ref_protocol_variable(void)
{
    Om_Type_Store store = {0};
    bool passed = false;
    TYPE_STORE_ASSERT(om_type_store_init(&store) == OM_STATUS_OK);
    Om_Type_Id protocol_variable = OM_TYPE_ID_INVALID;
    Om_Type_Id generic_reference = OM_TYPE_ID_INVALID;
    Om_Type_Id concrete_reference = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_variable(&store, 1, &protocol_variable)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_actor_ref_type(
                          &store, protocol_variable, &generic_reference)
                      == OM_STATUS_OK);
    TYPE_STORE_ASSERT(om_type_store_actor_ref(
                          &store, (Om_Symbol_Id){42}, &concrete_reference)
                      == OM_STATUS_OK);
    bool unified = false;
    TYPE_STORE_ASSERT(om_type_store_unify(&store, generic_reference,
                                          concrete_reference, &unified)
                      == OM_STATUS_OK
                      && unified);
    Om_Type_Id resolved_protocol = OM_TYPE_ID_INVALID;
    TYPE_STORE_ASSERT(om_type_store_child_at(&store, generic_reference, 0,
                                             &resolved_protocol) == OM_STATUS_OK);
    Om_Type_View view = {0};
    TYPE_STORE_ASSERT(om_type_store_get(&store, resolved_protocol, &view)
                      == OM_STATUS_OK
                      && view.kind == OM_TYPE_ACTOR_PROTOCOL
                      && view.nominal_symbol.value == 42);
    passed = true;

cleanup:
    om_type_store_destroy(&store);
    OM_TEST_ASSERT(passed);
    return true;
}
