#include "test.h"

#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/source/source_text.h"
#include "ombasic/source/import_path.h"

bool om_test_source_text(void)
{
    static const char path_bytes[] = "main.bas";
    static const uint8_t source_bytes[] = {
        UINT8_C(0x61), UINT8_C(0x0d), UINT8_C(0x0a), UINT8_C(0x62), UINT8_C(0x0a)
    };
    const Om_String_View path = {path_bytes, sizeof(path_bytes) - 1};
    const Om_Const_Byte_Slice input = {source_bytes, sizeof(source_bytes)};
    Om_Source_Text source = {0};
    Om_Source_Location location = {0};
    Om_Const_Byte_Slice selected = {0};
    const Om_Source_Id id = {7};

    OM_TEST_ASSERT(om_source_text_init(&source, id, path, input, 16) == OM_STATUS_OK);
    OM_TEST_ASSERT(source.id.value == id.value);
    OM_TEST_ASSERT(source.canonical_path.length == sizeof(path_bytes) - 1);
    OM_TEST_ASSERT(source.bytes.length == sizeof(source_bytes));
    OM_TEST_ASSERT(source.line_count == 3);
    OM_TEST_ASSERT(source.line_starts[0] == 0);
    OM_TEST_ASSERT(source.line_starts[1] == 3);
    OM_TEST_ASSERT(source.line_starts[2] == 5);
    OM_TEST_ASSERT(source.content_hash != 0);

    OM_TEST_ASSERT(om_source_text_location(&source, 0, &location) == OM_STATUS_OK);
    OM_TEST_ASSERT(location.line == 1 && location.byte_column == 1);
    OM_TEST_ASSERT(om_source_text_location(&source, 2, &location) == OM_STATUS_OK);
    OM_TEST_ASSERT(location.line == 1 && location.byte_column == 3);
    OM_TEST_ASSERT(om_source_text_location(&source, 3, &location) == OM_STATUS_OK);
    OM_TEST_ASSERT(location.line == 2 && location.byte_column == 1);
    OM_TEST_ASSERT(om_source_text_location(&source, 5, &location) == OM_STATUS_OK);
    OM_TEST_ASSERT(location.line == 3 && location.byte_column == 1);
    OM_TEST_ASSERT(om_source_text_location(&source, 6, &location) == OM_STATUS_OUT_OF_RANGE);
    OM_TEST_ASSERT(location.line == 0 && location.byte_column == 0);

    OM_TEST_ASSERT(om_source_text_slice(
                       &source,
                       (Om_Source_Span){.source = id, .start = 3, .end = 4},
                       &selected
                   ) == OM_STATUS_OK);
    OM_TEST_ASSERT(selected.length == 1 && selected.data[0] == UINT8_C(0x62));
    OM_TEST_ASSERT(om_source_text_slice(
                       &source,
                       (Om_Source_Span){.source = {8}, .start = 3, .end = 4},
                       &selected
                   ) == OM_STATUS_INVALID_ARGUMENT);
    OM_TEST_ASSERT(selected.data == NULL && selected.length == 0);
    OM_TEST_ASSERT(om_source_text_slice(
                       &source,
                       (Om_Source_Span){.source = id, .start = 0, .end = 6},
                       &selected
                   ) == OM_STATUS_OUT_OF_RANGE);
    om_source_text_destroy(&source);
    OM_TEST_ASSERT(source.id.value == UINT32_MAX && source.owned_storage == NULL);

    OM_TEST_ASSERT(om_source_text_init(&source, id, path, input, 4) == OM_STATUS_OUT_OF_RANGE);
    OM_TEST_ASSERT(source.owned_storage == NULL);
    OM_TEST_ASSERT(om_source_text_init(
                       &source,
                       id,
                       (Om_String_View){"empty.bas", 9},
                       (Om_Const_Byte_Slice){NULL, 0},
                       0
                   ) == OM_STATUS_OK);
    OM_TEST_ASSERT(source.line_count == 1 && source.line_starts[0] == 0);
    OM_TEST_ASSERT(source.bytes.length == 0 && source.bytes.data != NULL);
    om_source_text_destroy(&source);
    return true;
}

