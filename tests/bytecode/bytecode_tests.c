#include "test.h"

#include <string.h>

#include "ombasic/bytecode/bytecode.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/source/source_text.h"
#include "ombasic/syntax/lexer.h"

#if !defined(OM_BYTECODE_TEST_FIXTURE_ROOT)
#error OM_BYTECODE_TEST_FIXTURE_ROOT must name the bytecode fixture directory
#endif

#define BYTECODE_ASSERT(expression)                                             \
    do {                                                                        \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",           \
                          __FILE__, __LINE__, #expression);                     \
            goto cleanup;                                                       \
        }                                                                       \
    } while (false)

typedef struct Om_Bytecode_Test_Project {
    Om_Source_Root source_root;
    Om_Module_Project project;
    Om_Lexer_Result *lexed;
    Om_Parser_Result *parsed;
    size_t module_count;
    Om_Resolution_Result resolution;
    Om_Type_Inference_Result inference;
    Om_Semantic_Analysis_Result semantic;
} Om_Bytecode_Test_Project;

static void destroy_emitter_project(Om_Bytecode_Test_Project *test_project)
{
    if (test_project == NULL) {
        return;
    }
    om_semantic_analysis_result_destroy(&test_project->semantic);
    om_type_inference_result_destroy(&test_project->inference);
    om_resolution_result_destroy(&test_project->resolution);
    for (size_t index = 0; index < test_project->module_count; ++index) {
        if (test_project->parsed != NULL) {
            om_parser_result_destroy(&test_project->parsed[index]);
        }
        if (test_project->lexed != NULL) {
            om_lexer_result_destroy(&test_project->lexed[index]);
        }
    }
    om_free(test_project->parsed);
    om_free(test_project->lexed);
    om_module_project_destroy(&test_project->project);
    om_source_root_destroy(&test_project->source_root);
    *test_project = (Om_Bytecode_Test_Project){0};
}

static bool load_emitter_project(Om_Bytecode_Test_Project *test_project)
{
    static const char fixture_root[] = OM_BYTECODE_TEST_FIXTURE_ROOT;
    Om_Status status = om_source_root_init(
        &test_project->source_root,
        (Om_String_View){fixture_root, sizeof(fixture_root) - 1});
    if (status == OM_STATUS_OK) {
        status = om_module_project_load(
            &test_project->source_root,
            (Om_String_View){"main.bas", 8}, 16384,
            &test_project->project);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "bytecode fixture project load failed: %s\n",
                      om_status_name(status));
        return false;
    }
    test_project->module_count =
        om_module_project_module_count(&test_project->project);
    test_project->lexed = om_alloc_array(test_project->module_count,
                                         sizeof(*test_project->lexed));
    test_project->parsed = om_alloc_array(test_project->module_count,
                                          sizeof(*test_project->parsed));
    if (test_project->module_count == 0 || test_project->lexed == NULL
        || test_project->parsed == NULL) {
        return false;
    }
    (void)memset(test_project->lexed, 0,
                 test_project->module_count * sizeof(*test_project->lexed));
    (void)memset(test_project->parsed, 0,
                 test_project->module_count * sizeof(*test_project->parsed));
    for (size_t index = 0; index < test_project->module_count; ++index) {
        const Om_Source_Text *source = om_module_project_source(
            &test_project->project, (Om_Module_Id){(uint32_t)index});
        if (source == NULL
            || om_lexer_scan(source, &test_project->lexed[index]) != OM_STATUS_OK
            || test_project->lexed[index].diagnostic_count != 0
            || om_parser_parse(&test_project->lexed[index],
                               &test_project->parsed[index]) != OM_STATUS_OK
            || test_project->parsed[index].diagnostic_count != 0) {
            (void)fprintf(stderr, "bytecode fixture parse failed at module %zu\n",
                          index);
            return false;
        }
    }
    status = om_name_resolve_project(
        &test_project->project, test_project->parsed, test_project->module_count,
        NULL, &test_project->resolution);
    if (status == OM_STATUS_OK
        && test_project->resolution.diagnostic_count == 0) {
        status = om_type_infer_project(
            &test_project->project, test_project->parsed,
            test_project->module_count, &test_project->resolution,
            &test_project->inference);
    }
    if (status != OM_STATUS_OK
        || test_project->resolution.diagnostic_count != 0) {
        (void)fprintf(stderr, "bytecode fixture resolution failed: %s (%zu)\n",
                      om_status_name(status),
                      test_project->resolution.diagnostic_count);
        for (size_t index = 0;
             index < test_project->resolution.diagnostic_count; ++index) {
            (void)fprintf(stderr, "  %s at %u-%u\n",
                om_resolution_diagnostic_code_name(
                    test_project->resolution.diagnostics[index].code),
                test_project->resolution.diagnostics[index].span.start,
                test_project->resolution.diagnostics[index].span.end);
        }
    }
    if (status == OM_STATUS_OK
        && test_project->inference.diagnostic_count == 0) {
        status = om_semantic_analyze_project(
            &test_project->project, test_project->parsed,
            test_project->module_count, &test_project->resolution,
            &test_project->inference, &test_project->semantic);
    }
    if (status != OM_STATUS_OK || test_project->inference.diagnostic_count != 0) {
        (void)fprintf(stderr, "bytecode fixture inference failed: %s (%zu)\n",
                      om_status_name(status),
                      test_project->inference.diagnostic_count);
    }
    if (status == OM_STATUS_OK && test_project->semantic.diagnostic_count != 0) {
        (void)fprintf(stderr, "bytecode fixture semantic failed (%zu)\n",
                      test_project->semantic.diagnostic_count);
        for (size_t index = 0;
             index < test_project->semantic.diagnostic_count; ++index) {
            const Om_Semantic_Analysis_Diagnostic *diagnostic =
                &test_project->semantic.diagnostics[index];
            (void)fprintf(stderr, "  %s at %u-%u\n",
                om_semantic_analysis_diagnostic_code_name(diagnostic->code),
                diagnostic->span.start, diagnostic->span.end);
        }
    }
    return status == OM_STATUS_OK
        && test_project->semantic.diagnostic_count == 0;
}

