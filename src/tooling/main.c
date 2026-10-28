#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/source/source_text.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/tooling/formatter.h"
#include "ombasic/native/metadata/omi.h"
#include "ombasic/linker/linker.h"
#include "ombasic/toolchain/resources.h"
#include "cli_internal.h"

#define OM_CLI_MAX_SOURCE_BYTES ((size_t)16 * (size_t)1024 * (size_t)1024)

static FILE *open_file(const char *path, const char *mode)
{
#ifdef _MSC_VER
    FILE *file = NULL;
    if (fopen_s(&file, path, mode) != 0) {
        return NULL;
    }
    return file;
#else
    return fopen(path, mode);
#endif
}

static bool read_file(const char *path, uint8_t **bytes, size_t *length)
{
    FILE *file = NULL;
    uint8_t *data = NULL;
    size_t used = 0;
    size_t capacity = 0;
    bool succeeded = false;
    if (path == NULL || bytes == NULL || length == NULL) {
        return false;
    }
    *bytes = NULL;
    *length = 0;
    file = open_file(path, "rb");
    if (file == NULL) {
        return false;
    }
    for (;;) {
        if (capacity - used < 4096) {
            size_t next_capacity = capacity == 0 ? 4096 : capacity * 2;
            if (next_capacity > OM_CLI_MAX_SOURCE_BYTES) {
                next_capacity = OM_CLI_MAX_SOURCE_BYTES;
            }
            if (next_capacity <= capacity) {
                goto cleanup;
            }
            uint8_t *next = om_realloc_array(data, next_capacity, sizeof(*next));
            if (next == NULL) {
                goto cleanup;
            }
            data = next;
            capacity = next_capacity;
        }
        const size_t count = fread(data + used, 1, capacity - used, file);
        used += count;
        if (count == 0) {
            if (ferror(file) != 0) {
                goto cleanup;
            }
            succeeded = true;
            break;
        }
        if (used == OM_CLI_MAX_SOURCE_BYTES) {
            const int next = fgetc(file);
            if (next != EOF) {
                goto cleanup;
            }
            succeeded = ferror(file) == 0;
            break;
        }
    }
    if (succeeded) {
        *bytes = data;
        *length = used;
        data = NULL;
    }
cleanup:
    om_free(data);
    if (fclose(file) != 0) {
        succeeded = false;
        om_free(*bytes);
        *bytes = NULL;
        *length = 0;
    }
    return succeeded;
}

static bool write_file(const char *path, const char *data, size_t length)
{
    FILE *file = open_file(path, "wb");
    if (file == NULL) {
        return false;
    }
    const bool wrote_all = fwrite(data, 1, length, file) == length;
    const bool closed = fclose(file) == 0;
    return wrote_all && closed;
}

static void print_usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage:\n"
                  "  ombasic version\n"
                  "  ombasic fmt [--write|--check] <file.bas>\n"
                  "  ombasic check <entry.bas> [--root <source-root>] [--native file.omi] [-L dir] [--diagnostics=json]\n"
                  "  ombasic run <entry.bas> [--root <source-root>] [--target <llvm-triple>] [--sysroot path] [--resource-dir <sdk-root>] [--native file.omi] [-L dir] [--entry callable]\n"
                  "  ombasic build <entry.bas> [--root <source-root>] [--target <llvm-triple>] [--sysroot path] [--resource-dir <sdk-root>] [--native file.omi] [-L dir] [--emit=hir|mir|llvm-ir|asm|obj] [-o output]\n"
                  "  ombasic lsp --root <source-root>\n"
                  "  ombasic bind --header <file.h> --library <path> --output <file.omi> [-I dir]\n"
                  "  ombasic check-omi <file.omi>\n");
}

static int check_omi_file(const char *path)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    int result = 1;
    if (!read_file(path, &bytes, &length)) {
        (void)fprintf(stderr, "ombasic: cannot read '%s'\n", path);
        goto cleanup;
    }
    Om_Status status = om_source_text_init(
        &source, (Om_Source_Id){0}, (Om_String_View){path, strlen(path)},
        (Om_Const_Byte_Slice){bytes, length}, OM_CLI_MAX_SOURCE_BYTES);
    if (status == OM_STATUS_OK) {
        status = om_omi_parse(&source, &module);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: Native metadata validation failed (%s)\n",
                      om_status_name(status));
        goto cleanup;
    }
    for (size_t index = 0; index < module.diagnostic_count; ++index) {
        const Om_Omi_Diagnostic *diagnostic = &module.diagnostics[index];
        Om_Source_Location location = {0};
        (void)om_source_text_location(&source, diagnostic->span.start, &location);
        (void)fprintf(stderr, "%s:%u:%u: %s\n", path, location.line,
                      location.byte_column,
                      om_omi_diagnostic_code_name(diagnostic->code));
    }
    if (module.diagnostic_count == 0) {
        result = 0;
    }
cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    om_free(bytes);
    return result;
}

