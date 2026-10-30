#include "backend_internal.h"

Om_Status initialize_types(Om_Llvm_Backend_Context *backend)
{
    backend->i1_type = LLVMInt1TypeInContext(backend->context);
    backend->i8_type = LLVMInt8TypeInContext(backend->context);
    backend->i32_type = LLVMInt32TypeInContext(backend->context);
    backend->i64_type = LLVMInt64TypeInContext(backend->context);
    backend->double_type = LLVMDoubleTypeInContext(backend->context);
    backend->byte_pointer_type = LLVMPointerType(backend->i8_type, 0);

    backend->runtime_field_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Field_Descriptor");
    LLVMTypeRef field_members[2] = {
        backend->i64_type, backend->byte_pointer_type,
    };
    LLVMStructSetBody(backend->runtime_field_descriptor_type, field_members,
                      2, 0);
    backend->runtime_type_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Type_Descriptor");
    LLVMTypeRef descriptor_members[15] = {
        backend->i64_type,
        backend->i32_type,
        backend->i64_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->i32_type,
    };
    LLVMStructSetBody(backend->runtime_type_descriptor_type,
                      descriptor_members, 15, 0);

    LLVMTypeRef source_span_members[3] = {
        backend->byte_pointer_type, backend->i32_type, backend->i32_type,
    };
    backend->runtime_source_span_type = LLVMStructTypeInContext(
        backend->context, source_span_members, 3, 0);

    backend->runtime_actor_handler_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Actor_Handler_Descriptor");
    LLVMTypeRef handler_members[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMStructSetBody(backend->runtime_actor_handler_descriptor_type,
                      handler_members, 2, 0);
    backend->runtime_actor_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Actor_Descriptor");
    LLVMTypeRef actor_members[6] = {
        backend->i64_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMStructSetBody(backend->runtime_actor_descriptor_type,
                      actor_members, 6, 0);
    backend->runtime_actor_ref_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Actor_Ref");
    LLVMTypeRef actor_ref_members[1] = {backend->i64_type};
    LLVMStructSetBody(backend->runtime_actor_ref_type, actor_ref_members, 1,
                      0);
    backend->runtime_program_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Program_Descriptor");
    LLVMTypeRef program_members[14] = {
        backend->i32_type,
        backend->i64_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->i64_type,
        LLVMPointerType(backend->runtime_source_span_type, 0),
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMStructSetBody(backend->runtime_program_descriptor_type,
                      program_members, 14, 0);

    LLVMTypeRef context_create_parameters[1] = {backend->i64_type};
    LLVMTypeRef context_create_type = LLVMFunctionType(
        backend->byte_pointer_type, context_create_parameters, 1, 0);
    backend->runtime_context_create = LLVMAddFunction(backend->module,
        "om_runtime_context_create", context_create_type);
    LLVMTypeRef command_line_parameters[3] = {
        backend->byte_pointer_type,
        backend->i32_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef command_line_type = LLVMFunctionType(backend->i32_type,
        command_line_parameters, 3, 0);
    backend->runtime_context_set_command_line = LLVMAddFunction(
        backend->module, "om_runtime_context_set_command_line",
        command_line_type);
    LLVMTypeRef context_destroy_parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef context_destroy_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), context_destroy_parameters,
        1, 0);
    backend->runtime_context_destroy = LLVMAddFunction(backend->module,
        "om_runtime_context_destroy", context_destroy_type);
    LLVMTypeRef set_source_spans_parameters[3] = {
        backend->byte_pointer_type,
        LLVMPointerType(backend->runtime_source_span_type, 0),
        backend->i64_type,
    };
    LLVMTypeRef set_source_spans_type = LLVMFunctionType(backend->i32_type,
        set_source_spans_parameters, 3, 0);
    backend->runtime_set_source_spans = LLVMAddFunction(backend->module,
        "om_runtime_set_source_spans", set_source_spans_type);
    LLVMTypeRef validate_program_parameters[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMTypeRef validate_program_type = LLVMFunctionType(backend->i32_type,
        validate_program_parameters, 2, 0);
    backend->runtime_validate_program = LLVMAddFunction(backend->module,
        "om_runtime_validate_program", validate_program_type);
    LLVMTypeRef entry_function_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef entry_function_type = LLVMFunctionType(backend->i32_type,
        entry_function_parameters, 1, 0);
    LLVMTypeRef run_parameters[2] = {
        backend->byte_pointer_type, LLVMPointerType(entry_function_type, 0),
    };
    LLVMTypeRef run_type = LLVMFunctionType(backend->i32_type,
        run_parameters, 2, 0);
    backend->runtime_run = LLVMAddFunction(backend->module,
        "om_runtime_run", run_type);
    LLVMTypeRef fault_parameters[3] = {
        backend->byte_pointer_type, backend->i32_type, backend->i32_type,
    };
    LLVMTypeRef fault_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), fault_parameters, 3, 0);
    backend->runtime_fault = LLVMAddFunction(backend->module,
        "om_runtime_fault", fault_type);
    LLVMTypeRef roots_push_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMTypeRef roots_push_type = LLVMFunctionType(backend->byte_pointer_type,
        roots_push_parameters, 3, 0);
    backend->runtime_roots_push = LLVMAddFunction(backend->module,
        "om_runtime_roots_push", roots_push_type);
    LLVMTypeRef roots_pop_parameters[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMTypeRef roots_pop_type = LLVMFunctionType(backend->i32_type,
        roots_pop_parameters, 2, 0);
    backend->runtime_roots_pop = LLVMAddFunction(backend->module,
        "om_runtime_roots_pop", roots_pop_type);
    LLVMTypeRef native_pointer_release_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef native_pointer_release_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context),
        native_pointer_release_parameters, 1, 0);
    LLVMTypeRef native_pointer_release_pointer_type =
        LLVMPointerType(native_pointer_release_type, 0);
    LLVMTypeRef native_pointer_create_parameters[5] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        native_pointer_release_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef native_pointer_create_type = LLVMFunctionType(
        backend->i32_type, native_pointer_create_parameters, 5, 0);
    backend->runtime_native_pointer_create = LLVMAddFunction(backend->module,
        "om_runtime_native_pointer_create", native_pointer_create_type);
    LLVMTypeRef native_pointer_access_parameters[3] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef native_pointer_access_type = LLVMFunctionType(
        backend->i32_type, native_pointer_access_parameters, 3, 0);
    backend->runtime_native_pointer_borrow = LLVMAddFunction(backend->module,
        "om_runtime_native_pointer_borrow", native_pointer_access_type);
    backend->runtime_native_pointer_take = LLVMAddFunction(backend->module,
        "om_runtime_native_pointer_take", native_pointer_access_type);
    LLVMTypeRef string_create_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef string_create_type = LLVMFunctionType(backend->i32_type,
        string_create_parameters, 4, 0);
    backend->runtime_string_create = LLVMAddFunction(backend->module,
        "om_runtime_string_create_utf8_default", string_create_type);
    LLVMTypeRef runtime_string_cstring_view_parameters[4] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
        LLVMPointerType(LLVMIntPtrTypeForASInContext(backend->context,
            backend->target_data, 0), 0),
    };
    LLVMTypeRef runtime_string_cstring_view_type = LLVMFunctionType(
        backend->i32_type, runtime_string_cstring_view_parameters, 4, 0);
    backend->runtime_string_cstring_view = LLVMAddFunction(backend->module,
        "om_runtime_string_cstring_view",
        runtime_string_cstring_view_type);
    LLVMTypeRef runtime_string_view_parameters[3] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
    };
    LLVMTypeRef runtime_string_view_type = LLVMFunctionType(
        backend->i32_type, runtime_string_view_parameters, 3, 0);
    backend->runtime_string_view = LLVMAddFunction(backend->module,
        "om_runtime_string_view", runtime_string_view_type);
    LLVMTypeRef runtime_string_from_cstring_parameters[3] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef runtime_string_from_cstring_type = LLVMFunctionType(
        backend->i32_type, runtime_string_from_cstring_parameters, 3, 0);
    backend->runtime_string_from_cstring = LLVMAddFunction(backend->module,
        "om_runtime_string_create_from_cstring",
        runtime_string_from_cstring_type);
    LLVMTypeRef cstring_release_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef cstring_release_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context),
        cstring_release_parameters, 1, 0);
    LLVMTypeRef runtime_string_from_owned_cstring_parameters[4] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(cstring_release_type, 0),
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef runtime_string_from_owned_cstring_type = LLVMFunctionType(
        backend->i32_type, runtime_string_from_owned_cstring_parameters, 4, 0);
    backend->runtime_string_from_owned_cstring = LLVMAddFunction(
        backend->module,
        "om_runtime_string_create_from_owned_cstring",
        runtime_string_from_owned_cstring_type);
    backend->runtime_string_from_owned_utf8_slice = LLVMAddFunction(
        backend->module,
        "om_runtime_string_create_from_owned_utf8_slice",
        runtime_string_from_owned_cstring_type);
    LLVMTypeRef string_format_parameters[4] = {
        backend->byte_pointer_type, backend->i32_type,
        backend->byte_pointer_type, LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef string_format_type = LLVMFunctionType(backend->i32_type,
        string_format_parameters, 4, 0);
    backend->runtime_string_format = LLVMAddFunction(backend->module,
        "om_runtime_string_format_value", string_format_type);
    LLVMTypeRef string_concat_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type, LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef string_concat_type = LLVMFunctionType(backend->i32_type,
        string_concat_parameters, 4, 0);
    backend->runtime_string_concat = LLVMAddFunction(backend->module,
        "om_runtime_string_concat", string_concat_type);
    LLVMTypeRef string_compare_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type, LLVMPointerType(backend->i32_type, 0),
    };
    LLVMTypeRef string_compare_type = LLVMFunctionType(backend->i32_type,
        string_compare_parameters, 4, 0);
    backend->runtime_string_compare = LLVMAddFunction(backend->module,
        "om_runtime_string_compare", string_compare_type);
    LLVMTypeRef print_parameters[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMTypeRef print_type = LLVMFunctionType(backend->i32_type,
        print_parameters, 2, 0);
    backend->runtime_print = LLVMAddFunction(backend->module,
        "om_runtime_print", print_type);

    LLVMTypeRef descriptor_pointer_type = backend->byte_pointer_type;
    LLVMTypeRef value_pointer_type = backend->byte_pointer_type;
    LLVMTypeRef size_pointer_type = LLVMPointerType(backend->i64_type, 0);
    LLVMTypeRef object_create_parameters[3] = {
        backend->byte_pointer_type, descriptor_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef object_create_type = LLVMFunctionType(backend->i32_type,
        object_create_parameters, 3, 0);
    backend->runtime_record_create = LLVMAddFunction(backend->module,
        "om_runtime_record_create", object_create_type);
    backend->runtime_list_create = LLVMAddFunction(backend->module,
        "om_runtime_list_create", object_create_type);
    LLVMTypeRef list_concat_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef list_concat_type = LLVMFunctionType(backend->i32_type,
        list_concat_parameters, 4, 0);
    backend->runtime_list_concat = LLVMAddFunction(backend->module,
        "om_runtime_list_concat", list_concat_type);
    backend->runtime_map_create = LLVMAddFunction(backend->module,
        "om_runtime_map_create", object_create_type);
    LLVMTypeRef record_set_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef record_set_type = LLVMFunctionType(backend->i32_type,
        record_set_parameters, 4, 0);
    backend->runtime_record_set = LLVMAddFunction(backend->module,
        "om_runtime_record_set_value", record_set_type);
    LLVMTypeRef record_get_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef record_get_type = LLVMFunctionType(backend->i32_type,
        record_get_parameters, 4, 0);
    backend->runtime_record_get = LLVMAddFunction(backend->module,
        "om_runtime_record_get", record_get_type);
    LLVMTypeRef list_append_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        value_pointer_type,
    };
    LLVMTypeRef list_append_type = LLVMFunctionType(backend->i32_type,
        list_append_parameters, 3, 0);
    backend->runtime_list_append = LLVMAddFunction(backend->module,
        "om_runtime_list_append_value", list_append_type);
    LLVMTypeRef list_get_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef list_get_type = LLVMFunctionType(backend->i32_type,
        list_get_parameters, 4, 0);
    backend->runtime_list_get = LLVMAddFunction(backend->module,
        "om_runtime_list_get", list_get_type);
    LLVMTypeRef list_length_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        size_pointer_type,
    };
    LLVMTypeRef list_length_type = LLVMFunctionType(backend->i32_type,
        list_length_parameters, 3, 0);
    backend->runtime_list_length = LLVMAddFunction(backend->module,
        "om_runtime_list_length", list_length_type);
    LLVMTypeRef map_set_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        value_pointer_type, value_pointer_type,
    };
    LLVMTypeRef map_set_type = LLVMFunctionType(backend->i32_type,
        map_set_parameters, 4, 0);
    backend->runtime_map_set = LLVMAddFunction(backend->module,
        "om_runtime_map_set_values", map_set_type);
    LLVMTypeRef map_get_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        value_pointer_type, value_pointer_type,
    };
    LLVMTypeRef map_get_type = LLVMFunctionType(backend->i32_type,
        map_get_parameters, 4, 0);
    backend->runtime_map_get = LLVMAddFunction(backend->module,
        "om_runtime_map_get_value", map_get_type);
    LLVMTypeRef map_key_at_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef map_key_at_type = LLVMFunctionType(backend->i32_type,
        map_key_at_parameters, 4, 0);
    backend->runtime_map_key_at = LLVMAddFunction(backend->module,
        "om_runtime_map_key_at", map_key_at_type);
    LLVMTypeRef map_length_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        size_pointer_type,
    };
    LLVMTypeRef map_length_type = LLVMFunctionType(backend->i32_type,
        map_length_parameters, 3, 0);
    backend->runtime_map_length = LLVMAddFunction(backend->module,
        "om_runtime_map_length", map_length_type);
    LLVMTypeRef actor_start_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMTypeRef actor_start_type = LLVMFunctionType(backend->i32_type,
        actor_start_parameters, 3, 0);
    backend->runtime_actor_system_start = LLVMAddFunction(backend->module,
        "om_runtime_actor_system_start", actor_start_type);
    LLVMTypeRef actor_context_parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef actor_context_type = LLVMFunctionType(backend->i32_type,
        actor_context_parameters, 1, 0);
    backend->runtime_actor_system_wait = LLVMAddFunction(backend->module,
        "om_runtime_actor_system_wait", actor_context_type);
    LLVMTypeRef actor_spawn_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        LLVMPointerType(backend->i64_type, 0),
    };
    LLVMTypeRef actor_spawn_type = LLVMFunctionType(backend->i32_type,
        actor_spawn_parameters, 3, 0);
    backend->runtime_actor_spawn_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_spawn_id", actor_spawn_type);
    LLVMTypeRef actor_spawn_at_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, LLVMPointerType(backend->runtime_actor_ref_type, 0),
    };
    LLVMTypeRef actor_spawn_at_type = LLVMFunctionType(backend->i32_type,
        actor_spawn_at_parameters, 4, 0);
    backend->runtime_actor_spawn_at = LLVMAddFunction(backend->module,
        "om_runtime_actor_spawn_at", actor_spawn_at_type);
    LLVMTypeRef distribution_create_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type,
    };
    LLVMTypeRef distribution_create_type = LLVMFunctionType(
        backend->i32_type, distribution_create_parameters, 3, 0);
    backend->runtime_distribution_create = LLVMAddFunction(backend->module,
        "om_runtime_distribution_create", distribution_create_type);
    LLVMTypeRef distribution_destroy_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef distribution_destroy_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context),
        distribution_destroy_parameters, 1, 0);
    backend->runtime_distribution_destroy = LLVMAddFunction(backend->module,
        "om_runtime_distribution_destroy", distribution_destroy_type);
    LLVMTypeRef actor_id_out_parameters[2] = {
        backend->byte_pointer_type, LLVMPointerType(backend->i64_type, 0),
    };
    LLVMTypeRef actor_id_out_type = LLVMFunctionType(backend->i32_type,
        actor_id_out_parameters, 2, 0);
    backend->runtime_actor_self_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_self_id", actor_id_out_type);
    LLVMTypeRef actor_object_out_parameters[2] = {
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef actor_object_out_type = LLVMFunctionType(backend->i32_type,
        actor_object_out_parameters, 2, 0);
    backend->runtime_actor_state_object = LLVMAddFunction(backend->module,
        "om_runtime_actor_state_object", actor_object_out_type);
    backend->runtime_actor_message_object = LLVMAddFunction(backend->module,
        "om_runtime_actor_message_object", actor_object_out_type);
    LLVMTypeRef actor_send_parameters[3] = {
        backend->byte_pointer_type, backend->i64_type,
        backend->byte_pointer_type,
    };
    LLVMTypeRef actor_send_type = LLVMFunctionType(backend->i32_type,
        actor_send_parameters, 3, 0);
    backend->runtime_actor_send_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_send_id", actor_send_type);
    LLVMTypeRef actor_watch_parameters[2] = {
        backend->byte_pointer_type, backend->i64_type,
    };
    LLVMTypeRef actor_watch_type = LLVMFunctionType(backend->i32_type,
        actor_watch_parameters, 2, 0);
    backend->runtime_actor_watch_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_watch_id", actor_watch_type);
    backend->runtime_actor_stop = LLVMAddFunction(backend->module,
        "om_runtime_actor_stop", actor_context_type);
    LLVMTypeRef empty_callback_signature = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), NULL, 0, 0);
    backend->native_callback_function_pointer_type =
        empty_callback_signature == NULL ? NULL
            : LLVMPointerType(empty_callback_signature, 0);
    LLVMTypeRef callback_set_pointer_type =
        backend->native_callback_function_pointer_type == NULL ? NULL
        : LLVMPointerType(backend->native_callback_function_pointer_type, 0);
    LLVMTypeRef callback_sets_pointer_type = callback_set_pointer_type == NULL
        ? NULL : LLVMPointerType(callback_set_pointer_type, 0);
    LLVMTypeRef callback_count_parameters[6] = {
        backend->byte_pointer_type,
        backend->i64_type,
        callback_sets_pointer_type,
        LLVMPointerType(backend->i64_type, 0),
        LLVMPointerType(backend->byte_pointer_type, 0),
        callback_set_pointer_type,
    };
    LLVMTypeRef callback_register_type = LLVMFunctionType(backend->i32_type,
        callback_count_parameters, 6, 0);
    backend->runtime_native_callback_register_many = callback_register_type
        == NULL ? NULL : LLVMAddFunction(backend->module,
            "om_runtime_native_callback_register_many",
            callback_register_type);
    LLVMTypeRef callback_unregister_parameters[3] = {
        backend->byte_pointer_type, backend->i64_type,
        callback_set_pointer_type,
    };
    LLVMTypeRef callback_unregister_type = LLVMFunctionType(
        backend->i32_type, callback_unregister_parameters, 3, 0);
    backend->runtime_native_callback_unregister_many =
        callback_unregister_type == NULL ? NULL : LLVMAddFunction(
            backend->module,
            "om_runtime_native_callback_unregister_many",
            callback_unregister_type);
    LLVMTypeRef callback_post_parameters[3] = {
        backend->native_callback_function_pointer_type,
        LLVMPointerType(backend->i64_type, 0),
        backend->i64_type,
    };
    LLVMTypeRef callback_post_type = LLVMFunctionType(backend->i32_type,
        callback_post_parameters, 3, 0);
    backend->runtime_native_callback_post = callback_post_type == NULL
        ? NULL : LLVMAddFunction(backend->module,
            "om_runtime_native_callback_post", callback_post_type);
    return backend->runtime_context_create == NULL
        || backend->runtime_context_set_command_line == NULL
        || backend->runtime_context_destroy == NULL
        || backend->runtime_set_source_spans == NULL
        || backend->runtime_validate_program == NULL
        || backend->runtime_run == NULL
        || backend->runtime_fault == NULL
        || backend->runtime_roots_push == NULL
        || backend->runtime_roots_pop == NULL
        || backend->runtime_native_pointer_create == NULL
        || backend->runtime_native_pointer_borrow == NULL
        || backend->runtime_native_pointer_take == NULL
        || backend->runtime_string_create == NULL
        || backend->runtime_string_cstring_view == NULL
        || backend->runtime_string_view == NULL
        || backend->runtime_string_from_cstring == NULL
        || backend->runtime_string_from_owned_cstring == NULL
        || backend->runtime_string_from_owned_utf8_slice == NULL
        || backend->runtime_string_format == NULL
        || backend->runtime_string_concat == NULL
        || backend->runtime_string_compare == NULL
        || backend->runtime_print == NULL
        || backend->runtime_record_create == NULL
        || backend->runtime_record_set == NULL
        || backend->runtime_record_get == NULL
        || backend->runtime_list_create == NULL
        || backend->runtime_list_concat == NULL
        || backend->runtime_list_append == NULL
        || backend->runtime_list_get == NULL
        || backend->runtime_list_length == NULL
        || backend->runtime_map_create == NULL
        || backend->runtime_map_set == NULL
        || backend->runtime_map_get == NULL
        || backend->runtime_map_key_at == NULL
        || backend->runtime_map_length == NULL
        || backend->runtime_actor_system_start == NULL
        || backend->runtime_actor_system_wait == NULL
        || backend->runtime_actor_spawn_id == NULL
        || backend->runtime_actor_spawn_at == NULL
        || backend->runtime_distribution_create == NULL
        || backend->runtime_distribution_destroy == NULL
        || backend->runtime_actor_self_id == NULL
        || backend->runtime_actor_state_object == NULL
        || backend->runtime_actor_message_object == NULL
        || backend->runtime_actor_send_id == NULL
        || backend->runtime_actor_watch_id == NULL
        || backend->runtime_actor_stop == NULL
        || backend->runtime_native_callback_register_many == NULL
        || backend->runtime_native_callback_unregister_many == NULL
        || backend->runtime_native_callback_post == NULL
        || backend->native_callback_function_pointer_type == NULL
        || backend->runtime_type_descriptor_type == NULL
        || backend->runtime_field_descriptor_type == NULL
        || backend->runtime_source_span_type == NULL
        || backend->runtime_actor_handler_descriptor_type == NULL
        || backend->runtime_actor_descriptor_type == NULL
        || backend->runtime_actor_ref_type == NULL
        || backend->runtime_program_descriptor_type == NULL
        ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