bool om_test_bytecode_golden_round_trip(void)
{
    Om_Bytecode_Builder builder = {0};
    Om_Bytecode_Image image = {0};
    Om_Bytecode_Image decoded = {0};
    Om_Byte_Slice bytes = {0};
    bool passed = false;
    BYTECODE_ASSERT(om_bytecode_builder_init(&builder) == OM_STATUS_OK);
    uint32_t name_constant = 0;
    BYTECODE_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_STRING,
            .as.string_value = {"main", 4}
        }, &name_constant) == OM_STATUS_OK);
    uint32_t value_constant = 0;
    BYTECODE_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT,
            .as.integer_value = 42
        }, &value_constant) == OM_STATUS_OK);
    uint32_t function_index = 0;
    BYTECODE_ASSERT(om_bytecode_builder_begin_function(
        &builder, name_constant, 0, OM_BYTECODE_FUNCTION_NONE,
        &function_index) == OM_STATUS_OK);
    const Om_Source_Span span = {{0}, 3, 9};
    BYTECODE_ASSERT(om_bytecode_builder_emit(
        &builder, OM_BC_LOAD_CONST, value_constant, 0, 0, span, NULL)
        == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_emit(
        &builder, OM_BC_RETURN, 0, 0, 0, span, NULL) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_end_function(
        &builder, function_index) == OM_STATUS_OK);
    Om_Bytecode_Verify_Result verify = {0};
    BYTECODE_ASSERT(om_bytecode_builder_finish(
        &builder, &image, &verify) == OM_STATUS_OK && verify.valid);
    BYTECODE_ASSERT(image.function_count == 1 && image.instruction_count == 2
                    && image.functions[0].max_stack == 1
                    && image.source_map_count == 1
                    && image.source_map[0].start_pc == 0
                    && image.source_map[0].end_pc == 2);
    Om_Source_Span mapped = {0};
    BYTECODE_ASSERT(om_bytecode_source_map_lookup(&image, 1, &mapped)
                    == OM_STATUS_OK
                    && mapped.source.value == span.source.value
                    && mapped.start == span.start && mapped.end == span.end);
    BYTECODE_ASSERT(om_bytecode_encode(&image, &bytes) == OM_STATUS_OK);
    static const uint8_t golden[] = {
        0x4f, 0x4d, 0x42, 0x43, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x06, 0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x03,
        0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00
    };
    BYTECODE_ASSERT(bytes.length == sizeof(golden)
                    && memcmp(bytes.data, golden, sizeof(golden)) == 0);
    BYTECODE_ASSERT(om_bytecode_decode(
        (Om_Const_Byte_Slice){bytes.data, bytes.length}, 1024, &decoded,
        &verify) == OM_STATUS_OK && verify.valid);
    BYTECODE_ASSERT(decoded.constant_count == image.constant_count
                    && decoded.function_count == image.function_count
                    && decoded.instruction_count == image.instruction_count
                    && decoded.source_map_count == image.source_map_count);
    passed = true;
