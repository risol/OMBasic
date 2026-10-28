#include "cli_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/compiler/pipeline.h"
#include "ombasic/linker/linker.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/source/source_text.h"
#include "ombasic/native/metadata/omi_project.h"
#include "ombasic/tooling/diagnostics_json.h"
#include "ombasic/tooling/interface_cache.h"
#include "ombasic/tooling/type_format.h"

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define OM_NATIVE_EXECUTABLE_SUFFIX ".exe"
#else
#define OM_NATIVE_EXECUTABLE_SUFFIX ""
#endif

typedef struct Om_Cli_Diagnostic_List {
    Om_Tooling_Diagnostic *items;
    size_t length;
    size_t capacity;
} Om_Cli_Diagnostic_List;

static Om_Status read_stdin_source(uint8_t **bytes, size_t *length)
{
    size_t capacity = 0;
    size_t used = 0;
    uint8_t *data = NULL;
    if (bytes == NULL || length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = NULL;
    *length = 0;
    for (;;) {
        if (capacity - used < 4096) {
            size_t next_capacity = capacity == 0 ? 4096 : capacity * 2;
            if (next_capacity > OM_SOURCE_TEXT_DEFAULT_MAX_BYTES) {
                next_capacity = OM_SOURCE_TEXT_DEFAULT_MAX_BYTES;
            }
            if (next_capacity <= capacity) {
                om_free(data);
                return OM_STATUS_OUT_OF_RANGE;
            }
            uint8_t *next = om_realloc_array(data, next_capacity, sizeof(*next));
            if (next == NULL) {
                om_free(data);
                return OM_STATUS_OUT_OF_MEMORY;
            }
            data = next;
            capacity = next_capacity;
        }
        const size_t count = fread(data + used, 1, capacity - used, stdin);
        used += count;
        if (count == 0) {
            if (ferror(stdin) != 0) {
                om_free(data);
                return OM_STATUS_IO_ERROR;
            }
            break;
        }
        if (used == OM_SOURCE_TEXT_DEFAULT_MAX_BYTES) {
            const int next = fgetc(stdin);
            if (next != EOF) {
                om_free(data);
                return OM_STATUS_OUT_OF_RANGE;
            }
            if (ferror(stdin) != 0) {
                om_free(data);
                return OM_STATUS_IO_ERROR;
            }
            break;
        }
    }
    if (data == NULL) {
        data = om_alloc(1);
        if (data == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    *bytes = data;
    *length = used;
    return OM_STATUS_OK;
}

static Om_String_View literal_view(const char *value)
{
    return (Om_String_View){value, strlen(value)};
}

static bool symbol_name_matches(const Om_Symbol *symbol, const char *name)
{
    if (symbol == NULL || name == NULL) {
        return false;
    }
    const size_t length = strlen(name);
    return symbol->name.length == length
        && (length == 0 || memcmp(symbol->name.data, name, length) == 0);
}

static const Om_Symbol *find_entry_symbol(
    const Om_Resolution_Result *resolution,
    const char *entry_name
)
{
    const Om_Symbol *entry = NULL;
    if (resolution == NULL || entry_name == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &resolution->symbols[index];
        if (symbol->module_id.value != 0
            || (symbol->kind != OM_SYMBOL_ACTION
                && symbol->kind != OM_SYMBOL_FUNCTION)
            || !symbol_name_matches(symbol, entry_name)) {
            continue;
        }
        if (entry != NULL) {
            return NULL;
        }
        entry = symbol;
    }
    return entry;
}

#ifdef _WIN32
static int copy_windows_environment_variable(const char *name, char **value)
{
    *value = NULL;
    size_t required_size = 0;
    if (getenv_s(&required_size, NULL, 0, name) != 0) {
        return -1;
    }
    if (required_size == 0) {
        return 0;
    }
    char *copy = om_alloc(required_size);
    if (copy == NULL) {
        return -1;
    }
    size_t actual_size = 0;
    if (getenv_s(&actual_size, copy, required_size, name) != 0
        || actual_size > required_size) {
        om_free(copy);
        return -1;
    }
    *value = copy;
    return 0;
}
#endif

static bool path_exists(const char *path)
{
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

static char *make_temporary_executable_path(void)
{
    static unsigned long sequence = 0;
#ifdef _WIN32
    char *environment_directory = NULL;
    if (copy_windows_environment_variable("TEMP", &environment_directory)
        != 0) {
        return NULL;
    }
    const char *directory = environment_directory;
    const unsigned long process_id = (unsigned long)_getpid();
#else
    const char *directory = getenv("TMPDIR");
    const unsigned long process_id = (unsigned long)getpid();
#endif
    if (directory == NULL || directory[0] == '\0') {
#ifdef _WIN32
        directory = ".";
#else
        directory = "/tmp";
#endif
    }
    const size_t directory_length = strlen(directory);
    if (directory_length > SIZE_MAX - 96) {
#ifdef _WIN32
        om_free(environment_directory);
#endif
        return NULL;
    }
    char *path = om_alloc(directory_length + 96);
    if (path == NULL) {
#ifdef _WIN32
        om_free(environment_directory);
#endif
        return NULL;
    }
    const bool has_separator = directory_length != 0
        && (directory[directory_length - 1] == '/' ||
            directory[directory_length - 1] == '\\');
    const char *separator_text = has_separator ? "" :
#ifdef _WIN32
        "\\";
#else
        "/";
#endif
    for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
        ++sequence;
        const int length = snprintf(path, directory_length + 96,
            "%s%sombasic-run-%lu-%lu%s", directory, separator_text,
            process_id, sequence, OM_NATIVE_EXECUTABLE_SUFFIX);
        if (length < 0 || (size_t)length >= directory_length + 96) {
            om_free(path);
#ifdef _WIN32
            om_free(environment_directory);
#endif
            return NULL;
        }
        const size_t path_length = (size_t)length;
        if (path_length <= SIZE_MAX - sizeof(".obj")) {
            char *object_path = om_alloc(path_length + sizeof(".obj"));
            if (object_path == NULL) {
                om_free(path);
#ifdef _WIN32
                om_free(environment_directory);
#endif
                return NULL;
            }
            (void)memcpy(object_path, path, path_length);
            (void)memcpy(object_path + path_length, ".obj", sizeof(".obj"));
            const bool unused = !path_exists(path) && !path_exists(object_path);
            om_free(object_path);
            if (unused) {
#ifdef _WIN32
                om_free(environment_directory);
#endif
                return path;
            }
        }
    }
    om_free(path);
#ifdef _WIN32
    om_free(environment_directory);
#endif
    return NULL;
}

static char *make_object_path(const char *executable_path)
{
    const size_t length = strlen(executable_path);
    if (length > SIZE_MAX - sizeof(".obj")) {
        return NULL;
    }
    char *path = om_alloc(length + sizeof(".obj"));
    if (path == NULL) {
        return NULL;
    }
    (void)memcpy(path, executable_path, length);
    (void)memcpy(path + length, ".obj", sizeof(".obj"));
    return path;
}



static const char *default_emit_path(const char *emit_kind)
{
    if (strcmp(emit_kind, "hir") == 0) return "ombasic.hir";
    if (strcmp(emit_kind, "mir") == 0) return "ombasic.mir";
    if (strcmp(emit_kind, "llvm-ir") == 0) return "ombasic.ll";
    if (strcmp(emit_kind, "asm") == 0) return "ombasic.s";
    if (strcmp(emit_kind, "obj") == 0) return "ombasic.obj";
    return NULL;
}

static bool is_supported_emit_kind(const char *emit_kind)
{
    return emit_kind != NULL
        && (strcmp(emit_kind, "hir") == 0
            || strcmp(emit_kind, "mir") == 0
            || strcmp(emit_kind, "llvm-ir") == 0
            || strcmp(emit_kind, "asm") == 0
            || strcmp(emit_kind, "obj") == 0);
}

static int generate_native_program(
    const Om_Target *target,
    const Om_Module_Project *source_project,
    Om_String_View source_root,
    const Om_Parser_Result *parsed,
    size_t module_count,
    const Om_Resolution_Result *resolution,
    Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    const Om_Omi_Project *native_project,
    const Om_String_View *library_directories,
    size_t library_directory_count,
    const char *resource_dir_override,
    const char *sysroot,
    const char *entry_name,
    const char *requested_output,
    const char *emit_kind,
    bool run_after_link
)
{
    char *temporary_output = NULL;
    char *object_path = NULL;
    char *debug_symbols_path = NULL;
    const char *output_path = requested_output;
    bool keep_output = false;
    int exit_code = 1;
    Om_Status status = OM_STATUS_OK;
    Om_Compiler_Emit_Result compilation = {0};

    const Om_Symbol *entry = find_entry_symbol(resolution, entry_name);
    if (entry == NULL) {
        (void)fprintf(stderr,
            "ombasic: entry '%s' was not found in the entry module\n",
            entry_name);
        goto cleanup;
    }
    if (emit_kind != NULL) {
        if (run_after_link || !is_supported_emit_kind(emit_kind)) {
            (void)fprintf(stderr, "ombasic: invalid emit mode\n");
            goto cleanup;
        }
        output_path = requested_output != NULL ? requested_output
            : default_emit_path(emit_kind);
    } else if (run_after_link) {
        temporary_output = make_temporary_executable_path();
        output_path = temporary_output;
    } else if (output_path == NULL) {
        static const char default_output[] =
            "ombasic" OM_NATIVE_EXECUTABLE_SUFFIX;
        output_path = default_output;
    }
    if (output_path == NULL) {
        (void)fprintf(stderr,
            "ombasic: cannot create a temporary output path\n");
        goto cleanup;
    }
    object_path = emit_kind == NULL ? make_object_path(output_path) : NULL;
    if (emit_kind == NULL && object_path == NULL) {
        (void)fprintf(stderr, "ombasic: out of memory\n");
        goto cleanup;
    }

    Om_Compiler_Artifact artifact = OM_COMPILER_ARTIFACT_OBJECT;
    const char *artifact_path = object_path;
    if (emit_kind != NULL) {
        artifact_path = output_path;
        if (strcmp(emit_kind, "hir") == 0) {
            artifact = OM_COMPILER_ARTIFACT_HIR;
        } else if (strcmp(emit_kind, "mir") == 0) {
            artifact = OM_COMPILER_ARTIFACT_MIR;
        } else if (strcmp(emit_kind, "llvm-ir") == 0) {
            artifact = OM_COMPILER_ARTIFACT_LLVM_IR;
        } else if (strcmp(emit_kind, "asm") == 0) {
            artifact = OM_COMPILER_ARTIFACT_ASSEMBLY;
        } else {
            artifact = OM_COMPILER_ARTIFACT_OBJECT;
        }
    }
    Om_Compiler_Emit_Request request = {
        .target = target,
        .source_project = source_project,
        .source_root = source_root,
        .parsed_modules = parsed,
        .parsed_module_count = module_count,
        .resolution = resolution,
        .inference = inference,
        .semantic = semantic,
        .entry_symbol = entry->id,
        .artifact = artifact,
        .output_path = artifact_path,
    };
    status = om_compiler_emit(&request, &compilation);
    if (compilation.hir_verification.diagnostic_count != 0) {
        for (size_t index = 0;
             index < compilation.hir_verification.diagnostic_count; ++index) {
            (void)fprintf(stderr, "ombasic: HIR verification failed (%s)\n",
                om_hir_diagnostic_code_name(
                    compilation.hir_verification.diagnostics[index].code));
        }
        goto cleanup;
    }
    if (compilation.specialization.diagnostic_count != 0) {
        for (size_t index = 0;
             index < compilation.specialization.diagnostic_count; ++index) {
            (void)fprintf(stderr, "ombasic: specialization failed (%s)\n",
                om_specialization_diagnostic_code_name(
                    compilation.specialization.diagnostics[index].code));
        }
        goto cleanup;
    }
    if (compilation.mir_lowering.diagnostic_count != 0) {
        for (size_t index = 0;
             index < compilation.mir_lowering.diagnostic_count; ++index) {
            (void)fprintf(stderr,
                "ombasic: HIR-to-MIR lowering failed (%s)\n",
                om_mir_diagnostic_code_name(
                    compilation.mir_lowering.diagnostics[index].code));
        }
        goto cleanup;
    }
    if (compilation.mir_verification.diagnostic_count != 0) {
        for (size_t index = 0;
             index < compilation.mir_verification.diagnostic_count; ++index) {
            (void)fprintf(stderr, "ombasic: MIR verification failed (%s)\n",
                om_mir_diagnostic_code_name(
                    compilation.mir_verification.diagnostics[index].code));
        }
        goto cleanup;
    }
    if (status != OM_STATUS_OK) {
        if (compilation.failed_stage == OM_COMPILER_STAGE_BACKEND_EMIT) {
            (void)fprintf(stderr,
                "ombasic: LLVM object emission failed (%s)\n",
                om_status_name(status));
        } else if (compilation.failed_stage == OM_COMPILER_STAGE_HIR_EMIT) {
            (void)fprintf(stderr, "ombasic: HIR emission failed (%s)\n",
                          om_status_name(status));
        } else if (compilation.failed_stage == OM_COMPILER_STAGE_MIR_EMIT) {
            (void)fprintf(stderr, "ombasic: MIR emission failed (%s)\n",
                          om_status_name(status));
        } else {
            (void)fprintf(stderr, "ombasic: native lowering failed (%s)\n",
                          om_status_name(status));
        }
        goto cleanup;
    }
    if (emit_kind != NULL) {
        (void)fprintf(stderr, "ombasic: emitted %s %s\n",
                      emit_kind, output_path);
        exit_code = 0;
        keep_output = true;
        goto cleanup;
    }

    Om_String_View *native_libraries = NULL;
    size_t native_library_count = 0;
    if (native_project != NULL && native_project->module_count != 0) {
        native_library_count = native_project->module_count;
        if (native_library_count > SIZE_MAX / sizeof(*native_libraries)) {
            (void)fprintf(stderr, "ombasic: native library list is too large\n");
            goto cleanup;
        }
        native_libraries = om_alloc_array(native_library_count,
                                           sizeof(*native_libraries));
        if (native_libraries == NULL) {
            (void)fprintf(stderr, "ombasic: out of memory\n");
            goto cleanup;
        }
        for (size_t index = 0; index < native_library_count; ++index) {
            const Om_Omi_Module *module =
                om_omi_project_module(native_project, index);
            if (module == NULL) {
                om_free(native_libraries);
                (void)fprintf(stderr,
                    "ombasic: invalid native library descriptor\n");
                goto cleanup;
            }
            native_libraries[index] = module->library_path;
        }
    }
    Om_Toolchain_Resources resources = {0};
    const Om_Status resource_status = om_toolchain_runtime_resources_resolve(
        resource_dir_override, target->triple, &resources);
    if (resource_status != OM_STATUS_OK) {
        (void)fprintf(stderr,
            "ombasic: runtime resources unavailable for target '%s'; "
            "resource root '%s' is missing '%s'\n",
            resources.target_triple != NULL ? resources.target_triple : "host",
            resources.resource_root != NULL ? resources.resource_root
                : (resource_dir_override != NULL ? resource_dir_override
                    : "executable-relative SDK layout"),
            resources.missing_archive_path != NULL
                ? resources.missing_archive_path
                : "the expected target runtime archives");
        om_toolchain_resources_destroy(&resources);
        om_free(native_libraries);
        goto cleanup;
    }
    const Om_Linker_Request link_request = {
        .target = target,
        .target_runtime_root = resources.target_runtime_root,
        .link_driver = NULL,
        .sysroot = sysroot,
        .object_path = object_path,
        .executable_path = output_path,
        .libraries = native_libraries,
        .library_count = native_library_count,
        .library_directories = library_directories,
        .library_directory_count = library_directory_count,
        .resources = &resources,
    };
    const int linker_result = om_linker_link_native(&link_request,
                                                     &debug_symbols_path);
    om_toolchain_resources_destroy(&resources);
    om_free(native_libraries);
    if (linker_result != 0) {
        (void)fprintf(stderr, "ombasic: native linker failed (exit %d)\n",
                      linker_result);
        goto cleanup;
    }
    if (run_after_link) {
        char *arguments[] = {(char *)output_path, NULL};
        const int program_result = om_linker_run_executable(output_path, arguments);
        if (program_result < 0) {
            (void)fprintf(stderr,
                "ombasic: could not start native executable\n");
            goto cleanup;
        }
        exit_code = program_result;
    } else {
        (void)fprintf(stderr, "ombasic: built %s\n", output_path);
        exit_code = 0;
        keep_output = true;
    }

cleanup:
    if (object_path != NULL) {
        (void)remove(object_path);
    }
    if (!keep_output && output_path != NULL && run_after_link) {
        (void)remove(output_path);
    }
    if (!keep_output && debug_symbols_path != NULL) {
        (void)remove(debug_symbols_path);
    }
    om_free(debug_symbols_path);
    om_free(object_path);
    om_free(temporary_output);
    om_compiler_emit_result_destroy(&compilation);
    return exit_code;
}

static const Om_Source_Text *source_for_module(const Om_Module_Project *project,
                                               Om_Module_Id module_id)
{
    return om_module_project_source(project, module_id);
}

static Om_Status push_diagnostic(Om_Cli_Diagnostic_List *list,
                                 Om_Tooling_Diagnostic diagnostic)
{
    if (list == NULL || diagnostic.source == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (list->length == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        if (capacity < list->capacity) {
            return OM_STATUS_OVERFLOW;
        }
        Om_Tooling_Diagnostic *items = om_realloc_array(
            list->items, capacity, sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->length++] = diagnostic;
    return OM_STATUS_OK;
}

static const char *lex_message(Om_Lex_Diagnostic_Code code)
{
    switch (code) {
    case OM_LEX_DIAGNOSTIC_E0101:
        return "Reserved keyword spelling must use exact uppercase casing.";
    case OM_LEX_DIAGNOSTIC_E0102:
        return "Identifier does not match an ordinary or entity name pattern.";
    case OM_LEX_DIAGNOSTIC_E0103:
        return "Malformed or unsupported lexical token.";
    case OM_LEX_DIAGNOSTIC_E0104:
        return "Numeric literal is outside the supported Int64 or finite Float range.";
    default:
        return "Lexical error.";
    }
}

static const char *parse_message(Om_Parse_Diagnostic_Code code)
{
    switch (code) {
    case OM_PARSE_DIAGNOSTIC_E0202:
        return "Expected a different token or found malformed syntax.";
    case OM_PARSE_DIAGNOSTIC_E0203:
        return "Unsupported top-level construct or import placement.";
    case OM_PARSE_DIAGNOSTIC_E0204:
        return "An expression is required at this position.";
    case OM_PARSE_DIAGNOSTIC_E0205:
        return "The 2^63 integer magnitude must be the direct operand of unary minus.";
    case OM_PARSE_DIAGNOSTIC_E0206:
        return "A comparison expression cannot contain a comparison chain.";
    case OM_PARSE_DIAGNOSTIC_E0207:
        return "A standalone expression statement must be a call.";
    case OM_PARSE_DIAGNOSTIC_E0208:
        return "Parser recovery or nesting safety limit was reached.";
    default:
        return "Syntax error.";
    }
}

static const char *resolution_message(Om_Resolution_Diagnostic_Code code)
{
    switch (code) {
    case OM_RESOLUTION_DIAGNOSTIC_E0301:
        return "Name or qualified module member could not be resolved.";
    case OM_RESOLUTION_DIAGNOSTIC_E0302:
        return "Imported module member exists but is private.";
    case OM_RESOLUTION_DIAGNOSTIC_E0303:
        return "Name is declared more than once in the same scope.";
    case OM_RESOLUTION_DIAGNOSTIC_E0304:
        return "Declaration conflicts with a visible name using different casing.";
    case OM_RESOLUTION_DIAGNOSTIC_E0305:
        return "Declaration shadows a name visible from an enclosing scope or prelude.";
    case OM_RESOLUTION_DIAGNOSTIC_E0306:
        return "Module alias is invalid or conflicts with another declaration.";
    case OM_RESOLUTION_DIAGNOSTIC_E0307:
        return "Assignment targets an immutable binding.";
    default:
        return "Name resolution error.";
    }
}

static const char *type_message(Om_Type_Inference_Diagnostic_Code code)
{
    switch (code) {
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0401:
        return "Expression, argument, field, return value, or exported field has an incompatible or unstable type.";
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0402:
        return "Empty collection has no contextual type for inference.";
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0403:
        return "Native callback must bind to a unique Message with matching fields and be registered inside an Actor RECEIVE.";
    default:
        return "Type inference error.";
    }
}

static const char *semantic_message(Om_Semantic_Analysis_Diagnostic_Code code)
{
    switch (code) {
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501:
        return "Effect is not allowed in this context.";
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601:
        return "Actor does not accept this Message.";
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701:
        return "Message payload contains a non-serializable value.";
    default:
        return "Semantic validation error.";
    }
}

static Om_Status collect_lex_diagnostics(Om_Cli_Diagnostic_List *list,
                                         const Om_Source_Text *source,
                                         const Om_Lexer_Result *lexed)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < lexed->diagnostic_count; ++index) {
        const Om_Lex_Diagnostic *item = &lexed->diagnostics[index];
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"lexer", 5},
            .code = literal_view(om_lex_diagnostic_code_name(item->code)),
            .message = literal_view(lex_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static Om_Status collect_parse_diagnostics(Om_Cli_Diagnostic_List *list,
                                           const Om_Source_Text *source,
                                           const Om_Parser_Result *parsed)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < parsed->diagnostic_count; ++index) {
        const Om_Parse_Diagnostic *item = &parsed->diagnostics[index];
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"parser", 6},
            .code = literal_view(om_parse_diagnostic_code_name(item->code)),
            .message = literal_view(parse_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static Om_Status collect_resolution_diagnostics(Om_Cli_Diagnostic_List *list,
                                                const Om_Module_Project *project,
                                                const Om_Resolution_Result *result)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < result->diagnostic_count; ++index) {
        const Om_Resolution_Diagnostic *item = &result->diagnostics[index];
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        const Om_Source_Text *related = item->has_related_span
            ? source_for_module(project, item->related_span.source.value == UINT32_MAX
                ? item->module_id : (Om_Module_Id){item->related_span.source.value})
            : NULL;
        if (source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"resolution", 10},
            .code = literal_view(om_resolution_diagnostic_code_name(item->code)),
            .message = literal_view(resolution_message(item->code)),
            .source = source,
            .span = item->span,
            .has_related_span = item->has_related_span,
            .related_source = related,
            .related_span = item->related_span
        });
    }
    return status;
}

static Om_Status collect_type_diagnostics(Om_Cli_Diagnostic_List *list,
                                          const Om_Module_Project *project,
                                          const Om_Type_Inference_Result *result)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < result->diagnostic_count; ++index) {
        const Om_Type_Inference_Diagnostic *item = &result->diagnostics[index];
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        if (source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"type", 4},
            .code = literal_view(om_type_inference_diagnostic_code_name(item->code)),
            .message = literal_view(type_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static Om_Status collect_semantic_diagnostics(Om_Cli_Diagnostic_List *list,
                                              const Om_Module_Project *project,
                                              const Om_Semantic_Analysis_Result *result)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < result->diagnostic_count; ++index) {
        const Om_Semantic_Analysis_Diagnostic *item = &result->diagnostics[index];
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        if (source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"semantic", 8},
            .code = literal_view(om_semantic_analysis_diagnostic_code_name(item->code)),
            .message = literal_view(semantic_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static void print_diagnostics(const Om_Cli_Diagnostic_List *list, bool json)
{
    if (json) {
        Om_Tooling_Json_Result result = {0};
        const Om_Status status = om_tooling_diagnostics_to_json(
            list->items, list->length, &result);
        if (status == OM_STATUS_OK) {
            (void)fwrite(result.data, 1, result.length, stdout);
            (void)fputc('\n', stdout);
        } else {
            (void)fprintf(stderr, "ombasic: could not encode diagnostics (%s)\n",
                          om_status_name(status));
        }
        om_tooling_json_result_destroy(&result);
        return;
    }
    for (size_t index = 0; index < list->length; ++index) {
        const Om_Tooling_Diagnostic *diagnostic = &list->items[index];
        Om_Source_Location location = {0};
        (void)om_source_text_location(diagnostic->source, diagnostic->span.start,
                                      &location);
        const Om_String_View path = diagnostic->source->canonical_path;
        (void)fprintf(stderr, "%.*s:%u:%u: %.*s: %.*s\n",
                      path.length > INT_MAX ? INT_MAX : (int)path.length,
                      path.data, location.line, location.byte_column,
                      (int)diagnostic->code.length, diagnostic->code.data,
                      (int)diagnostic->message.length, diagnostic->message.data);
    }
}

static Om_Status print_json_string(Om_String_View value)
{
    static const char hex[] = "0123456789abcdef";
    if (fputc('"', stdout) == EOF) {
        return OM_STATUS_IO_ERROR;
    }
    for (size_t index = 0; index < value.length; ++index) {
        const unsigned char byte = (unsigned char)value.data[index];
        if (byte == '"' || byte == '\\') {
            if (fputc('\\', stdout) == EOF || fputc(byte, stdout) == EOF) {
                return OM_STATUS_IO_ERROR;
            }
        } else if (byte == '\n' || byte == '\r' || byte == '\t') {
            const char escaped = byte == '\n' ? 'n' : byte == '\r' ? 'r' : 't';
            if (fputc('\\', stdout) == EOF || fputc(escaped, stdout) == EOF) {
                return OM_STATUS_IO_ERROR;
            }
        } else if (byte < UINT8_C(0x20)) {
            const char escaped[6] = {
                '\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0f]
            };
            if (fwrite(escaped, 1, sizeof(escaped), stdout) != sizeof(escaped)) {
                return OM_STATUS_IO_ERROR;
            }
        } else if (fputc(byte, stdout) == EOF) {
            return OM_STATUS_IO_ERROR;
        }
    }
    return fputc('"', stdout) == EOF ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
}

static const char *ast_kind_name(Om_Ast_Kind kind)
{
    switch (kind) {
    case OM_AST_BINDING: return "binding";
    case OM_AST_NAME: return "name";
    case OM_AST_PARAMETER: return "parameter";
    case OM_AST_CALL: return "call";
    case OM_AST_MEMBER: return "member";
    case OM_AST_INDEX: return "index";
    case OM_AST_BINARY: return "binary";
    case OM_AST_UNARY: return "unary";
    case OM_AST_INTEGER_LITERAL: return "integer";
    case OM_AST_FLOAT_LITERAL: return "float";
    case OM_AST_STRING_LITERAL: return "string";
    case OM_AST_BOOLEAN_LITERAL: return "boolean";
    case OM_AST_LIST: return "list";
    case OM_AST_MAP: return "map";
    case OM_AST_SPAWN: return "spawn";
    default: return "expression";
    }
}

static const char *symbol_kind_name(Om_Symbol_Kind kind)
{
    switch (kind) {
    case OM_SYMBOL_FUNCTION: return "function";
    case OM_SYMBOL_ACTION: return "action";
    case OM_SYMBOL_RECORD: return "record";
    case OM_SYMBOL_MESSAGE: return "message";
    case OM_SYMBOL_ACTOR: return "actor";
    default: return "symbol";
    }
}

static Om_Status write_inferred_nodes(const Om_Module_Project *project,
                                      const Om_Parser_Result *parsed,
                                      size_t module_count,
                                      const Om_Resolution_Result *resolution,
                                      const Om_Type_Inference_Result *inference)
{
    Om_Status status = OM_STATUS_OK;
    bool first = true;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < inference->inferred_node_count; ++index) {
        const Om_Inferred_Node *inferred = &inference->inferred_nodes[index];
        if (inferred->module_id.value >= module_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Source_Text *source = source_for_module(project, inferred->module_id);
        const Om_Ast *ast = &parsed[inferred->module_id.value].ast;
        const Om_Ast_Node *node = om_ast_get_node(ast, inferred->node_id);
        if (source == NULL || node == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Tooling_String type = {0};
        status = om_tooling_type_to_string(&inference->type_store, resolution,
                                           inferred->type_id, &type);
        Om_Source_Location start = {0};
        Om_Source_Location end = {0};
        if (status == OM_STATUS_OK) {
            status = om_source_text_location(source, node->span.start, &start);
        }
        if (status == OM_STATUS_OK) {
            status = om_source_text_location(source, node->span.end, &end);
        }
        if (status == OM_STATUS_OK && !first) {
            status = fputc(',', stdout) == EOF ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = fputs("{\"path\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = print_json_string(source->canonical_path);
        }
        if (status == OM_STATUS_OK) {
            status = fputs(",\"kind\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            const char *kind = ast_kind_name(node->kind);
            status = print_json_string((Om_String_View){kind, strlen(kind)});
        }
        if (status == OM_STATUS_OK) {
            status = fputs(",\"name\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = print_json_string(node->name);
        }
        if (status == OM_STATUS_OK) {
            status = fputs(",\"type\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = print_json_string((Om_String_View){type.data, type.length});
        }
        if (status == OM_STATUS_OK) {
            status = fprintf(stdout,
                ",\"range\":{\"start\":{\"line\":%u,\"byteColumn\":%u},"
                "\"end\":{\"line\":%u,\"byteColumn\":%u}}}",
                start.line, start.byte_column, end.line, end.byte_column) < 0
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            first = false;
        }
        om_tooling_string_destroy(&type);
    }
    return status;
}

static void print_analysis_json(const Om_Cli_Diagnostic_List *list,
                                const Om_Module_Project *project,
                                const Om_Parser_Result *parsed,
                                size_t module_count,
                                const Om_Resolution_Result *resolution,
                                const Om_Type_Inference_Result *inference,
                                const Om_Interface_Cache *interfaces,
                                bool include_inferred,
                                bool include_interfaces)
{
    Om_Tooling_Json_Result diagnostics_json = {0};
    const Om_Status status = om_tooling_diagnostics_to_json(
        list->items, list->length, &diagnostics_json);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: could not encode diagnostics (%s)\n",
                      om_status_name(status));
        goto cleanup;
    }
    if (!include_inferred && !include_interfaces) {
        (void)fwrite(diagnostics_json.data, 1, diagnostics_json.length, stdout);
        (void)fputc('\n', stdout);
        goto cleanup;
    }
    static const char diagnostics_open[] = "{\"diagnostics\":[";
    if (diagnostics_json.length < sizeof(diagnostics_open)
        || memcmp(diagnostics_json.data, diagnostics_open,
                  sizeof(diagnostics_open) - 1) != 0) {
        (void)fprintf(stderr, "ombasic: invalid diagnostics JSON\n");
        goto cleanup;
    }
    (void)fwrite(diagnostics_json.data, 1, diagnostics_json.length - 1, stdout);
    (void)fputs(",\"inferred\":[", stdout);
    if (include_inferred && inference->type_store.owned_storage != NULL
        && resolution->symbols != NULL) {
        const Om_Status inferred_status = write_inferred_nodes(
            project, parsed, module_count, resolution, inference);
        if (inferred_status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: could not format inferred types (%s)\n",
                          om_status_name(inferred_status));
            goto cleanup;
        }
    }
    (void)fputs("],\"interfaces\":[", stdout);
    if (include_interfaces && interfaces->modules != NULL) {
        for (size_t module_index = 0; module_index < interfaces->module_count;
             ++module_index) {
            const Om_Module_Interface *module = &interfaces->modules[module_index];
            if (module_index != 0) {
                (void)fputc(',', stdout);
            }
            (void)fputs("{\"path\":", stdout);
            (void)print_json_string(module->path);
            (void)fprintf(stdout, ",\"sourceHash\":\"%" PRIu64
                          "\",\"interfaceHash\":\"%" PRIu64 "\",\"symbols\":[",
                          module->source_hash, module->interface_hash);
            for (size_t symbol_index = 0; symbol_index < module->symbol_count;
                 ++symbol_index) {
                const Om_Interface_Symbol *symbol = &module->symbols[symbol_index];
                if (symbol_index != 0) {
                    (void)fputc(',', stdout);
                }
                (void)fputs("{\"name\":", stdout);
                (void)print_json_string(symbol->name);
                (void)fputs(",\"kind\":", stdout);
                const char *kind = symbol_kind_name(symbol->kind);
                (void)print_json_string((Om_String_View){kind, strlen(kind)});
                (void)fprintf(stdout, ",\"effects\":%" PRIu32,
                              symbol->effects);
                if (symbol->type_scheme != NULL
                    && inference->type_store.owned_storage != NULL) {
                    Om_Tooling_String type = {0};
                    const Om_Status type_status = om_tooling_type_to_string(
                        &inference->type_store, resolution,
                        symbol->type_scheme->type_id, &type);
                    if (type_status == OM_STATUS_OK) {
                        (void)fputs(",\"type\":", stdout);
                        (void)print_json_string((Om_String_View){type.data, type.length});
                    } else {
                        (void)fputs(",\"type\":null", stdout);
                    }
                    om_tooling_string_destroy(&type);
                } else {
                    (void)fputs(",\"type\":null", stdout);
                }
                (void)fprintf(stdout, ",\"hasSerializableType\":%s,\"serializable\":%s}",
                    symbol->has_serializability ? "true" : "false",
                    symbol->serializable ? "true" : "false");
            }
            (void)fputs("],\"actorProtocol\":[", stdout);
            for (size_t protocol_index = 0;
                 protocol_index < module->actor_protocol_count; ++protocol_index) {
                const Om_Actor_Protocol_Entry *entry = &module->actor_protocol[protocol_index];
                const Om_Symbol *actor = om_resolution_symbol_by_id(
                    resolution, entry->actor_symbol);
                const Om_Symbol *message = om_resolution_symbol_by_id(
                    resolution, entry->message_symbol);
                if (actor == NULL || message == NULL) {
                    continue;
                }
                if (protocol_index != 0) {
                    (void)fputc(',', stdout);
                }
                (void)fputs("{\"actor\":", stdout);
                (void)print_json_string(actor->name);
                (void)fputs(",\"message\":", stdout);
                (void)print_json_string(message->name);
                (void)fputc('}', stdout);
            }
            (void)fputs("]}", stdout);
        }
    }
    (void)fputs("]}\n", stdout);
cleanup:
    om_tooling_json_result_destroy(&diagnostics_json);
}

int om_cli_compile_command(int argc, char **argv, const char *command)
{
    Om_Source_Root root = {0};
    Om_Module_Project project = {0};
    Om_Omi_Project native_project = {0};
    Om_Lexer_Result *lexed = NULL;
    Om_Parser_Result *parsed = NULL;
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Interface_Cache interfaces = {0};
    Om_Cli_Diagnostic_List diagnostics = {0};
    Om_String_View *native_metadata_paths = NULL;
    Om_String_View *library_directories = NULL;
    size_t native_metadata_path_count = 0;
    size_t library_directory_count = 0;
    uint8_t *entry_source = NULL;
    size_t entry_source_length = 0;
    Om_Status status = OM_STATUS_OK;
    const char *root_path = NULL;
    const char *entry_path = NULL;
    const char *output_path = NULL;
    const char *resource_dir_override = NULL;
    const char *target_triple_override = NULL;
    const char *sysroot = NULL;
    const char *entry_name = "main";
    const char *emit_kind = NULL;
    bool json = false;
    bool read_source_stdin = false;
    bool include_inferred = false;
    bool include_interfaces = false;
    bool all_syntax_valid = true;
    bool semantic_valid = false;
    int exit_code = 1;
    Om_Target selected_target = {0};

    const size_t option_capacity = argc > 2 ? (size_t)(argc - 2) : 0;
    if (option_capacity != 0) {
        native_metadata_paths = om_alloc_array(option_capacity,
                                               sizeof(*native_metadata_paths));
        library_directories = om_alloc_array(option_capacity,
                                             sizeof(*library_directories));
        if (native_metadata_paths == NULL || library_directories == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
    }

    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--root") == 0 && index + 1 < argc) {
            root_path = argv[++index];
        } else if (strcmp(argv[index], "--resource-dir") == 0
                   && index + 1 < argc) {
            resource_dir_override = argv[++index];
        } else if ((strcmp(command, "build") == 0
                    || strcmp(command, "run") == 0)
                   && strcmp(argv[index], "--target") == 0
                   && index + 1 < argc) {
            target_triple_override = argv[++index];
        } else if ((strcmp(command, "build") == 0
                    || strcmp(command, "run") == 0)
                   && strcmp(argv[index], "--sysroot") == 0
                   && index + 1 < argc) {
            sysroot = argv[++index];
        } else if (strcmp(argv[index], "--native") == 0
                   && index + 1 < argc) {
            const char *path = argv[++index];
            native_metadata_paths[native_metadata_path_count++] =
                literal_view(path);
        } else if (strcmp(argv[index], "-L") == 0 && index + 1 < argc) {
            const char *path = argv[++index];
            library_directories[library_directory_count++] =
                literal_view(path);
        } else if (strcmp(command, "build") == 0
                   && strncmp(argv[index], "--emit=", 7) == 0) {
            emit_kind = argv[index] + 7;
            if (!is_supported_emit_kind(emit_kind)) {
                (void)fprintf(stderr,
                    "Usage: ombasic build --emit=hir|mir|llvm-ir|asm|obj\\n");
                goto cleanup;
            }
        } else if (strcmp(argv[index], "--diagnostics=json") == 0) {
            json = true;
        } else if (strcmp(argv[index], "--source-stdin") == 0) {
            read_source_stdin = true;
        } else if (strcmp(argv[index], "--include-inferred") == 0) {
            include_inferred = true;
        } else if (strcmp(argv[index], "--include-interfaces") == 0) {
            include_interfaces = true;
        } else if (strcmp(command, "build") == 0
                   && (strcmp(argv[index], "--output") == 0
                       || strcmp(argv[index], "-o") == 0)
                   && index + 1 < argc) {
            output_path = argv[++index];
        } else if (strcmp(command, "run") == 0
                   && strcmp(argv[index], "--entry") == 0 && index + 1 < argc) {
            entry_name = argv[++index];
        } else if (argv[index][0] != '-' && entry_path == NULL) {
            entry_path = argv[index];
        } else {
            (void)fprintf(stderr,
                "Usage: ombasic %s <entry.bas> [--root <source-root>] "
                "[--target <llvm-triple>] [--sysroot path] "
                "[--native file.omi] [-L dir]\n",
                command);
            goto cleanup;
        }
    }
    if (root_path == NULL && entry_path != NULL) {
        root_path = ".";
    }
    if (root_path == NULL || entry_path == NULL) {
        (void)fprintf(stderr,
            "Usage: ombasic %s <entry.bas> [--root <source-root>] "
            "[--target <llvm-triple>] [--sysroot path] "
            "[--native file.omi] [-L dir]\n",
            command);
        goto cleanup;
    }
    if (strcmp(command, "build") == 0 || strcmp(command, "run") == 0) {
        Om_Target host_target = {0};
        status = om_target_host(&host_target);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr,
                "ombasic: E0700: configured host target is unavailable (%s)\n",
                om_status_name(status));
            goto cleanup;
        }
        if (target_triple_override == NULL) {
            selected_target = host_target;
        } else {
            status = om_target_describe(target_triple_override,
                                        &selected_target);
            if (status != OM_STATUS_OK) {
                (void)fprintf(stderr,
                    "ombasic: E0701: unsupported target triple '%s'\n",
                    target_triple_override);
                goto cleanup;
            }
            if (strcmp(selected_target.triple, host_target.triple) != 0) {
                (void)fprintf(stderr,
                    "ombasic: E0702: target runtime/sysroot unavailable for "
                    "'%s' (compiler host target is '%s')\n",
                    selected_target.triple, host_target.triple);
                goto cleanup;
            }
        }
    }
    status = om_source_root_init(&root, literal_view(root_path));
    if (status == OM_STATUS_OK && read_source_stdin) {
        status = read_stdin_source(&entry_source, &entry_source_length);
    }
    if (status == OM_STATUS_OK && read_source_stdin) {
        status = om_module_project_load_with_entry_source(
            &root, literal_view(entry_path),
            (Om_Const_Byte_Slice){entry_source, entry_source_length},
            OM_SOURCE_TEXT_DEFAULT_MAX_BYTES, &project);
    } else if (status == OM_STATUS_OK) {
        status = om_module_project_load(&root, literal_view(entry_path),
                                        OM_SOURCE_TEXT_DEFAULT_MAX_BYTES, &project);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: cannot load project (%s)\n",
                      om_status_name(status));
        goto cleanup;
    }

    const size_t module_count = om_module_project_module_count(&project);
    if (module_count == 0 || module_count > SIZE_MAX / sizeof(*lexed)) {
        status = OM_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (native_metadata_path_count != 0) {
        if (module_count > UINT32_MAX
            || native_metadata_path_count
                > (size_t)(UINT32_MAX - (uint32_t)module_count)) {
            status = OM_STATUS_OUT_OF_RANGE;
            goto cleanup;
        }
        status = om_omi_project_load(&root, native_metadata_paths,
            native_metadata_path_count, (uint32_t)module_count,
            OM_SOURCE_TEXT_DEFAULT_MAX_BYTES, &native_project);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr,
                "ombasic: cannot load Native metadata (%s)\n",
                om_status_name(status));
            goto cleanup;
        }
        bool native_metadata_valid = true;
        for (size_t module_index = 0;
             module_index < native_project.module_count; ++module_index) {
            const Om_Omi_Module *native_module =
                om_omi_project_module(&native_project, module_index);
            const Om_Source_Text *native_source =
                om_omi_project_source(&native_project, module_index);
            if (native_module == NULL || native_source == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            for (size_t diagnostic_index = 0;
                 diagnostic_index < native_module->diagnostic_count;
                 ++diagnostic_index) {
                const Om_Omi_Diagnostic *diagnostic =
                    &native_module->diagnostics[diagnostic_index];
                Om_Source_Location location = {0};
                (void)om_source_text_location(native_source,
                    diagnostic->span.start, &location);
                (void)fprintf(stderr, "%.*s:%u:%u: %s\n",
                    (int)native_source->canonical_path.length,
                    native_source->canonical_path.data, location.line,
                    location.byte_column,
                    om_omi_diagnostic_code_name(diagnostic->code));
                native_metadata_valid = false;
            }
        }
        if (!native_metadata_valid) {
            goto cleanup;
        }
    }
    lexed = om_alloc_array(module_count, sizeof(*lexed));
    parsed = om_alloc_array(module_count, sizeof(*parsed));
    if (lexed == NULL || parsed == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    (void)memset(lexed, 0, module_count * sizeof(*lexed));
    (void)memset(parsed, 0, module_count * sizeof(*parsed));
    for (size_t index = 0; index < module_count; ++index) {
        const Om_Module_Id module_id = {(uint32_t)index};
        const Om_Source_Text *source = source_for_module(&project, module_id);
        if (source == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        status = om_lexer_scan(source, &lexed[index]);
        if (status == OM_STATUS_OK) {
            status = collect_lex_diagnostics(&diagnostics, source, &lexed[index]);
        }
        if (status != OM_STATUS_OK) {
            goto cleanup;
        }
        if (lexed[index].diagnostic_count != 0) {
            all_syntax_valid = false;
            continue;
        }
        status = om_parser_parse(&lexed[index], &parsed[index]);
        if (status == OM_STATUS_OK) {
            status = collect_parse_diagnostics(&diagnostics, source, &parsed[index]);
        }
        if (status != OM_STATUS_OK) {
            goto cleanup;
        }
        if (parsed[index].diagnostic_count != 0) {
            all_syntax_valid = false;
        }
    }

    if (all_syntax_valid) {
        const Om_Resolver_Config resolver_config = {
            .native_modules = native_project.modules,
            .native_module_count = native_project.module_count,
        };
        status = om_name_resolve_project(&project, parsed, module_count,
                                         &resolver_config, &resolution);
        if (status == OM_STATUS_OK) {
            status = collect_resolution_diagnostics(&diagnostics, &project,
                                                    &resolution);
        }
        if (status == OM_STATUS_OK && resolution.diagnostic_count == 0) {
            status = om_type_infer_project(&project, parsed, module_count,
                                           &resolution, &inference);
        }
        if (status == OM_STATUS_OK && resolution.diagnostic_count == 0) {
            status = collect_type_diagnostics(&diagnostics, &project, &inference);
        }
        if (status == OM_STATUS_OK && resolution.diagnostic_count == 0
            && inference.diagnostic_count == 0) {
            status = om_semantic_analyze_project(&project, parsed, module_count,
                                                  &resolution, &inference, &semantic);
        }
        if (status == OM_STATUS_OK && semantic.diagnostics != NULL) {
            status = collect_semantic_diagnostics(&diagnostics, &project, &semantic);
        }
        if (status == OM_STATUS_OK && semantic.diagnostic_count == 0
            && inference.diagnostic_count == 0 && resolution.diagnostic_count == 0) {
            status = om_interface_cache_build(&project, &resolution, &inference,
                                              &semantic, &interfaces);
        }
        semantic_valid = status == OM_STATUS_OK && diagnostics.length == 0;
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: check failed (%s)\n", om_status_name(status));
        goto cleanup;
    }
    if (include_inferred || include_interfaces) {
        if (strcmp(command, "check") != 0 || !json) {
            (void)fprintf(stderr, "ombasic: analysis details require --diagnostics=json\n");
            goto cleanup;
        }
        print_analysis_json(&diagnostics, &project, parsed, module_count,
                            &resolution, &inference, &interfaces,
                            include_inferred, include_interfaces);
    } else if (strcmp(command, "check") == 0 || !semantic_valid) {
        print_diagnostics(&diagnostics, json);
    }
    exit_code = semantic_valid ? 0 : 1;
    if (semantic_valid && (strcmp(command, "run") == 0
                           || strcmp(command, "build") == 0)) {
        exit_code = generate_native_program(&selected_target,
            &project, root.canonical_path,
            parsed, module_count, &resolution, &inference, &semantic,
            &native_project, library_directories, library_directory_count,
            resource_dir_override, sysroot, entry_name, output_path, emit_kind,
            strcmp(command, "run") == 0);
    }

cleanup:
    om_free(entry_source);
    om_free(diagnostics.items);
    om_interface_cache_destroy(&interfaces);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    om_omi_project_destroy(&native_project);
    if (parsed != NULL) {
        for (size_t index = 0; index < om_module_project_module_count(&project); ++index) {
            om_parser_result_destroy(&parsed[index]);
        }
    }
    if (lexed != NULL) {
        for (size_t index = 0; index < om_module_project_module_count(&project); ++index) {
            om_lexer_result_destroy(&lexed[index]);
        }
    }
    om_free(parsed);
    om_free(lexed);
    om_module_project_destroy(&project);
    om_source_root_destroy(&root);
    om_free(library_directories);
    om_free(native_metadata_paths);
    return exit_code;
}
