#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ombasic/source/file_loader.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if !defined(OM_SOURCE_TEST_FIXTURE_ROOT)
#error OM_SOURCE_TEST_FIXTURE_ROOT must name the checked-in source fixture directory
#endif

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>

static bool format_path(char *output, size_t capacity, const char *format, const char *root)
{
    const int written = snprintf(output, capacity, format, root);
    return written >= 0 && (size_t)written < capacity;
}

static bool write_test_file(const char *path, const char *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const size_t written = fwrite(bytes, 1, length, file);
    const int close_status = fclose(file);
    return written == length && close_status == 0;
}

static bool test_symlink_rejection(void)
{
    char temporary_root[64] = "/tmp/om-source-XXXXXX";
    char source_root[128] = {0};
    char outside_file[128] = {0};
    char root_file[128] = {0};
    char case_file[128] = {0};
    char escape_link[128] = {0};
    char alias_link[128] = {0};
    bool passed = false;
    Om_Source_Root root = {0};
    Om_Source_Text source = {0};

    if (mkdtemp(temporary_root) == NULL
        || !format_path(source_root, sizeof(source_root), "%s/root", temporary_root)
        || !format_path(outside_file, sizeof(outside_file), "%s/outside.bas", temporary_root)
        || mkdir(source_root, 0700) != 0
        || !format_path(root_file, sizeof(root_file), "%s/main.bas", source_root)
        || !format_path(case_file, sizeof(case_file), "%s/Wrong_case.bas", source_root)
        || !format_path(escape_link, sizeof(escape_link), "%s/escape.bas", source_root)
        || !format_path(alias_link, sizeof(alias_link), "%s/alias.bas", source_root)) {
        goto cleanup;
    }
    if (!write_test_file(outside_file, "outside", 7)) {
        goto cleanup;
    }
    if (!write_test_file(root_file, "inside", 6)) {
        goto cleanup;
    }
    if (!write_test_file(case_file, "case", 4)) {
        goto cleanup;
    }
    if (symlink(outside_file, escape_link) != 0 || symlink(root_file, alias_link) != 0) {
        goto cleanup;
    }
    if (om_source_root_init(
            &root,
            (Om_String_View){source_root, strlen(source_root)}
        ) != OM_STATUS_OK) {
        goto cleanup;
    }
    const Om_Status escape_status = om_source_root_load(
        &root,
        (Om_Source_Id){1},
        (Om_String_View){"escape.bas", 10},
        64,
        &source
    );
    const Om_Status alias_status = om_source_root_load(
        &root,
        (Om_Source_Id){2},
        (Om_String_View){"alias.bas", 9},
        64,
        &source
    );
    const Om_Status case_status = om_source_root_load(
        &root,
        (Om_Source_Id){3},
        (Om_String_View){"wrong_case.bas", 14},
        64,
        &source
    );
    passed = escape_status == OM_STATUS_PATH_ESCAPE
             && alias_status == OM_STATUS_PATH_ESCAPE
             && case_status == OM_STATUS_NOT_FOUND
             && source.owned_storage == NULL;

cleanup:
    om_source_text_destroy(&source);
    om_source_root_destroy(&root);
    if (escape_link[0] != '\0') {
        (void)unlink(escape_link);
    }
    if (alias_link[0] != '\0') {
        (void)unlink(alias_link);
    }
    if (root_file[0] != '\0') {
        (void)unlink(root_file);
    }
    if (case_file[0] != '\0') {
        (void)unlink(case_file);
    }
    if (outside_file[0] != '\0') {
        (void)unlink(outside_file);
    }
    if (source_root[0] != '\0') {
        (void)rmdir(source_root);
    }
    (void)rmdir(temporary_root);
    return passed;
}
#else
static bool test_case_mismatch(void)
{
    wchar_t temporary_directory[MAX_PATH] = {0};
    wchar_t temporary_root[MAX_PATH] = {0};
    wchar_t source_path[MAX_PATH] = {0};
    char utf8_root[MAX_PATH * 4] = {0};
    DWORD temporary_directory_length = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    Om_Source_Root root = {0};
    Om_Source_Text source = {0};
    bool passed = false;
    static const wchar_t file_suffix[] = L"\\Wrong_case.bas";

    temporary_directory_length = GetTempPathW(MAX_PATH, temporary_directory);
    if (temporary_directory_length == 0 || temporary_directory_length >= MAX_PATH
        || GetTempFileNameW(temporary_directory, L"omb", 0, temporary_root) == 0) {
        goto cleanup;
    }
    if (!DeleteFileW(temporary_root) || !CreateDirectoryW(temporary_root, NULL)) {
        goto cleanup;
    }
    const size_t root_length = wcslen(temporary_root);
    const size_t suffix_length = sizeof(file_suffix) / sizeof(file_suffix[0]);
    if (root_length + suffix_length > MAX_PATH) {
        goto cleanup;
    }
    (void)memcpy(source_path, temporary_root, root_length * sizeof(wchar_t));
    (void)memcpy(
        source_path + root_length,
        file_suffix,
        suffix_length * sizeof(wchar_t)
    );
    file = CreateFileW(
        source_path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    DWORD written = 0;
    const bool write_succeeded = WriteFile(file, "x", 1, &written, NULL) && written == 1;
    const bool close_succeeded = CloseHandle(file) != 0;
    file = INVALID_HANDLE_VALUE;
    if (!write_succeeded || !close_succeeded) {
        goto cleanup;
    }
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            temporary_root,
            -1,
            utf8_root,
            (int)sizeof(utf8_root),
            NULL,
            NULL
        ) <= 0) {
        goto cleanup;
    }
    if (om_source_root_init(
            &root,
            (Om_String_View){utf8_root, strlen(utf8_root)}
        ) != OM_STATUS_OK) {
        goto cleanup;
    }
    const Om_Status load_status = om_source_root_load(
        &root,
        (Om_Source_Id){20},
        (Om_String_View){"wrong_case.bas", 14},
        64,
        &source
    );
    passed = load_status == OM_STATUS_NOT_FOUND && source.owned_storage == NULL;

