#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ombasic/runtime/vm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "ombasic/bytecode/bytecode.h"
#include "ombasic/compiler/native_binding.h"
#include "ombasic/ffi/omi.h"
#include "ombasic/ffi/omi_project.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/source/source_text.h"
#include "ombasic/syntax/lexer.h"
#include "vm_internal.h"

#if !defined(OM_VM_TEST_FIXTURE_ROOT)
#error OM_VM_TEST_FIXTURE_ROOT must name the runtime fixture directory
#endif
#if !defined(OM_VM_ACTOR_TEST_FIXTURE_ROOT)
#error OM_VM_ACTOR_TEST_FIXTURE_ROOT must name the actor fixture directory
#endif
#if !defined(OM_VM_SYSTEM_ACTOR_TEST_FIXTURE_ROOT)
#error OM_VM_SYSTEM_ACTOR_TEST_FIXTURE_ROOT must name the system actor fixture directory
#endif
#if !defined(OM_VM_NATIVE_TEST_FIXTURE_ROOT)
#error OM_VM_NATIVE_TEST_FIXTURE_ROOT must name the Native FFI fixture directory
#endif
#if !defined(OM_VM_NATIVE_CALLBACK_TEST_FIXTURE_ROOT)
#error OM_VM_NATIVE_CALLBACK_TEST_FIXTURE_ROOT must name the Native callback fixture directory
#endif
#if !defined(OM_VM_NATIVE_BLOCKING_TEST_FIXTURE_ROOT)
#error OM_VM_NATIVE_BLOCKING_TEST_FIXTURE_ROOT must name the Native blocking fixture directory
#endif
#if !defined(OM_VM_DISTRIBUTION_TEST_FIXTURE_ROOT)
#error OM_VM_DISTRIBUTION_TEST_FIXTURE_ROOT must name the distribution fixture directory
#endif

#define VM_TEST_ASSERT(expression)                                               \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",             \
                          __FILE__, __LINE__, #expression);                      \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

typedef struct Om_VM_Test_Compilation {
    Om_Source_Root source_root;
    Om_Module_Project project;
    Om_Lexer_Result *lexed;
    Om_Parser_Result *parsed;
    size_t module_count;
    Om_Resolution_Result resolution;
    Om_Type_Inference_Result inference;
    Om_Semantic_Analysis_Result semantic;
    Om_Bytecode_Emit_Result emitted;
} Om_VM_Test_Compilation;

static void destroy_compilation(Om_VM_Test_Compilation *compilation)
{
    if (compilation == NULL) {
        return;
    }
    om_bytecode_emit_result_destroy(&compilation->emitted);
    om_semantic_analysis_result_destroy(&compilation->semantic);
    om_type_inference_result_destroy(&compilation->inference);
    om_resolution_result_destroy(&compilation->resolution);
    for (size_t index = 0; index < compilation->module_count; ++index) {
        if (compilation->parsed != NULL) {
            om_parser_result_destroy(&compilation->parsed[index]);
        }
        if (compilation->lexed != NULL) {
            om_lexer_result_destroy(&compilation->lexed[index]);
        }
    }
    om_free(compilation->parsed);
    om_free(compilation->lexed);
    om_module_project_destroy(&compilation->project);
    om_source_root_destroy(&compilation->source_root);
    *compilation = (Om_VM_Test_Compilation){0};
}

static bool load_compilation_from_config(
    Om_VM_Test_Compilation *compilation, const char *fixture_root,
    const Om_Resolver_Config *resolver_config)
{
    Om_Status status = om_source_root_init(
        &compilation->source_root,
        (Om_String_View){fixture_root, strlen(fixture_root)});
    if (status == OM_STATUS_OK) {
        status = om_module_project_load(
            &compilation->source_root,
            (Om_String_View){"main.bas", 8}, 16384,
            &compilation->project);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "VM fixture load failed: %s\n",
                      om_status_name(status));
        return false;
    }
    compilation->module_count =
        om_module_project_module_count(&compilation->project);
    compilation->lexed = om_alloc_array(compilation->module_count,
                                         sizeof(*compilation->lexed));
    compilation->parsed = om_alloc_array(compilation->module_count,
                                         sizeof(*compilation->parsed));
    if (compilation->module_count == 0 || compilation->lexed == NULL
        || compilation->parsed == NULL) {
        return false;
    }
    (void)memset(compilation->lexed, 0,
                 compilation->module_count * sizeof(*compilation->lexed));
    (void)memset(compilation->parsed, 0,
                 compilation->module_count * sizeof(*compilation->parsed));
    for (size_t index = 0; index < compilation->module_count; ++index) {
        const Om_Source_Text *source = om_module_project_source(
            &compilation->project, (Om_Module_Id){(uint32_t)index});
        if (source == NULL) {
            (void)fprintf(stderr, "VM fixture source missing at module %zu\n",
                          index);
            return false;
        }
        status = om_lexer_scan(source, &compilation->lexed[index]);
        if (status != OM_STATUS_OK
            || compilation->lexed[index].diagnostic_count != 0) {
            (void)fprintf(stderr, "VM fixture lex failed at module %zu: %s",
                          index, om_status_name(status));
            for (size_t diagnostic = 0;
                 diagnostic < compilation->lexed[index].diagnostic_count;
                 ++diagnostic) {
                const Om_Lex_Diagnostic *item =
                    &compilation->lexed[index].diagnostics[diagnostic];
                Om_Source_Location location = {0};
                (void)om_source_text_location(source, item->span.start,
                                              &location);
                (void)fprintf(stderr, " %s:%u:%u",
                    om_lex_diagnostic_code_name(item->code),
                    location.line, location.byte_column);
            }
            (void)fputc('\n', stderr);
            return false;
        }
        status = om_parser_parse(&compilation->lexed[index],
                                 &compilation->parsed[index]);
        if (status != OM_STATUS_OK
            || compilation->parsed[index].diagnostic_count != 0) {
            (void)fprintf(stderr, "VM fixture parse failed at module %zu: %s",
                          index, om_status_name(status));
            for (size_t diagnostic = 0;
                 diagnostic < compilation->parsed[index].diagnostic_count;
                 ++diagnostic) {
                const Om_Parse_Diagnostic *item =
                    &compilation->parsed[index].diagnostics[diagnostic];
                Om_Source_Location location = {0};
                (void)om_source_text_location(source, item->span.start,
                                              &location);
                (void)fprintf(stderr, " %s:%u:%u(expected token=%d keyword=%d)",
                    om_parse_diagnostic_code_name(item->code),
                    location.line, location.byte_column,
                    item->expected_kind, item->expected_keyword);
            }
            (void)fputc('\n', stderr);
            return false;
        }
    }
    const char *failed_stage = "name resolution";
    status = om_name_resolve_project(
        &compilation->project, compilation->parsed,
        compilation->module_count, resolver_config,
        &compilation->resolution);
    if (status == OM_STATUS_OK
        && compilation->resolution.diagnostic_count == 0) {
        failed_stage = "type inference";
        status = om_type_infer_project(
            &compilation->project, compilation->parsed,
            compilation->module_count, &compilation->resolution,
            &compilation->inference);
    }
    if (status == OM_STATUS_OK
        && compilation->inference.diagnostic_count == 0) {
        failed_stage = "semantic analysis";
        status = om_semantic_analyze_project(
            &compilation->project, compilation->parsed,
            compilation->module_count, &compilation->resolution,
            &compilation->inference, &compilation->semantic);
    }
    if (status == OM_STATUS_OK
        && compilation->semantic.diagnostic_count == 0) {
        failed_stage = "bytecode emission";
        status = om_bytecode_emit_project(
            &compilation->project, compilation->parsed,
            compilation->module_count, &compilation->resolution,
            &compilation->inference, &compilation->semantic,
            &compilation->emitted);
    }
    if (status != OM_STATUS_OK || compilation->resolution.diagnostic_count != 0
        || compilation->inference.diagnostic_count != 0
        || compilation->semantic.diagnostic_count != 0) {
        (void)fprintf(stderr,
            "VM fixture %s failed: %s (resolve=%zu infer=%zu semantic=%zu)\n",
            failed_stage, om_status_name(status), compilation->resolution.diagnostic_count,
            compilation->inference.diagnostic_count,
            compilation->semantic.diagnostic_count);
        for (size_t index = 0;
             index < compilation->resolution.diagnostic_count; ++index) {
            const Om_Resolution_Diagnostic *diagnostic =
                &compilation->resolution.diagnostics[index];
            (void)fprintf(stderr, "  %s at %u-%u\n",
                om_resolution_diagnostic_code_name(diagnostic->code),
                diagnostic->span.start, diagnostic->span.end);
        }
        for (size_t index = 0;
             index < compilation->inference.diagnostic_count; ++index) {
            const Om_Type_Inference_Diagnostic *diagnostic =
                &compilation->inference.diagnostics[index];
            (void)fprintf(stderr, "  %s at %u-%u\n",
                om_type_inference_diagnostic_code_name(diagnostic->code),
                diagnostic->span.start, diagnostic->span.end);
        }
        for (size_t index = 0;
             index < compilation->semantic.diagnostic_count; ++index) {
            const Om_Semantic_Analysis_Diagnostic *diagnostic =
                &compilation->semantic.diagnostics[index];
            (void)fprintf(stderr, "  E%04u at %u-%u\n",
                (unsigned)diagnostic->code, diagnostic->span.start,
                diagnostic->span.end);
        }
        for (size_t index = 0;
             index < compilation->emitted.diagnostic_count; ++index) {
            const Om_Bytecode_Emit_Diagnostic *diagnostic =
                &compilation->emitted.diagnostics[index];
            (void)fprintf(stderr, "  emitter code=%u at %u-%u\n",
                (unsigned)diagnostic->code,
                diagnostic->span.start, diagnostic->span.end);
        }
        return false;
    }
    return true;
}