Om_Status emit_runtime_status_check(Om_Llvm_Backend_Context *backend,
                                           Om_Mir_Block_Id block_id,
                                           LLVMValueRef status_value)
{
    if (status_value == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef zero = LLVMConstInt(backend->i32_type, 0, 0);
    LLVMValueRef failed = LLVMBuildICmp(backend->builder, LLVMIntNE,
        status_value, zero, "runtime_call_failed");
    if (failed == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t span_id = backend->current_instruction == NULL
        ? UINT32_MAX : backend->current_instruction->id.value;
    return add_checked_continuation(backend, block_id, failed,
        OM_LLVM_RUNTIME_FAULT_OPERATION_FAILED, span_id);
}

Om_Status emit_runtime_status_check_as(
    Om_Llvm_Backend_Context *backend,
    Om_Mir_Block_Id block_id,
    LLVMValueRef status_value,
    uint32_t fault_code
)
{
    if (status_value == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef zero = LLVMConstInt(backend->i32_type, 0, 0);
    LLVMValueRef failed = LLVMBuildICmp(backend->builder, LLVMIntNE,
        status_value, zero, "runtime_call_failed");
    if (failed == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t span_id = backend->current_instruction == NULL
        ? UINT32_MAX : backend->current_instruction->id.value;
    return add_checked_continuation(backend, block_id, failed, fault_code,
                                    span_id);
}

Om_Status emit_runtime_object_call(Om_Llvm_Backend_Context *backend,
                                          const Om_Mir_Function *function,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef callee,
                                          LLVMValueRef *arguments,
                                          unsigned argument_count,
                                          LLVMValueRef *result)
{
    LLVMValueRef result_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "runtime_object_result_slot");
    if (result_slot == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), result_slot);
    LLVMValueRef context = context_for_function(backend, function);
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    arguments[0] = context;
    arguments[argument_count - 1] = result_slot;
    LLVMValueRef call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(callee), callee, arguments, argument_count,
        "runtime_object_status");
    Om_Status status = emit_runtime_status_check(backend, block_id, call);
    if (status == OM_STATUS_OK) {
        *result = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, result_slot, "runtime_object_result");
        if (*result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return status;
}

Om_Status emit_runtime_status_call(Om_Llvm_Backend_Context *backend,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef function,
                                          LLVMValueRef *arguments,
                                          unsigned argument_count)
{
    LLVMValueRef call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(function), function, arguments, argument_count,
        "runtime_operation_status");
    return emit_runtime_status_check(backend, block_id, call);
}