cleanup:
    if (file != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(file);
    }
    om_source_text_destroy(&source);
    om_source_root_destroy(&root);
    (void)DeleteFileW(source_path);
    (void)RemoveDirectoryW(temporary_root);
    return passed;
}
#endif

bool om_test_file_loader(void)
{
    static const char fixture_root[] = OM_SOURCE_TEST_FIXTURE_ROOT;
    Om_Source_Root root = {0};
    Om_Source_Text source = {0};
    Om_Source_Text limited = {0};
    bool passed = false;
    const Om_String_View root_path = {fixture_root, sizeof(fixture_root) - 1};

    if (om_source_root_init(&root, root_path) != OM_STATUS_OK) {
        goto cleanup;
    }
    if (om_source_root_load(
            &root,
            (Om_Source_Id){10},
            (Om_String_View){"ordinary_lower_snake.bas", 24},
            1024,
            &source
        ) != OM_STATUS_OK) {
        goto cleanup;
    }
    if (source.bytes.length < 6 || memcmp(source.bytes.data, "ACTION", 6) != 0
        || !om_string_view_equal(source.canonical_path,
                                 (Om_String_View){"ordinary_lower_snake.bas", 24})) {
        goto cleanup;
    }
    if (om_source_root_load(
            &root,
            (Om_Source_Id){11},
            (Om_String_View){"ordinary_lower_snake.bas", 24},
            1,
            &limited
        ) != OM_STATUS_OUT_OF_RANGE
        || limited.owned_storage != NULL) {
        goto cleanup;
    }
    if (om_source_root_load(
            &root,
            (Om_Source_Id){12},
            (Om_String_View){"missing.bas", 11},
            1024,
            &limited
        ) != OM_STATUS_NOT_FOUND) {
        goto cleanup;
    }
    if (om_source_root_load(
            &root,
            (Om_Source_Id){13},
            (Om_String_View){"../ordinary_lower_snake.bas", 27},
            1024,
            &limited
        ) != OM_STATUS_INVALID_ARGUMENT) {
        goto cleanup;
    }
#if !defined(_WIN32)
    if (!test_symlink_rejection()) {
        goto cleanup;
    }
#else
    if (!test_case_mismatch()) {
        goto cleanup;
    }
#endif
    passed = true;

cleanup:
    om_source_text_destroy(&limited);
    om_source_text_destroy(&source);
    om_source_root_destroy(&root);
    OM_TEST_ASSERT(passed);
    return true;
}