static bool load_compilation_from(Om_VM_Test_Compilation *compilation,
                                  const char *fixture_root)
{
    return load_compilation_from_config(compilation, fixture_root, NULL);
}

static bool load_compilation(Om_VM_Test_Compilation *compilation)
{
    return load_compilation_from(compilation, OM_VM_TEST_FIXTURE_ROOT);
}

static bool load_actor_compilation(Om_VM_Test_Compilation *compilation)
{
    return load_compilation_from(compilation,
                                 OM_VM_ACTOR_TEST_FIXTURE_ROOT);
}

static bool load_system_actor_compilation(
    Om_VM_Test_Compilation *compilation)
{
    return load_compilation_from(compilation,
                                 OM_VM_SYSTEM_ACTOR_TEST_FIXTURE_ROOT);
}

static bool load_distribution_compilation(
    Om_VM_Test_Compilation *compilation)
{
    return load_compilation_from(
        compilation, OM_VM_DISTRIBUTION_TEST_FIXTURE_ROOT);
}

static bool execute_named_with_limits(Om_VM_Test_Compilation *compilation,
                                      Om_VM *vm, Om_String_View name,
                                      Om_VM_Limits limits,
                                      Om_VM_Execution_Result *result)
{
    uint32_t function_index = UINT32_MAX;
    Om_Status status = om_vm_find_function(
        &compilation->emitted.image, name, &function_index);
    if (status == OM_STATUS_OK) {
        status = om_vm_init(vm, &compilation->emitted.image, limits);
    }
    if (status == OM_STATUS_OK) {
        status = om_vm_execute(vm, function_index, NULL, 0, result);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "VM execute %.*s failed: %s\n",
                      (int)name.length, name.data, om_status_name(status));
    }
    return status == OM_STATUS_OK;
}

static bool execute_named(Om_VM_Test_Compilation *compilation,
                          Om_VM *vm, Om_String_View name,
                          Om_VM_Execution_Result *result)
{
    return execute_named_with_limits(compilation, vm, name,
                                     (Om_VM_Limits){0}, result);
}

bool om_test_vm_runs_compiled_program(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"run", 3},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    bool is_ok = false;
    Om_VM_Value payload = {0};
    VM_TEST_ASSERT(om_vm_value_as_result(result.value, &is_ok, &payload));
    VM_TEST_ASSERT(is_ok);
    int64_t integer = 0;
    VM_TEST_ASSERT(om_vm_value_as_int(payload, &integer) && integer == 24);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_distribution_handshake_info(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM first_vm = {0};
    Om_VM second_vm = {0};
    Om_Distribution_Node_Handshake first = {0};
    Om_Distribution_Node_Handshake second = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(om_vm_init(&first_vm, &compilation.emitted.image,
                              (Om_VM_Limits){0}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_init(&second_vm, &compilation.emitted.image,
                              (Om_VM_Limits){0}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_distribution_handshake_info(&first_vm, &first)
                   == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_distribution_handshake_info(&second_vm, &second)
                   == OM_STATUS_OK);
    VM_TEST_ASSERT(first.protocol_version == OM_DISTRIBUTION_PROTOCOL_VERSION
                   && second.protocol_version
                          == OM_DISTRIBUTION_PROTOCOL_VERSION);
    VM_TEST_ASSERT(!om_distribution_node_id_equal(first.node_id,
                                                   second.node_id));
    VM_TEST_ASSERT(first.program_fingerprint
                       == compilation.emitted.image.program_fingerprint
                   && second.program_fingerprint
                          == compilation.emitted.image.program_fingerprint);
    VM_TEST_ASSERT(first.interface_fingerprint
                       == compilation.emitted.image.interface_fingerprint
                   && second.interface_fingerprint
                          == compilation.emitted.image.interface_fingerprint);
    passed = true;
cleanup:
    om_vm_destroy(&first_vm);
    om_vm_destroy(&second_vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_node_ref_values(void)
{
    const Om_VM_Value value =
        om_vm_value_node_ref(UINT64_C(0x1020304050607080));
    uint64_t node_ref = 0;
    uint64_t actor_id = 0;
    VM_TEST_ASSERT(om_vm_value_as_node_ref(value, &node_ref));
    VM_TEST_ASSERT(node_ref == UINT64_C(0x1020304050607080));
    VM_TEST_ASSERT(!om_vm_value_as_actor_id(value, &actor_id));
    VM_TEST_ASSERT(!om_vm_value_as_actor_ref(value, &actor_id));
    return true;
cleanup:
    return false;
}

bool om_test_vm_propagates_result_error(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"propagate_error", 15},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    bool is_ok = true;
    Om_VM_Value payload = {0};
    VM_TEST_ASSERT(om_vm_value_as_result(result.value, &is_ok, &payload));
    VM_TEST_ASSERT(!is_ok);
    int64_t integer = 0;
    VM_TEST_ASSERT(om_vm_value_as_int(payload, &integer) && integer == 7);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_formats_interpolation(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"render", 6},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    Om_String_View rendered = {0};
    VM_TEST_ASSERT(om_vm_value_as_string(result.value, &rendered));
    VM_TEST_ASSERT(om_string_view_equal(rendered,
                                       (Om_String_View){"value 3", 7}));
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_reports_runtime_error(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"divide_by_zero", 14},
                                 &result));
    VM_TEST_ASSERT(!result.succeeded);
    VM_TEST_ASSERT(result.error.code
                   == OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO);
    VM_TEST_ASSERT(result.error.has_source_span);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_indirect_calls(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"indirect_call", 13},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    int64_t integer = 0;
    VM_TEST_ASSERT(om_vm_value_as_int(result.value, &integer) && integer == 21);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_float_operations(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"float_math", 10},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    double floating = 0.0;
    VM_TEST_ASSERT(om_vm_value_as_float(result.value, &floating)
                   && floating == 7.5);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_reports_integer_overflow(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"overflow_int", 12},
                                 &result));
    VM_TEST_ASSERT(!result.succeeded);
    VM_TEST_ASSERT(result.error.code
                   == OM_VM_RUNTIME_ERROR_INTEGER_OVERFLOW);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_loader_rejects_invalid_image(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_Bytecode_Instruction *instructions = NULL;
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    Om_Bytecode_Image malformed = compilation.emitted.image;
    VM_TEST_ASSERT(malformed.instruction_count > 0);
    instructions = om_alloc_array(malformed.instruction_count,
                                  sizeof(*instructions));
    VM_TEST_ASSERT(instructions != NULL);
    (void)memcpy(instructions, malformed.instructions,
                 malformed.instruction_count * sizeof(*instructions));
    instructions[0].opcode = OM_BC_OPCODE_COUNT;
    malformed.instructions = instructions;
    VM_TEST_ASSERT(om_vm_init(&vm, &malformed, (Om_VM_Limits){0})
                   == OM_STATUS_INVALID_ARGUMENT);
    VM_TEST_ASSERT(vm.owned_state == NULL);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    om_free(instructions);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_enforces_call_depth_limit(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    uint32_t function_index = UINT32_MAX;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"recursive_sum", 13}, &function_index)
        == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_init(
        &vm, &compilation.emitted.image,
        (Om_VM_Limits){.max_call_frames = 1}) == OM_STATUS_OK);
    const Om_VM_Value argument = om_vm_value_int(3);
    VM_TEST_ASSERT(om_vm_execute(&vm, function_index, &argument, 1, &result)
                   == OM_STATUS_OK);
    VM_TEST_ASSERT(!result.succeeded);
    VM_TEST_ASSERT(result.error.code
                   == OM_VM_RUNTIME_ERROR_CALL_DEPTH_LIMIT);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_unwraps_error_pattern(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"unwrap_error_pattern", 20},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    int64_t value = 0;
    VM_TEST_ASSERT(om_vm_value_as_int(result.value, &value) && value == 9);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_constructs_message(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"make_message", 12},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    Om_VM_Object_Kind kind = OM_VM_OBJECT_STRING;
    VM_TEST_ASSERT(om_vm_value_object_kind(result.value, &kind));
    VM_TEST_ASSERT(kind == OM_VM_OBJECT_MESSAGE);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_schedules_actors_deterministically(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"main", 4}, &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.value.tag == OM_VM_VALUE_UNIT);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    VM_TEST_ASSERT(result.messages_processed == 9);
    VM_TEST_ASSERT(result.actors_stopped == 2);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_system_actors_complete_through_messages(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    (void)remove("system_actor_test.tmp");
    VM_TEST_ASSERT(load_system_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"main", 4}, &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.value.tag == OM_VM_VALUE_UNIT);
    VM_TEST_ASSERT(result.actors_spawned == 1);
    VM_TEST_ASSERT(result.messages_processed == 5);
    VM_TEST_ASSERT(result.actors_stopped == 1);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    (void)remove("system_actor_test.tmp");
    return passed;
}