bool om_test_import_path(void)
{
    static const unsigned char alphabet[] = {
        (unsigned char)'a', (unsigned char)'Z', (unsigned char)'0', (unsigned char)'_',
        (unsigned char)'/', (unsigned char)'.', (unsigned char)'\\', (unsigned char)'-',
        UINT8_C(0x00), UINT8_C(0xff), (unsigned char)'b'
    };
    static const char *const valid_paths[] = {
        "main.bas",
        "storage/database.bas",
        "sales/customer2_record.bas",
        "a/b/c.bas"
    };
    uint32_t random_state = UINT32_C(0xa341316c);
    unsigned char candidate[64] = {0};
    static const char *const invalid_paths[] = {
        "",
        "/main.bas",
        "\\main.bas",
        "./main.bas",
        "storage/../main.bas",
        "storage//main.bas",
        "storage/",
        "Main.bas",
        "storage/Database.bas",
        "storage/main.BAS",
        "storage/main",
        "storage/.bas",
        "storage/2main.bas",
        "storage/a__b.bas",
        "storage/a_.bas",
        "storage/a-b.bas"
    };

    for (size_t index = 0; index < sizeof(valid_paths) / sizeof(valid_paths[0]); ++index) {
        const char *const path = valid_paths[index];
        OM_TEST_ASSERT(om_import_path_validate((Om_String_View){path, strlen(path)})
                       == OM_STATUS_OK);
    }
    for (size_t index = 0; index < sizeof(invalid_paths) / sizeof(invalid_paths[0]); ++index) {
        const char *const path = invalid_paths[index];
        OM_TEST_ASSERT(om_import_path_validate((Om_String_View){path, strlen(path)})
                       == OM_STATUS_INVALID_ARGUMENT);
    }
    struct Resolve_Case {
        const char *importer;
        const char *literal;
        const char *expected;
    };
    static const struct Resolve_Case resolve_cases[] = {
        {"main.bas", "./foo.bas", "foo.bas"},
        {"vendor/json/json.bas", "./internal/parser.bas",
         "vendor/json/internal/parser.bas"},
        {"vendor/json/json.bas", "vendor/json/internal/parser.bas",
         "vendor/json/internal/parser.bas"},
    };
    for (size_t index = 0;
         index < sizeof(resolve_cases) / sizeof(resolve_cases[0]); ++index) {
        const struct Resolve_Case *test_case = &resolve_cases[index];
        Om_Resolved_Import_Path resolved = {0};
        OM_TEST_ASSERT(om_import_path_resolve(
            (Om_String_View){test_case->importer, strlen(test_case->importer)},
            (Om_String_View){test_case->literal, strlen(test_case->literal)},
            &resolved) == OM_STATUS_OK);
        OM_TEST_ASSERT(resolved.length == strlen(test_case->expected));
        OM_TEST_ASSERT(memcmp(resolved.data, test_case->expected,
                              resolved.length) == 0);
        om_resolved_import_path_destroy(&resolved);
        OM_TEST_ASSERT(resolved.data == NULL && resolved.length == 0);
    }
    static const char *const invalid_literals[] = {
        "./../outside.bas", "././foo.bas", "./", "./internal/../foo.bas",
    };
    for (size_t index = 0;
         index < sizeof(invalid_literals) / sizeof(invalid_literals[0]); ++index) {
        Om_Resolved_Import_Path resolved = {0};
        const char *const literal = invalid_literals[index];
        OM_TEST_ASSERT(om_import_path_resolve(
            (Om_String_View){"vendor/json/json.bas",
                             sizeof("vendor/json/json.bas") - 1},
            (Om_String_View){literal, strlen(literal)}, &resolved)
            == OM_STATUS_INVALID_ARGUMENT);
        OM_TEST_ASSERT(resolved.data == NULL && resolved.length == 0);
    }
    OM_TEST_ASSERT(om_import_path_validate((Om_String_View){"./foo.bas", 9})
                   == OM_STATUS_INVALID_ARGUMENT);
    OM_TEST_ASSERT(om_import_path_validate((Om_String_View){NULL, 0})
                   == OM_STATUS_INVALID_ARGUMENT);
    for (size_t iteration = 0; iteration < 4096; ++iteration) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        const size_t length = (size_t)(random_state % (uint32_t)sizeof(candidate));
        for (size_t index = 0; index < length; ++index) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            candidate[index] = alphabet[random_state % (uint32_t)sizeof(alphabet)];
        }
        const Om_Status status = om_import_path_validate((Om_String_View){
            (const char *)candidate,
            length
        });
        OM_TEST_ASSERT(status == OM_STATUS_OK || status == OM_STATUS_INVALID_ARGUMENT);
    }
    return true;
}