static const char *python_executable(void)
{
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

static int run_python_tool(int argc, char **argv,
                           const char *script_name, bool lsp)
{
    char *script_path = NULL;
    size_t argument_count = 0;
    const char *relative_script = NULL;
    if (strcmp(script_name, "bind") == 0) {
        relative_script = "share/ombasic/tools/ombasic_bind.py";
    } else {
        relative_script = "share/ombasic/tools/ombasic_lsp.py";
    }
    const Om_Status path_status = om_toolchain_resource_path(relative_script,
                                                              &script_path);
    if (path_status != OM_STATUS_OK) {
        (void)fprintf(stderr,
            "ombasic: SDK tool '%s' is missing; set OMBASIC_RESOURCE_DIR\n",
            relative_script);
        return 1;
    }
    const size_t forwarded_count = argc > 2 ? (size_t)(argc - 2) : 0;
    const size_t capacity = forwarded_count + (lsp ? 5u : 3u);
    char **owned_arguments = om_alloc_array(capacity, sizeof(*owned_arguments));
    if (owned_arguments == NULL) {
        om_free(script_path);
        (void)fprintf(stderr, "ombasic: out of memory\n");
        return 1;
    }
    (void)memset(owned_arguments, 0,
                 capacity * sizeof(*owned_arguments));
    owned_arguments[argument_count++] = (char *)python_executable();
    owned_arguments[argument_count++] = script_path;
    if (lsp) {
        owned_arguments[argument_count++] = (char *)"--compiler";
        owned_arguments[argument_count++] = argv[0];
    }
    for (int index = 2; index < argc; ++index) {
        owned_arguments[argument_count++] = argv[index];
    }
    owned_arguments[argument_count] = NULL;
    const int result = om_linker_run_executable(
        python_executable(), owned_arguments);
    if (result < 0) {
        (void)fprintf(stderr,
            "ombasic: could not start Python tooling with '%s'\n",
            python_executable());
    }
    om_free(script_path);
    om_free(owned_arguments);
    return result < 0 ? 1 : result;
}

static int bind_header(int argc, char **argv)
{
    return run_python_tool(argc, argv, "bind", false);
}

static int launch_lsp(int argc, char **argv)
{
    return run_python_tool(argc, argv, "lsp", true);
}

static int format_file(const char *path, bool write_back, bool check_only)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    Om_Formatter_Result formatted = {0};
    int exit_code = 1;
    if (!read_file(path, &bytes, &length)) {
        (void)fprintf(stderr, "ombasic: cannot read '%s'\n", path);
        goto cleanup;
    }
    Om_Status status = om_source_text_init(
        &source, (Om_Source_Id){0}, (Om_String_View){path, strlen(path)},
        (Om_Const_Byte_Slice){bytes, length}, OM_CLI_MAX_SOURCE_BYTES);
    if (status == OM_STATUS_OK) {
        status = om_lexer_scan(&source, &lexed);
    }
    if (status == OM_STATUS_OK && lexed.diagnostic_count == 0) {
        status = om_parser_parse(&lexed, &parsed);
    }
    if (status == OM_STATUS_OK && lexed.diagnostic_count == 0
        && parsed.diagnostic_count == 0) {
        status = om_formatter_format(&source, &lexed, &parsed, &formatted);
    }
    if (status != OM_STATUS_OK || lexed.diagnostic_count != 0
        || parsed.diagnostic_count != 0) {
        (void)fprintf(stderr, "ombasic: formatting requires valid syntax (%s)\n",
                      om_status_name(status));
        goto cleanup;
    }
    const bool changed = formatted.length != length
                         || memcmp(formatted.data, bytes, length) != 0;
    if (check_only) {
        if (changed) {
            (void)fprintf(stderr, "ombasic: '%s' needs formatting\n", path);
            exit_code = 1;
        } else {
            exit_code = 0;
        }
    } else if (write_back) {
        if (!write_file(path, formatted.data, formatted.length)) {
            (void)fprintf(stderr, "ombasic: cannot write '%s'\n", path);
            goto cleanup;
        }
        exit_code = 0;
    } else {
        if (fwrite(formatted.data, 1, formatted.length, stdout) != formatted.length) {
            (void)fprintf(stderr, "ombasic: failed writing formatted source\n");
            goto cleanup;
        }
        exit_code = 0;
    }
cleanup:
    om_formatter_result_destroy(&formatted);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    om_free(bytes);
    return exit_code;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "version") == 0) {
        (void)puts("OMBasic 0.1.0");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "fmt") == 0) {
        bool write_back = false;
        bool check_only = false;
        const char *path = NULL;
        for (int index = 2; index < argc; ++index) {
            if (strcmp(argv[index], "--write") == 0) {
                write_back = true;
            } else if (strcmp(argv[index], "--check") == 0) {
                check_only = true;
            } else if (path == NULL) {
                path = argv[index];
            } else {
                print_usage(stderr);
                return 2;
            }
        }
        if (path == NULL || (write_back && check_only)) {
            print_usage(stderr);
            return 2;
        }
        return format_file(path, write_back, check_only);
    }
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        return om_cli_compile_command(argc, argv, "check");
    }
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        return om_cli_compile_command(argc, argv, "run");
    }
    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        return om_cli_compile_command(argc, argv, "build");
    }
    if (argc >= 2 && strcmp(argv[1], "bind") == 0) {
        return bind_header(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "lsp") == 0) {
        return launch_lsp(argc, argv);
    }
    if (argc == 3 && strcmp(argv[1], "check-omi") == 0) {
        return check_omi_file(argv[2]);
    }
    print_usage(stderr);
    return 2;
}