static uint64_t test_monotonic_milliseconds(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000)
        + (uint64_t)now.tv_nsec / UINT64_C(1000000);
#endif
}

typedef struct Om_VM_Distribution_Attach_Worker {
    Om_VM *vm;
    Om_Distribution_Connection *connection;
    Om_Distribution_Node_Id expected_peer;
    Om_VM_Value node_ref;
    Om_Status status;
} Om_VM_Distribution_Attach_Worker;

static void run_distribution_attach_worker(
    Om_VM_Distribution_Attach_Worker *worker)
{
    worker->status = om_vm_distribution_attach_peer(
        worker->vm, worker->connection, &worker->expected_peer,
        &worker->node_ref);
}

#if defined(_WIN32)
static DWORD WINAPI distribution_attach_worker_entry(LPVOID argument)
{
    run_distribution_attach_worker(
        (Om_VM_Distribution_Attach_Worker *)argument);
    return 0;
}
#else
static void *distribution_attach_worker_entry(void *argument)
{
    run_distribution_attach_worker(
        (Om_VM_Distribution_Attach_Worker *)argument);
    return NULL;
}
#endif

static uint32_t distribution_schema_index(
    const Om_VM_Test_Compilation *compilation,
    const char *name)
{
    if (compilation == NULL || name == NULL) {
        return UINT32_MAX;
    }
    const size_t name_length = strlen(name);
    for (size_t index = 0;
         index < compilation->emitted.image.schema_count; ++index) {
        const Om_Bytecode_Schema *schema =
            &compilation->emitted.image.schemas[index];
        if (schema->kind != OM_BYTECODE_SCHEMA_MESSAGE) {
            continue;
        }
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            &compilation->resolution, schema->symbol_id);
        if (symbol != NULL && symbol->name.length == name_length
            && memcmp(symbol->name.data, name, name_length) == 0) {
            return (uint32_t)index;
        }
    }
    return UINT32_MAX;
}