cleanup:
    om_bytecode_image_destroy(&decoded);
    om_byte_slice_destroy(&bytes);
    om_bytecode_image_destroy(&image);
    om_bytecode_builder_destroy(&builder);
    return passed;
}

bool om_test_bytecode_verifier_rejects_invalid_images(void)
{
    const Om_Bytecode_Constant constants[] = {
        {.kind = OM_BYTECODE_CONSTANT_STRING,
         .as.string_value = {"bad", 3}}
    };
    Om_Bytecode_Function function = {
        .name_constant = 0, .entry_pc = 0, .end_pc = 2,
        .parameter_count = 0, .local_count = 0, .max_stack = 1,
        .flags = OM_BYTECODE_FUNCTION_NONE
    };
    Om_Bytecode_Instruction instructions[] = {
        {.opcode = OM_BC_LOAD_CONST, .operand_a = 1},
        {.opcode = OM_BC_RETURN}
    };
    Om_Bytecode_Image image = {
        .constants = constants, .constant_count = 1,
        .functions = &function, .function_count = 1,
        .instructions = instructions, .instruction_count = 2
    };
    Om_Bytecode_Verify_Result verify = {0};
    OM_TEST_ASSERT(om_bytecode_verify(&image, &verify) == OM_STATUS_OK
                   && !verify.valid
                   && verify.code == OM_BYTECODE_VERIFY_INVALID_OPERAND);
    instructions[0] = (Om_Bytecode_Instruction){
        .opcode = OM_BC_LOAD_UNIT
    };
    instructions[1] = (Om_Bytecode_Instruction){.opcode = OM_BC_JUMP,
                                                 .operand_a = 2};
    OM_TEST_ASSERT(om_bytecode_verify(&image, &verify) == OM_STATUS_OK
                   && !verify.valid
                   && verify.code == OM_BYTECODE_VERIFY_INVALID_JUMP);
    instructions[1] = (Om_Bytecode_Instruction){.opcode = OM_BC_RETURN};
    function.max_stack = 0;
    OM_TEST_ASSERT(om_bytecode_verify(&image, &verify) == OM_STATUS_OK
                   && !verify.valid
                   && verify.code == OM_BYTECODE_VERIFY_STACK_LIMIT);
    const Om_Bytecode_Constant schema_constants[] = {
        {.kind = OM_BYTECODE_CONSTANT_STRING,
         .as.string_value = {"make_ping", 9}},
        {.kind = OM_BYTECODE_CONSTANT_STRING,
         .as.string_value = {"value", 5}},
        {.kind = OM_BYTECODE_CONSTANT_INT, .as.integer_value = 7}
    };
    Om_Bytecode_Function schema_function = {
        .name_constant = 0, .entry_pc = 0, .end_pc = 3,
        .parameter_count = 0, .local_count = 0, .max_stack = 1,
        .flags = OM_BYTECODE_FUNCTION_NONE
    };
    Om_Bytecode_Instruction schema_instructions[] = {
        {.opcode = OM_BC_LOAD_CONST, .operand_a = 2},
        {.opcode = OM_BC_MAKE_MESSAGE, .operand_a = 42, .operand_b = 1},
        {.opcode = OM_BC_RETURN}
    };
    const Om_Bytecode_Schema schema = {
        .symbol_id = {42}, .kind = OM_BYTECODE_SCHEMA_MESSAGE,
        .first_field = 0, .field_count = 1
    };
    const Om_Bytecode_Schema_Field schema_field = {.name_constant = 1};
    image = (Om_Bytecode_Image){
        .constants = schema_constants, .constant_count = 3,
        .functions = &schema_function, .function_count = 1,
        .instructions = schema_instructions, .instruction_count = 3,
        .schemas = &schema, .schema_count = 1,
        .schema_fields = &schema_field, .schema_field_count = 1
    };
    OM_TEST_ASSERT(om_bytecode_verify(&image, &verify) == OM_STATUS_OK
                   && verify.valid);
    schema_instructions[1].operand_b = 0;
    OM_TEST_ASSERT(om_bytecode_verify(&image, &verify) == OM_STATUS_OK
                   && !verify.valid
                   && verify.code == OM_BYTECODE_VERIFY_INVALID_OPERAND);
    return true;
}