static Om_Status post_distribution_watch_message(
    Om_VM_State *state,
    const Om_VM_Test_Compilation *compilation,
    Om_VM_Value remote_actor_ref,
    uint64_t watcher_actor_id)
{
    const uint32_t schema_index = distribution_schema_index(
        compilation, "Begin_Watch");
    if (state == NULL || schema_index == UINT32_MAX
        || remote_actor_ref.tag != OM_VM_VALUE_ACTOR_REF
        || watcher_actor_id == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Message_Node *node =
        om_vm_distribution_message_node_create(state);
    if (node == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_VM_Value fields[] = {remote_actor_ref};
    Om_Status status = om_vm_heap_make_record(
        &node->transit_heap, OM_VM_OBJECT_MESSAGE, schema_index,
        fields, 1, &node->message);
    if (status == OM_STATUS_OK) {
        status = om_vm_distribution_enqueue_message(
            state, watcher_actor_id, node);
    }
    if (status != OM_STATUS_OK) {
        om_vm_distribution_message_node_destroy(node);
    }
    return status;
}

bool om_test_vm_distribution_routes_actor_messages(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM first_vm = {0};
    Om_VM second_vm = {0};
    Om_VM_Execution_Result result = {0};
    Om_Distribution_Listener listener = {0};
    Om_Distribution_Connection client = {0};
    Om_Distribution_Connection server = {0};
    Om_Distribution_Node_Handshake first_handshake = {0};
    Om_Distribution_Node_Handshake second_handshake = {0};
    Om_VM_Value first_node_ref = {.tag = OM_VM_VALUE_UNINITIALIZED};
    Om_VM_Value remote_actor_ref = {.tag = OM_VM_VALUE_UNINITIALIZED};
    Om_VM_Distribution_Attach_Worker worker = {0};
    bool worker_started = false;
    bool passed = false;
#if defined(_WIN32)
    HANDLE worker_thread = NULL;
#else
    pthread_t worker_thread;
#endif

    VM_TEST_ASSERT(load_distribution_compilation(&compilation));
    VM_TEST_ASSERT(om_vm_init(&first_vm, &compilation.emitted.image,
                              (Om_VM_Limits){0}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_init(&second_vm, &compilation.emitted.image,
                              (Om_VM_Limits){0}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_distribution_handshake_info(
                       &first_vm, &first_handshake) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_distribution_handshake_info(
                       &second_vm, &second_handshake) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_distribution_listener_open(
        &listener, (Om_String_View){"127.0.0.1", 9}, 0, 4)
        == OM_STATUS_OK);
    const uint16_t port = om_distribution_listener_port(&listener);
    VM_TEST_ASSERT(port != 0);
    VM_TEST_ASSERT(om_distribution_connection_connect(
        &client, (Om_String_View){"127.0.0.1", 9}, port) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_distribution_listener_accept(
        &listener, &server) == OM_STATUS_OK);

    worker = (Om_VM_Distribution_Attach_Worker){
        .vm = &second_vm,
        .connection = &server,
        .expected_peer = first_handshake.node_id,
        .status = OM_STATUS_IO_ERROR
    };
#if defined(_WIN32)
    worker_thread = CreateThread(NULL, 0,
        distribution_attach_worker_entry, &worker, 0, NULL);
    VM_TEST_ASSERT(worker_thread != NULL);
#else
    VM_TEST_ASSERT(pthread_create(
        &worker_thread, NULL, distribution_attach_worker_entry,
        &worker) == 0);
#endif
    worker_started = true;
    VM_TEST_ASSERT(om_vm_distribution_attach_peer(
        &first_vm, &client, &second_handshake.node_id,
        &first_node_ref) == OM_STATUS_OK);
#if defined(_WIN32)
    VM_TEST_ASSERT(WaitForSingleObject(worker_thread, INFINITE)
                   == WAIT_OBJECT_0);
    (void)CloseHandle(worker_thread);
#else
    VM_TEST_ASSERT(pthread_join(worker_thread, NULL) == 0);
#endif
    worker_started = false;
    VM_TEST_ASSERT(worker.status == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_value_as_node_ref(first_node_ref,
                                           &(uint64_t){0}));
    VM_TEST_ASSERT(om_vm_value_as_node_ref(worker.node_ref,
                                           &(uint64_t){0}));

    uint32_t function_index = UINT32_MAX;
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"prepare_remote_watcher", 22},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.actors_spawned == 1);

    const Om_VM_Value spawn_arguments[] = {first_node_ref};
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"spawn_remote", 12},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, spawn_arguments,
        sizeof(spawn_arguments) / sizeof(spawn_arguments[0]),
        &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded);

    VM_TEST_ASSERT(om_vm_distribution_actor_ref(
        &first_vm, first_node_ref, 1, &remote_actor_ref) == OM_STATUS_OK);
    Om_VM_State *first_state = (Om_VM_State *)first_vm.owned_state;
    VM_TEST_ASSERT(post_distribution_watch_message(
        first_state, &compilation, remote_actor_ref, 1) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"pump_remote", 11},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.messages_processed == 1);
    Om_VM_State *second_state = (Om_VM_State *)second_vm.owned_state;
    bool watch_arrived = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        om_vm_scheduler_lock(second_state);
        watch_arrived = second_state->actors[0].remote_watcher_count == 1;
        om_vm_scheduler_unlock(second_state);
        if (watch_arrived) {
            break;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
#endif
    }
    VM_TEST_ASSERT(watch_arrived);

    bool message_arrived = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        om_vm_scheduler_lock(second_state);
        message_arrived = second_state->mailbox_message_count == 1;
        om_vm_scheduler_unlock(second_state);
        if (message_arrived) {
            break;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
#endif
    }
    VM_TEST_ASSERT(message_arrived);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"pump_remote", 11},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &second_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.messages_processed == 1
                   && result.actors_stopped == 1);
    if (!second_state->actors[0].stopped
        || second_state->actors[0].failed) {
        (void)fprintf(stderr,
            "distribution sink state: id=%llu stopped=%d failed=%d down=%u\n",
            (unsigned long long)second_state->actors[0].id,
            second_state->actors[0].stopped, second_state->actors[0].failed,
            second_state->actors[0].down_reason);
        VM_TEST_ASSERT(false);
    }

    bool down_arrived = false;
    Om_VM_State *first_state_shared = (Om_VM_State *)first_vm.owned_state;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        om_vm_scheduler_lock(first_state_shared);
        down_arrived = first_state_shared->mailbox_message_count == 1;
        om_vm_scheduler_unlock(first_state_shared);
        if (down_arrived) {
            break;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
#endif
    }
    VM_TEST_ASSERT(down_arrived);
    const Om_VM_Message_Node *down_node =
        first_state_shared->actors[0].mailbox_head;
    VM_TEST_ASSERT(down_node != NULL
                   && down_node->message.tag == OM_VM_VALUE_OBJECT
                   && down_node->message.as.object->kind
                       == OM_VM_OBJECT_MESSAGE);
    const Om_VM_Record_Object *down_message =
        (const Om_VM_Record_Object *)down_node->message.as.object;
    VM_TEST_ASSERT(down_message->field_count == 2
                   && down_message->fields[0].tag == OM_VM_VALUE_ACTOR_ID
                   && down_message->fields[0].as.actor_id == 1
                   && down_message->fields[1].tag
                       == OM_VM_VALUE_DOWN_REASON
                   && down_message->fields[1].as.down_reason == 0);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.messages_processed == 1
                   && result.actors_stopped == 1);
    VM_TEST_ASSERT(first_state_shared->actors[0].stopped
                   && !first_state_shared->actors[0].failed);

    const size_t original_max_actors = second_state->limits.max_actors;
    second_state->limits.max_actors = 1;
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"spawn_remote", 12},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, spawn_arguments,
        sizeof(spawn_arguments) / sizeof(spawn_arguments[0]),
        &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(!result.succeeded
                   && result.error.code == OM_VM_RUNTIME_ERROR_ACTOR_LIMIT);
    second_state->limits.max_actors = original_max_actors;

    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"spawn_remote_idle", 17},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, spawn_arguments,
        sizeof(spawn_arguments) / sizeof(spawn_arguments[0]),
        &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"pump_remote", 11},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &second_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && second_state->actor_count == 2);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"prepare_remote_watcher", 22},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.actors_spawned == 1);
    const uint64_t second_watcher_actor_id = atomic_load_explicit(
        &first_state_shared->actor_count, memory_order_acquire);
    Om_VM_Value second_remote_actor_ref = {.tag = OM_VM_VALUE_UNINITIALIZED};
    VM_TEST_ASSERT(om_vm_distribution_actor_ref(
        &first_vm, first_node_ref, 2,
        &second_remote_actor_ref) == OM_STATUS_OK);
    VM_TEST_ASSERT(post_distribution_watch_message(
        first_state_shared, &compilation, second_remote_actor_ref,
        second_watcher_actor_id) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"pump_remote", 11},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.messages_processed == 1);
    bool second_watch_arrived = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        om_vm_scheduler_lock(second_state);
        second_watch_arrived =
            second_state->actors[1].remote_watcher_count == 1;
        om_vm_scheduler_unlock(second_state);
        if (second_watch_arrived) {
            break;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
#endif
    }
    VM_TEST_ASSERT(second_watch_arrived);
    om_vm_distribution_shutdown(second_state);
    down_arrived = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        om_vm_scheduler_lock(first_state_shared);
        down_arrived = first_state_shared->mailbox_message_count == 1;
        om_vm_scheduler_unlock(first_state_shared);
        if (down_arrived) {
            break;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
#endif
    }
    VM_TEST_ASSERT(down_arrived);
    const Om_VM_Message_Node *unreachable_node =
        first_state_shared->actors[(size_t)(second_watcher_actor_id - 1)]
            .mailbox_head;
    VM_TEST_ASSERT(unreachable_node != NULL
                   && unreachable_node->message.tag == OM_VM_VALUE_OBJECT
                   && unreachable_node->message.as.object->kind
                       == OM_VM_OBJECT_MESSAGE);
    const Om_VM_Record_Object *unreachable_message =
        (const Om_VM_Record_Object *)unreachable_node->message.as.object;
    VM_TEST_ASSERT(unreachable_message->field_count == 2
                   && unreachable_message->fields[0].tag
                       == OM_VM_VALUE_ACTOR_ID
                   && unreachable_message->fields[0].as.actor_id == 2
                   && unreachable_message->fields[1].tag
                       == OM_VM_VALUE_DOWN_REASON
                   && unreachable_message->fields[1].as.down_reason == 2);
    VM_TEST_ASSERT(om_vm_execute(
        &first_vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.messages_processed == 1
                   && result.actors_stopped == 1);
    VM_TEST_ASSERT(first_state_shared->actors[
                       (size_t)(second_watcher_actor_id - 1)].stopped
                   && !first_state_shared->actors[
                       (size_t)(second_watcher_actor_id - 1)].failed);
    passed = true;

cleanup:
    if (worker_started) {
        om_distribution_connection_interrupt(&client);
        om_distribution_connection_interrupt(&server);
#if defined(_WIN32)
        (void)WaitForSingleObject(worker_thread, INFINITE);
        (void)CloseHandle(worker_thread);
#else
        (void)pthread_join(worker_thread, NULL);
#endif
    }
    om_distribution_listener_close(&listener);
    om_distribution_connection_close(&client);
    om_distribution_connection_close(&server);
    om_vm_destroy(&first_vm);
    om_vm_destroy(&second_vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_failure_cancels_pending_timer(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_system_actor_compilation(&compilation));
    const uint64_t start = test_monotonic_milliseconds();
    VM_TEST_ASSERT(execute_named(
        &compilation, &vm,
        (Om_String_View){"fail_while_timer_is_pending", 27}, &result));
    const uint64_t elapsed = test_monotonic_milliseconds() - start;
    VM_TEST_ASSERT(!result.succeeded);
    VM_TEST_ASSERT(result.error.code == OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO);
    VM_TEST_ASSERT(elapsed < UINT64_C(2000));
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_stop_discards_queued_messages(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(&compilation, &vm,
                                 (Om_String_View){"stop_drops_mailbox", 18},
                                 &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    VM_TEST_ASSERT(result.messages_processed == 2);
    VM_TEST_ASSERT(result.actors_stopped == 1);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_actor_state_isolation(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(
        &compilation, &vm,
        (Om_String_View){"actors_have_isolated_state", 26}, &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 3);
    VM_TEST_ASSERT(result.messages_processed == 4);
    VM_TEST_ASSERT(result.actors_stopped == 3);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_enforces_actor_limit(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named_with_limits(
        &compilation, &vm, (Om_String_View){"main", 4},
        (Om_VM_Limits){.max_actors = 1}, &result));
    VM_TEST_ASSERT(!result.succeeded);
    VM_TEST_ASSERT(result.error.code == OM_VM_RUNTIME_ERROR_ACTOR_LIMIT);
    VM_TEST_ASSERT(result.actors_spawned == 1);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_enforces_mailbox_limit(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named_with_limits(
        &compilation, &vm, (Om_String_View){"main", 4},
        (Om_VM_Limits){.max_mailbox_messages = 1}, &result));
    VM_TEST_ASSERT(!result.succeeded);
    VM_TEST_ASSERT(result.error.code == OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_reports_failed_actor_to_watchers(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(
        &compilation, &vm,
        (Om_String_View){"failed_actor_notifies_watchers", 30}, &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    VM_TEST_ASSERT(result.messages_processed == 3);
    VM_TEST_ASSERT(result.actors_stopped == 2);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_watches_actor_that_already_stopped(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named(
        &compilation, &vm,
        (Om_String_View){"watch_stopped_actor", 19}, &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    VM_TEST_ASSERT(result.messages_processed == 5);
    VM_TEST_ASSERT(result.actors_stopped == 2);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_actor_gc_stress(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named_with_limits(
        &compilation, &vm, (Om_String_View){"main", 4},
        (Om_VM_Limits){.gc_threshold_bytes = 1}, &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    VM_TEST_ASSERT(result.messages_processed == 9);
    VM_TEST_ASSERT(result.actors_stopped == 2);
    Om_VM_State *state = (Om_VM_State *)vm.owned_state;
    VM_TEST_ASSERT(state->actor_count == 2);
    VM_TEST_ASSERT(state->actors[0].heap == NULL);
    VM_TEST_ASSERT(state->actors[1].heap == NULL);
    VM_TEST_ASSERT(state->gc_collection_count > 0);
    VM_TEST_ASSERT(state->heap_bytes_allocated == state->heap.bytes_allocated);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_actor_instruction_quantum_fairness(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named_with_limits(
        &compilation, &vm, (Om_String_View){"fairness_quantum", 16},
        (Om_VM_Limits){.worker_count = 1, .instruction_quantum = 1},
        &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 3);
    VM_TEST_ASSERT(result.messages_processed == 4);
    Om_VM_State *state = (Om_VM_State *)vm.owned_state;
    VM_TEST_ASSERT(state->actor_count == 3);
    VM_TEST_ASSERT(state->actors[2].state_count == 2);
    VM_TEST_ASSERT(state->actors[2].state_values[0].tag == OM_VM_VALUE_BOOL);
    VM_TEST_ASSERT(state->actors[2].state_values[0].as.boolean);
    VM_TEST_ASSERT(state->actors[2].state_values[1].tag == OM_VM_VALUE_BOOL);
    VM_TEST_ASSERT(state->actors[2].state_values[1].as.boolean);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_actor_message_stress_concurrency(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    bool passed = false;
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    VM_TEST_ASSERT(execute_named_with_limits(
        &compilation, &vm, (Om_String_View){"message_pool_stress", 19},
        (Om_VM_Limits){
            .max_instructions = UINT64_C(100000000),
            .max_mailbox_messages = 64,
            .worker_count = 4,
            .instruction_quantum = 32
        },
        &result));
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.actors_spawned == 3);
    VM_TEST_ASSERT(result.messages_processed == UINT64_C(2000002));
    VM_TEST_ASSERT(result.actors_stopped == 2);
    Om_VM_State *state = (Om_VM_State *)vm.owned_state;
    VM_TEST_ASSERT(state->actor_count == 3);
    VM_TEST_ASSERT(state->actors[2].state_count == 2);
    VM_TEST_ASSERT(state->actors[2].state_values[0].tag == OM_VM_VALUE_INT);
    VM_TEST_ASSERT(state->actors[2].state_values[0].as.integer == 2000000);
    VM_TEST_ASSERT(state->actors[2].state_values[1].tag == OM_VM_VALUE_INT);
    VM_TEST_ASSERT(state->actors[2].state_values[1].as.integer == 2);
    VM_TEST_ASSERT(state->max_running_actor_count >= 2);
    passed = true;
cleanup:
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

typedef struct Om_VM_Native_Callback_Thread_Context {
    Om_VM *vm;
    Om_Symbol_Id message_symbol;
    _Atomic bool stop_requested;
    Om_Status status;
} Om_VM_Native_Callback_Thread_Context;

static void native_callback_sender_run(
    Om_VM_Native_Callback_Thread_Context *context)
{
    const Om_VM_Value fields[] = {om_vm_value_int(42)};
    context->status = OM_STATUS_CANCELLED;
    for (size_t attempt = 0; attempt < 5000
         && !atomic_load_explicit(&context->stop_requested,
                                  memory_order_acquire);
         ++attempt) {
        const Om_Status status = om_vm_native_callback_post(
            context->vm, 1, context->message_symbol, fields, 1);
        if (status == OM_STATUS_OK) {
            context->status = status;
            return;
        }
        if (status != OM_STATUS_NOT_FOUND && status != OM_STATUS_CANCELLED) {
            context->status = status;
            return;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
#endif
    }
}

#if defined(_WIN32)
static DWORD WINAPI native_callback_sender_entry(LPVOID argument)
{
    native_callback_sender_run(
        (Om_VM_Native_Callback_Thread_Context *)argument);
    return 0;
}
#else
static void *native_callback_sender_entry(void *argument)
{
    native_callback_sender_run(
        (Om_VM_Native_Callback_Thread_Context *)argument);
    return NULL;
}
#endif

static Om_Symbol_Id find_message_symbol(
    const Om_Resolution_Result *resolution,
    Om_String_View message_name)
{
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &resolution->symbols[index];
        if (symbol->kind == OM_SYMBOL_MESSAGE
            && om_string_view_equal(symbol->name, message_name)) {
            return symbol->id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Symbol_Id find_native_prelude_symbol(
    const Om_Resolution_Result *resolution, Om_String_View name)
{
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &resolution->symbols[index];
        if (symbol->kind == OM_SYMBOL_BUILTIN
            && symbol->native_module != NULL
            && om_string_view_equal(symbol->name, name)) {
            return symbol->id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Symbol_Id find_symbol_by_kind_and_name(
    const Om_Resolution_Result *resolution, Om_Symbol_Kind kind,
    Om_String_View name)
{
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &resolution->symbols[index];
        if (symbol->kind == kind && om_string_view_equal(symbol->name, name)) {
            return symbol->id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Ast_Node_Id find_named_call_in_tree(
    const Om_Ast *ast, Om_Ast_Node_Id node_id, Om_String_View name,
    size_t depth)
{
    if (ast == NULL || depth >= 256) {
        return OM_AST_NODE_ID_INVALID;
    }
    const Om_Ast_Node *node = om_ast_get_node(ast, node_id);
    if (node == NULL) {
        return OM_AST_NODE_ID_INVALID;
    }
    if (node->kind == OM_AST_CALL && node->child_count != 0) {
        const Om_Ast_Node_Id callee_id = om_ast_get_child(ast, node, 0);
        const Om_Ast_Node *callee = om_ast_get_node(ast, callee_id);
        if (callee != NULL && callee->kind == OM_AST_NAME
            && om_string_view_equal(callee->name, name)) {
            return node_id;
        }
    }
    for (size_t index = 0; index < node->child_count; ++index) {
        const Om_Ast_Node_Id found = find_named_call_in_tree(
            ast, om_ast_get_child(ast, node, index), name, depth + 1);
        if (found.value != UINT32_MAX) {
            return found;
        }
    }
    return OM_AST_NODE_ID_INVALID;
}

bool om_test_vm_native_callback_enters_actor_mailbox(void)
{
    Om_VM_Test_Compilation compilation = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    Om_VM_Native_Callback_Thread_Context callback_context = {0};
    bool callback_thread_started = false;
    bool passed = false;
#if defined(_WIN32)
    HANDLE callback_thread = NULL;
#else
    pthread_t callback_thread;
#endif
    VM_TEST_ASSERT(load_actor_compilation(&compilation));
    callback_context.vm = &vm;
    callback_context.message_symbol = find_message_symbol(
        &compilation.resolution, (Om_String_View){"Runtime_Callback", 16});
    VM_TEST_ASSERT(callback_context.message_symbol.value != UINT32_MAX);
    atomic_init(&callback_context.stop_requested, false);

    uint32_t function_index = UINT32_MAX;
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image,
        (Om_String_View){"native_callback_ingress", 23},
        &function_index) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_init(
        &vm, &compilation.emitted.image,
        (Om_VM_Limits){
            .max_instructions = UINT64_C(10000000),
            .worker_count = 1,
            .instruction_quantum = 64
        }) == OM_STATUS_OK);
#if defined(_WIN32)
    callback_thread = CreateThread(
        NULL, 0, native_callback_sender_entry, &callback_context, 0, NULL);
    VM_TEST_ASSERT(callback_thread != NULL);
#else
    VM_TEST_ASSERT(pthread_create(
        &callback_thread, NULL, native_callback_sender_entry,
        &callback_context) == 0);
#endif
    callback_thread_started = true;
    VM_TEST_ASSERT(om_vm_execute(
        &vm, function_index, NULL, 0, &result) == OM_STATUS_OK);
#if defined(_WIN32)
    (void)WaitForSingleObject(callback_thread, INFINITE);
    (void)CloseHandle(callback_thread);
    callback_thread = NULL;
#else
    (void)pthread_join(callback_thread, NULL);
#endif
    callback_thread_started = false;
    VM_TEST_ASSERT(callback_context.status == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded);
    VM_TEST_ASSERT(result.value.tag == OM_VM_VALUE_UNIT);
    VM_TEST_ASSERT(result.actors_spawned == 1);
    VM_TEST_ASSERT(result.messages_processed == 2);
    VM_TEST_ASSERT(result.actors_stopped == 1);
    passed = true;
cleanup:
    atomic_store_explicit(&callback_context.stop_requested, true,
                          memory_order_release);
    if (callback_thread_started) {
#if defined(_WIN32)
        (void)WaitForSingleObject(callback_thread, INFINITE);
        (void)CloseHandle(callback_thread);
#else
        (void)pthread_join(callback_thread, NULL);
#endif
    }
    om_vm_destroy(&vm);
    destroy_compilation(&compilation);
    return passed;
}

static Om_Status add_native_test_external(Om_Bytecode_Builder *builder,
                                          Om_String_View name,
                                          uint32_t parameter_count,
                                          uint32_t *function_index)
{
    uint32_t name_constant = 0;
    Om_Status status = om_bytecode_builder_add_constant(
        builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_STRING,
            .as.string_value = name
        }, &name_constant);
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_add_external_function(
            builder, name_constant, parameter_count,
            OM_BYTECODE_FUNCTION_NONE, function_index);
    }
    return status;
}

static Om_Status add_native_test_entry(
    Om_Bytecode_Builder *builder, Om_String_View name,
    uint32_t target_function, const uint32_t *argument_constants,
    size_t argument_count, Om_Source_Span span, uint32_t *function_index)
{
    if (argument_count > UINT32_MAX
        || (argument_count != 0 && argument_constants == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t name_constant = 0;
    Om_Status status = om_bytecode_builder_add_constant(
        builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_STRING,
            .as.string_value = name
        }, &name_constant);
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_begin_function(
            builder, name_constant, 0, OM_BYTECODE_FUNCTION_NONE,
            function_index);
    }
    for (size_t index = 0;
         status == OM_STATUS_OK && index < argument_count; ++index) {
        status = om_bytecode_builder_emit(
            builder, OM_BC_LOAD_CONST, argument_constants[index], 0, 0,
            span, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_emit(
            builder, OM_BC_CALL, target_function,
            (uint32_t)argument_count, 0, span, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_emit(
            builder, OM_BC_RETURN, 0, 0, 0, span, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_end_function(builder, *function_index);
    }
    return status;
}

bool om_test_vm_calls_omi_native_functions(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    static const char metadata[] =
        "NATIVE \"native_fixture\"\n"
        "FUNCTION add(left: CInt, right: CInt): CInt\n"
        "    SYMBOL \"om_ffi_add\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION mixed(left: CInt, scale: CDouble, right: CInt): CDouble\n"
        "    SYMBOL \"om_ffi_mixed\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION utf8_length(value: CString BORROWED): CSize\n"
        "    SYMBOL \"om_ffi_utf8_length\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION echo_string(value: CString BORROWED): CString\n"
        "    SYMBOL \"om_ffi_echo_string\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "    RETURNS BORROWED FROM value\n"
        "END FUNCTION\n"
        "FUNCTION create_name(): CString\n"
        "    SYMBOL \"native_create_name\"\n"
        "    EFFECT NATIVE\n"
        "    BLOCKING\n"
        "    RETURNS OWNED WITH release_name\n"
        "END FUNCTION\n"
        "FUNCTION release_name(value: CPointer OWNED): CVoid\n"
        "    SYMBOL \"native_release_name\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    static const char fixture_path[] = OM_NATIVE_FIXTURE_PATH;
    static const char string_argument[] = "name_\xc3\xa9";
    static const char source_path[] = "native_vm_test.omi";
    static const Om_Source_Span span = {{99}, 0, 1};
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    Om_Native_Library library = {0};
    Om_Native_Call_Binding bindings[5] = {{0}};
    Om_Bytecode_Builder builder = {0};
    Om_Bytecode_Image image = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    uint32_t external_functions[5] = {0};
    uint32_t entry_functions[5] = {0};
    uint32_t left_constant = 0;
    uint32_t right_constant = 0;
    uint32_t mixed_left_constant = 0;
    uint32_t mixed_scale_constant = 0;
    uint32_t mixed_right_constant = 0;
    uint32_t string_constant = 0;
    uint32_t add_args[] = {0, 0};
    uint32_t mixed_args[] = {0, 0, 0};
    uint32_t string_arg[] = {0};
    bool passed = false;

    VM_TEST_ASSERT(om_source_text_init(
        &source, (Om_Source_Id){99},
        (Om_String_View){source_path, sizeof(source_path) - 1},
        (Om_Const_Byte_Slice){(const uint8_t *)metadata,
                              sizeof(metadata) - 1}, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_omi_parse(&source, &module) == OM_STATUS_OK);
    VM_TEST_ASSERT(module.diagnostic_count == 0);
    VM_TEST_ASSERT(om_native_library_open(
        &library, (Om_String_View){fixture_path,
                                   sizeof(fixture_path) - 1})
        == OM_STATUS_OK);
    VM_TEST_ASSERT(om_bytecode_builder_init(&builder) == OM_STATUS_OK);
    for (size_t index = 0; index < 5; ++index) {
        const Om_Omi_Native_Function *function = &module.functions[index];
        VM_TEST_ASSERT(om_native_call_binding_init(
            &bindings[index], &library, &module, index, 0) == OM_STATUS_OK);
        VM_TEST_ASSERT(add_native_test_external(
            &builder, function->name, (uint32_t)function->parameter_count,
            &external_functions[index]) == OM_STATUS_OK);
    }
    VM_TEST_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT,
            .as.integer_value = 19
        }, &left_constant) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT,
            .as.integer_value = 23
        }, &right_constant) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT,
            .as.integer_value = 2
        }, &mixed_left_constant) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_FLOAT,
            .as.float_value = 1.5
        }, &mixed_scale_constant) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT,
            .as.integer_value = 4
        }, &mixed_right_constant) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_STRING,
            .as.string_value = {string_argument,
                                sizeof(string_argument) - 1}
        }, &string_constant) == OM_STATUS_OK);
    add_args[0] = left_constant;
    add_args[1] = right_constant;
    mixed_args[0] = mixed_left_constant;
    mixed_args[1] = mixed_scale_constant;
    mixed_args[2] = mixed_right_constant;
    string_arg[0] = string_constant;
    VM_TEST_ASSERT(add_native_test_entry(
        &builder, (Om_String_View){"main_add", 8}, external_functions[0],
        add_args, 2, span, &entry_functions[0]) == OM_STATUS_OK);
    VM_TEST_ASSERT(add_native_test_entry(
        &builder, (Om_String_View){"main_mixed", 10}, external_functions[1],
        mixed_args, 3, span, &entry_functions[1]) == OM_STATUS_OK);
    VM_TEST_ASSERT(add_native_test_entry(
        &builder, (Om_String_View){"main_length", 11},
        external_functions[2], string_arg, 1, span,
        &entry_functions[2]) == OM_STATUS_OK);
    VM_TEST_ASSERT(add_native_test_entry(
        &builder, (Om_String_View){"main_echo", 9}, external_functions[3],
        string_arg, 1, span, &entry_functions[3]) == OM_STATUS_OK);
    VM_TEST_ASSERT(add_native_test_entry(
        &builder, (Om_String_View){"main_owned", 10}, external_functions[4],
        NULL, 0, span, &entry_functions[4]) == OM_STATUS_OK);
    Om_Bytecode_Verify_Result verify = {0};
    VM_TEST_ASSERT(om_bytecode_builder_finish(
        &builder, &image, &verify) == OM_STATUS_OK && verify.valid);
    VM_TEST_ASSERT(om_vm_init(&vm, &image, (Om_VM_Limits){0})
                   == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_functions[0], NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(!result.succeeded
                   && result.error.code == OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION);
    VM_TEST_ASSERT(om_vm_bind_native_function(
        &vm, entry_functions[0], &bindings[0]) == OM_STATUS_INVALID_ARGUMENT);
    VM_TEST_ASSERT(om_vm_bind_native_function(
        &vm, external_functions[1], &bindings[0])
        == OM_STATUS_INVALID_ARGUMENT);
    for (size_t index = 0; index < 5; ++index) {
        VM_TEST_ASSERT(om_vm_bind_native_function(
            &vm, external_functions[index], &bindings[index])
            == OM_STATUS_OK);
    }

    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_functions[0], NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.value.tag == OM_VM_VALUE_INT
                   && result.value.as.integer == 42);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_functions[1], NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.value.tag == OM_VM_VALUE_FLOAT
                   && result.value.as.floating == 8.0);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_functions[2], NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.value.tag == OM_VM_VALUE_INT
                   && result.value.as.integer
                       == (int64_t)(sizeof(string_argument) - 1));
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_functions[3], NULL, 0, &result) == OM_STATUS_OK);
    Om_String_View returned_string = {0};
    VM_TEST_ASSERT(result.succeeded
                   && om_vm_value_as_string(result.value, &returned_string));
    VM_TEST_ASSERT(returned_string.length == sizeof(string_argument) - 1
                   && memcmp(returned_string.data, string_argument,
                             returned_string.length) == 0);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_functions[4], NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded
                   && om_vm_value_as_string(result.value, &returned_string));
    VM_TEST_ASSERT(returned_string.length == strlen("native-name")
                   && memcmp(returned_string.data, "native-name",
                             returned_string.length) == 0);
    passed = true;

cleanup:
    om_vm_destroy(&vm);
    om_bytecode_image_destroy(&image);
    om_bytecode_builder_destroy(&builder);
    for (size_t index = 0; index < 5; ++index) {
        om_native_call_binding_destroy(&bindings[index]);
    }
    om_native_library_close(&library);
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_vm_compiles_and_calls_omi_native_function(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    static const char fixture_path[] = OM_NATIVE_FIXTURE_PATH;
    static const char native_path[] = "native_add.omi";
    Om_Source_Root native_root = {0};
    Om_Omi_Project native_project = {0};
    const Om_Omi_Module *module = NULL;
    Om_Resolver_Config resolver_config = {0};
    Om_VM_Test_Compilation compilation = {0};
    Om_Native_Library library = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    uint32_t entry_function = UINT32_MAX;
    uint32_t native_function = UINT32_MAX;
    Om_Symbol_Id native_symbol = OM_SYMBOL_ID_INVALID;
    Om_Symbol_Id calculate_symbol = OM_SYMBOL_ID_INVALID;
    bool passed = false;

    VM_TEST_ASSERT(om_source_root_init(
        &native_root,
        (Om_String_View){OM_VM_NATIVE_TEST_FIXTURE_ROOT,
                         sizeof(OM_VM_NATIVE_TEST_FIXTURE_ROOT) - 1})
        == OM_STATUS_OK);
    VM_TEST_ASSERT(om_omi_project_load(
        &native_root, &(Om_String_View){native_path,
                                        sizeof(native_path) - 1},
        1, 2000, 0, &native_project) == OM_STATUS_OK);
    module = om_omi_project_module(&native_project, 0);
    VM_TEST_ASSERT(module != NULL && module->diagnostic_count == 0);
    resolver_config.native_modules = native_project.modules;
    resolver_config.native_module_count = native_project.module_count;
    VM_TEST_ASSERT(om_native_library_open(
        &library, (Om_String_View){fixture_path,
                                   sizeof(fixture_path) - 1})
        == OM_STATUS_OK);
    VM_TEST_ASSERT(load_compilation_from_config(
        &compilation, OM_VM_NATIVE_TEST_FIXTURE_ROOT, &resolver_config));
    native_symbol = find_native_prelude_symbol(
        &compilation.resolution, (Om_String_View){"native_add", 10});
    VM_TEST_ASSERT(native_symbol.value != UINT32_MAX);
    const Om_Type_Scheme *native_scheme = om_type_inference_scheme_for_symbol(
        &compilation.inference, native_symbol);
    VM_TEST_ASSERT(native_scheme != NULL);
    Om_Type_Id resolved_native_type = OM_TYPE_ID_INVALID;
    Om_Type_View native_function_type = {0};
    VM_TEST_ASSERT(om_type_store_resolve(
        &compilation.inference.type_store, native_scheme->type_id,
        &resolved_native_type) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_type_store_get(
        &compilation.inference.type_store, resolved_native_type,
        &native_function_type) == OM_STATUS_OK);
    VM_TEST_ASSERT(native_function_type.kind == OM_TYPE_FUNCTION
                   && native_function_type.child_count == 3);
    for (size_t index = 0; index < native_function_type.child_count; ++index) {
        Om_Type_Id child_type = OM_TYPE_ID_INVALID;
        Om_Type_Id resolved_child_type = OM_TYPE_ID_INVALID;
        Om_Type_View child_view = {0};
        VM_TEST_ASSERT(om_type_store_child_at(
            &compilation.inference.type_store, resolved_native_type, index,
            &child_type) == OM_STATUS_OK);
        VM_TEST_ASSERT(om_type_store_resolve(
            &compilation.inference.type_store, child_type,
            &resolved_child_type) == OM_STATUS_OK);
        VM_TEST_ASSERT(om_type_store_get(
            &compilation.inference.type_store, resolved_child_type,
            &child_view) == OM_STATUS_OK);
        VM_TEST_ASSERT(child_view.kind == OM_TYPE_INT);
    }
    calculate_symbol = find_symbol_by_kind_and_name(
        &compilation.resolution, OM_SYMBOL_FUNCTION,
        (Om_String_View){"calculate", 9});
    VM_TEST_ASSERT(calculate_symbol.value != UINT32_MAX);
    VM_TEST_ASSERT(om_semantic_effect_for_symbol(
        &compilation.semantic, calculate_symbol) == OM_EFFECT_NONE);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image, (Om_String_View){"main", 4},
        &entry_function) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image, (Om_String_View){"native_add", 10},
        &native_function) == OM_STATUS_OK);
    VM_TEST_ASSERT(
        compilation.emitted.image.functions[native_function].parameter_count
            == 2);
    VM_TEST_ASSERT((compilation.emitted.image.functions[native_function].flags
                    & OM_BYTECODE_FUNCTION_VARIADIC) == 0);
    VM_TEST_ASSERT(om_vm_init(
        &vm, &compilation.emitted.image, (Om_VM_Limits){0}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_compiler_bind_native_module(
        &vm, &compilation.emitted.image, &library, module,
        &compilation.resolution, &compilation.inference, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_function, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.value.tag == OM_VM_VALUE_UNIT);
    passed = true;

cleanup:
    om_vm_destroy(&vm);
    om_native_library_close(&library);
    om_omi_project_destroy(&native_project);
    om_source_root_destroy(&native_root);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_native_callback_reaches_actor_mailbox(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    static const char metadata[] =
        "NATIVE \"native_fixture\"\n"
        "FUNCTION native_fire(callback: CALLBACK(CInt) -> CVoid): CVoid\n"
        "    SYMBOL \"om_ffi_invoke_single_callback_on_thread\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    static const char wrong_metadata[] =
        "NATIVE \"native_fixture\"\n"
        "FUNCTION native_fire(callback: CALLBACK(CDouble) -> CVoid): CVoid\n"
        "    SYMBOL \"om_ffi_invoke_single_callback_on_thread\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    static const char fixture_path[] = OM_NATIVE_FIXTURE_PATH;
    static const char source_path[] = "native_vm_callback_test.omi";
    Om_Omi_Module module = {0};
    const Om_Resolver_Config resolver_config = {
        .native_modules = &module,
        .native_module_count = 1
    };
    Om_VM_Test_Compilation compilation = {0};
    Om_Source_Text source = {0};
    Om_Source_Text wrong_source = {0};
    Om_Omi_Module wrong_module = {0};
    Om_Native_Library library = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    uint32_t entry_function = UINT32_MAX;
    uint32_t native_function = UINT32_MAX;
    Om_Symbol_Id message_symbol = OM_SYMBOL_ID_INVALID;
    Om_Ast_Node_Id native_call_node = OM_AST_NODE_ID_INVALID;
    bool passed = false;

    VM_TEST_ASSERT(om_source_text_init(
        &source, (Om_Source_Id){102},
        (Om_String_View){source_path, sizeof(source_path) - 1},
        (Om_Const_Byte_Slice){(const uint8_t *)metadata,
                              sizeof(metadata) - 1}, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_omi_parse(&source, &module) == OM_STATUS_OK);
    VM_TEST_ASSERT(module.diagnostic_count == 0);
    VM_TEST_ASSERT(om_native_library_open(
        &library, (Om_String_View){fixture_path,
                                   sizeof(fixture_path) - 1})
        == OM_STATUS_OK);
    VM_TEST_ASSERT(load_compilation_from_config(
        &compilation, OM_VM_NATIVE_CALLBACK_TEST_FIXTURE_ROOT,
        &resolver_config));
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image, (Om_String_View){"main", 4},
        &entry_function) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image, (Om_String_View){"native_fire", 11},
        &native_function) == OM_STATUS_OK);
    message_symbol = find_message_symbol(
        &compilation.resolution, (Om_String_View){"Tick", 4});
    VM_TEST_ASSERT(message_symbol.value != UINT32_MAX);
    native_call_node = find_named_call_in_tree(
        &compilation.parsed[0].ast, compilation.parsed[0].ast.root,
        (Om_String_View){"native_fire", 11}, 0);
    VM_TEST_ASSERT(native_call_node.value != UINT32_MAX);
    VM_TEST_ASSERT((om_semantic_effect_for_node(
        &compilation.semantic, (Om_Module_Id){0}, native_call_node)
        & OM_EFFECT_NATIVE) != 0);
    VM_TEST_ASSERT(om_vm_init(
        &vm, &compilation.emitted.image,
        (Om_VM_Limits){.worker_count = 1}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_source_text_init(
        &wrong_source, (Om_Source_Id){103},
        (Om_String_View){source_path, sizeof(source_path) - 1},
        (Om_Const_Byte_Slice){(const uint8_t *)wrong_metadata,
                              sizeof(wrong_metadata) - 1}, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_omi_parse(&wrong_source, &wrong_module) == OM_STATUS_OK);
    VM_TEST_ASSERT(wrong_module.diagnostic_count == 0);
    VM_TEST_ASSERT(om_compiler_bind_native_module(
        &vm, &compilation.emitted.image, &library, &wrong_module,
        &compilation.resolution, &compilation.inference, 0)
        == OM_STATUS_INVALID_ARGUMENT);
    VM_TEST_ASSERT(om_compiler_bind_native_module(
        &vm, &compilation.emitted.image, &library, &module,
        &compilation.resolution, &compilation.inference, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_function, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.value.tag == OM_VM_VALUE_UNIT);
    VM_TEST_ASSERT(result.actors_spawned == 1);
    VM_TEST_ASSERT(result.messages_processed == 2);
    VM_TEST_ASSERT(result.actors_stopped == 1);
    passed = true;

cleanup:
    om_vm_destroy(&vm);
    om_native_library_close(&library);
    om_omi_module_destroy(&wrong_module);
    om_source_text_destroy(&wrong_source);
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    destroy_compilation(&compilation);
    return passed;
}

bool om_test_vm_blocking_native_call_releases_actor_worker(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    static const char metadata[] =
        "NATIVE \"native_fixture\"\n"
        "FUNCTION native_reset_fast(): CVoid\n"
        "    SYMBOL \"om_ffi_reset_fast_actor_seen\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION native_wait_for_fast(timeout_ms: CInt): CInt\n"
        "    SYMBOL \"om_ffi_wait_for_fast_actor\"\n"
        "    EFFECT NATIVE\n"
        "    BLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION native_mark_fast(): CVoid\n"
        "    SYMBOL \"om_ffi_mark_fast_actor_seen\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    static const char fixture_path[] = OM_NATIVE_FIXTURE_PATH;
    Om_Omi_Module module = {0};
    const Om_Resolver_Config resolver_config = {
        .native_modules = &module,
        .native_module_count = 1
    };
    Om_VM_Test_Compilation compilation = {0};
    Om_Source_Text source = {0};
    Om_Native_Library library = {0};
    Om_VM vm = {0};
    Om_VM_Execution_Result result = {0};
    uint32_t entry_function = UINT32_MAX;
    bool passed = false;

    VM_TEST_ASSERT(om_source_text_init(
        &source, (Om_Source_Id){104},
        (Om_String_View){"native_blocking_test.omi", 24},
        (Om_Const_Byte_Slice){(const uint8_t *)metadata,
                              sizeof(metadata) - 1}, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_omi_parse(&source, &module) == OM_STATUS_OK);
    VM_TEST_ASSERT(module.diagnostic_count == 0);
    VM_TEST_ASSERT(om_native_library_open(
        &library, (Om_String_View){fixture_path,
                                   sizeof(fixture_path) - 1})
        == OM_STATUS_OK);
    VM_TEST_ASSERT(load_compilation_from_config(
        &compilation, OM_VM_NATIVE_BLOCKING_TEST_FIXTURE_ROOT,
        &resolver_config));
    VM_TEST_ASSERT(om_vm_find_function(
        &compilation.emitted.image, (Om_String_View){"main", 4},
        &entry_function) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_init(
        &vm, &compilation.emitted.image,
        (Om_VM_Limits){.worker_count = 1}) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_compiler_bind_native_module(
        &vm, &compilation.emitted.image, &library, &module,
        &compilation.resolution, &compilation.inference, 0) == OM_STATUS_OK);
    VM_TEST_ASSERT(om_vm_execute(
        &vm, entry_function, NULL, 0, &result) == OM_STATUS_OK);
    VM_TEST_ASSERT(result.succeeded && result.value.tag == OM_VM_VALUE_UNIT);
    VM_TEST_ASSERT(result.actors_spawned == 2);
    VM_TEST_ASSERT(result.messages_processed == 2);
    VM_TEST_ASSERT(result.actors_stopped == 2);
    passed = true;

cleanup:
    om_vm_destroy(&vm);
    om_native_library_close(&library);
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    destroy_compilation(&compilation);
    return passed;
}