bool om_test_bytecode_decoder_fuzz(void)
{
    uint32_t state = UINT32_C(0x6f626d63);
    uint8_t bytes[257] = {0};
    Om_Bytecode_Builder builder = {0};
    Om_Bytecode_Image sample = {0};
    Om_Byte_Slice encoded = {0};
    bool passed = false;
    for (size_t iteration = 0; iteration < 4096; ++iteration) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const size_t length = (size_t)(state % sizeof(bytes));
        for (size_t index = 0; index < length; ++index) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            bytes[index] = (uint8_t)state;
        }
        Om_Bytecode_Image image = {0};
        Om_Bytecode_Verify_Result verify = {0};
        const Om_Status status = om_bytecode_decode(
            (Om_Const_Byte_Slice){bytes, length}, sizeof(bytes), &image, &verify);
        if (status == OM_STATUS_OK) {
            if (!verify.valid || image.owned_storage == NULL) {
                om_bytecode_image_destroy(&image);
                return false;
            }
        } else if (image.owned_storage != NULL) {
            om_bytecode_image_destroy(&image);
            return false;
        }
        om_bytecode_image_destroy(&image);
    }
    BYTECODE_ASSERT(om_bytecode_builder_init(&builder) == OM_STATUS_OK);
    uint32_t name_constant = 0;
    uint32_t function_index = 0;
    BYTECODE_ASSERT(om_bytecode_builder_add_constant(
        &builder, &(Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_STRING,
            .as.string_value = {"fuzz", 4}
        }, &name_constant) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_begin_function(
        &builder, name_constant, 0, OM_BYTECODE_FUNCTION_NONE,
        &function_index) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_emit(
        &builder, OM_BC_LOAD_UNIT, 0, 0, 0,
        (Om_Source_Span){{0}, 0, 1}, NULL) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_emit(
        &builder, OM_BC_RETURN, 0, 0, 0,
        (Om_Source_Span){{0}, 0, 1}, NULL) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_end_function(
        &builder, function_index) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_builder_finish(
        &builder, &sample, NULL) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_encode(&sample, &encoded) == OM_STATUS_OK);
    for (size_t length = 0; length < encoded.length; ++length) {
        Om_Bytecode_Image image = {0};
        Om_Bytecode_Verify_Result verify = {0};
        const Om_Status status = om_bytecode_decode(
            (Om_Const_Byte_Slice){encoded.data, length},
            encoded.length, &image, &verify);
        BYTECODE_ASSERT(status != OM_STATUS_OK && image.owned_storage == NULL);
        om_bytecode_image_destroy(&image);
    }
    for (size_t iteration = 0; iteration < 4096; ++iteration) {
        uint8_t *mutated = om_alloc(encoded.length);
        BYTECODE_ASSERT(mutated != NULL);
        (void)memcpy(mutated, encoded.data, encoded.length);
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const size_t mutation_count = (size_t)(state % 4) + 1;
        for (size_t mutation = 0; mutation < mutation_count; ++mutation) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const size_t offset = (size_t)(state % encoded.length);
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            mutated[offset] ^= (uint8_t)(state | 1u);
        }
        Om_Bytecode_Image image = {0};
        Om_Bytecode_Verify_Result verify = {0};
        const Om_Status status = om_bytecode_decode(
            (Om_Const_Byte_Slice){mutated, encoded.length},
            encoded.length, &image, &verify);
        om_free(mutated);
        if (status == OM_STATUS_OK) {
            BYTECODE_ASSERT(verify.valid && image.owned_storage != NULL);
        } else {
            BYTECODE_ASSERT(image.owned_storage == NULL);
        }
        om_bytecode_image_destroy(&image);
    }
    passed = true;
cleanup:
    om_byte_slice_destroy(&encoded);
    om_bytecode_image_destroy(&sample);
    om_bytecode_builder_destroy(&builder);
    return passed;
}

bool om_test_bytecode_emitter_project(void)
{
    Om_Bytecode_Test_Project test_project = {0};
    Om_Bytecode_Emit_Result emitted = {0};
    Om_Bytecode_Image decoded = {0};
    Om_Byte_Slice bytes = {0};
    bool passed = false;
    BYTECODE_ASSERT(load_emitter_project(&test_project));
    BYTECODE_ASSERT(om_bytecode_emit_project(
        &test_project.project, test_project.parsed, test_project.module_count,
        &test_project.resolution, &test_project.inference,
        &test_project.semantic, &emitted) == OM_STATUS_OK);
    Om_Bytecode_Verify_Result verify = {0};
    BYTECODE_ASSERT(om_bytecode_verify(&emitted.image, &verify)
                    == OM_STATUS_OK && verify.valid);
    bool saw_for = false;
    bool saw_try = false;
    bool saw_result_match = false;
    bool saw_spawn = false;
    bool saw_send = false;
    bool saw_state = false;
    bool saw_system_actor = false;
    for (size_t index = 0; index < emitted.image.instruction_count; ++index) {
        const Om_Bytecode_Opcode opcode =
            emitted.image.instructions[index].opcode;
        saw_for = saw_for || opcode == OM_BC_FOR_RANGE_TEST;
        saw_try = saw_try || opcode == OM_BC_CHECK_RESULT;
        saw_result_match = saw_result_match || opcode == OM_BC_IS_OK;
        saw_spawn = saw_spawn || opcode == OM_BC_SPAWN;
        saw_send = saw_send || opcode == OM_BC_SEND;
        saw_state = saw_state || opcode == OM_BC_LOAD_STATE
            || opcode == OM_BC_STORE_STATE;
        saw_system_actor = saw_system_actor
            || opcode == OM_BC_LOAD_SYSTEM_ACTOR;
    }
    BYTECODE_ASSERT(saw_for && saw_try && saw_result_match && saw_spawn
                    && saw_send && saw_state && saw_system_actor
                    && emitted.image.actor_count == 2
                    && emitted.image.actors[0].handler_count == 1);
    BYTECODE_ASSERT(emitted.image.program_fingerprint != 0
                    && emitted.image.interface_fingerprint != 0);
    BYTECODE_ASSERT(emitted.image.schema_count > 0);
    for (size_t index = 0; index < emitted.image.schema_count; ++index) {
        BYTECODE_ASSERT(emitted.image.schemas[index].fingerprint != 0);
    }
    BYTECODE_ASSERT(om_bytecode_encode(&emitted.image, &bytes) == OM_STATUS_OK);
    BYTECODE_ASSERT(om_bytecode_decode(
        (Om_Const_Byte_Slice){bytes.data, bytes.length},
        OM_BYTECODE_DEFAULT_MAX_BYTES, &decoded, &verify) == OM_STATUS_OK
        && verify.valid
        && decoded.instruction_count == emitted.image.instruction_count
        && decoded.schema_count == emitted.image.schema_count
        && decoded.program_fingerprint == emitted.image.program_fingerprint
        && decoded.interface_fingerprint
            == emitted.image.interface_fingerprint);
    for (size_t index = 0; index < decoded.schema_count; ++index) {
        BYTECODE_ASSERT(decoded.schemas[index].fingerprint
                        == emitted.image.schemas[index].fingerprint);
    }
    passed = true;
cleanup:
    om_bytecode_image_destroy(&decoded);
    om_byte_slice_destroy(&bytes);
    om_bytecode_emit_result_destroy(&emitted);
    destroy_emitter_project(&test_project);
    return passed;
}
