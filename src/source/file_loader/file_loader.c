#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "ombasic/source/file_loader.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"
#include "ombasic/source/import_path.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct Om_Source_Root_Storage {
    char *utf8_path;
    size_t utf8_length;
#if defined(_WIN32)
    wchar_t *wide_path;
    size_t wide_length;
    HANDLE root_handle;
#else
    int root_descriptor;
#endif
} Om_Source_Root_Storage;

static void root_storage_destroy(Om_Source_Root_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    om_free(storage->utf8_path);
#if defined(_WIN32)
    om_free(storage->wide_path);
    if (storage->root_handle != NULL && storage->root_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(storage->root_handle);
    }
#else
    if (storage->root_descriptor >= 0) {
        (void)close(storage->root_descriptor);
    }
#endif
    om_free(storage);
}

static Om_Status validate_root_path(Om_String_View path)
{
    if (!om_string_view_is_valid(path) || path.length == 0
        || memchr(path.data, '\0', path.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_utf8_validate((Om_Const_Byte_Slice){
        (const uint8_t *)path.data,
        path.length
    });
}

#if defined(_WIN32)
static Om_Status windows_error_status(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return OM_STATUS_NOT_FOUND;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return OM_STATUS_OUT_OF_MEMORY;
    case ERROR_DIRECTORY:
        return OM_STATUS_NOT_A_DIRECTORY;
    default:
        return OM_STATUS_IO_ERROR;
    }
}

static Om_Status utf8_to_wide(Om_String_View input, wchar_t **output, size_t *output_length)
{
    int required = 0;
    size_t allocation_count = 0;
    wchar_t *wide = NULL;
    if (input.length > (size_t)INT_MAX || output == NULL || output_length == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *output = NULL;
    *output_length = 0;
    required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data,
        (int)input.length,
        NULL,
        0
    );
    if (required <= 0) {
        return OM_STATUS_INVALID_UTF8;
    }
    if (!om_checked_size_add((size_t)required, 1, &allocation_count)) {
        return OM_STATUS_OVERFLOW;
    }
    wide = om_alloc_array(allocation_count, sizeof(*wide));
    if (wide == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            input.data,
            (int)input.length,
            wide,
            required
        ) != required) {
        om_free(wide);
        return OM_STATUS_INVALID_UTF8;
    }
    wide[required] = L'\0';
    *output = wide;
    *output_length = (size_t)required;
    return OM_STATUS_OK;
}

static Om_Status windows_final_path(HANDLE handle, wchar_t **output, size_t *output_length)
{
    DWORD required = 0;
    DWORD written = 0;
    size_t allocation_count = 0;
    wchar_t *wide = NULL;
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    if (output == NULL || output_length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    *output_length = 0;
    required = GetFinalPathNameByHandleW(handle, NULL, 0, flags);
    if (required == 0) {
        return windows_error_status(GetLastError());
    }
    if (required == MAXDWORD) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (!om_checked_size_add((size_t)required, 1, &allocation_count)) {
        return OM_STATUS_OVERFLOW;
    }
    wide = om_alloc_array(allocation_count, sizeof(*wide));
    if (wide == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    written = GetFinalPathNameByHandleW(handle, wide, (DWORD)allocation_count, flags);
    if (written == 0 || written >= allocation_count) {
        const DWORD error = GetLastError();
        om_free(wide);
        return error == ERROR_SUCCESS ? OM_STATUS_IO_ERROR : windows_error_status(error);
    }
    wide[written] = L'\0';
    *output = wide;
    *output_length = (size_t)written;
    return OM_STATUS_OK;
}

static Om_Status wide_to_utf8(const wchar_t *wide, char **output, size_t *output_length)
{
    int required = 0;
    char *utf8 = NULL;
    if (wide == NULL || output == NULL || output_length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    *output_length = 0;
    required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (required <= 0) {
        return OM_STATUS_INVALID_UTF8;
    }
    utf8 = om_alloc((size_t)required);
    if (utf8 == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide,
            -1,
            utf8,
            required,
            NULL,
            NULL
        ) != required) {
        om_free(utf8);
        return OM_STATUS_INVALID_UTF8;
    }
    *output = utf8;
    *output_length = (size_t)required - 1;
    return OM_STATUS_OK;
}

static Om_Status windows_expected_path(
    const Om_Source_Root_Storage *root,
    Om_String_View module_path,
    wchar_t **output,
    size_t *output_length
)
{
    size_t length = 0;
    size_t allocation_count = 0;
    bool needs_separator = true;
    wchar_t *expected = NULL;
    if (!om_checked_size_add(root->wide_length, module_path.length, &length)) {
        return OM_STATUS_OVERFLOW;
    }
    if (root->wide_length != 0) {
        const wchar_t last = root->wide_path[root->wide_length - 1];
        needs_separator = last != L'\\' && last != L'/';
    }
    if (needs_separator && !om_checked_size_add(length, 1, &length)) {
        return OM_STATUS_OVERFLOW;
    }
    if (!om_checked_size_add(length, 1, &allocation_count)) {
        return OM_STATUS_OVERFLOW;
    }
    expected = om_alloc_array(allocation_count, sizeof(*expected));
    if (expected == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < root->wide_length; ++index) {
        expected[index] = root->wide_path[index];
    }
    size_t write_index = root->wide_length;
    if (needs_separator) {
        expected[write_index++] = L'\\';
    }
    for (size_t index = 0; index < module_path.length; ++index) {
        expected[write_index++] = module_path.data[index] == '/' ? L'\\'
                                                                 : (wchar_t)(unsigned char)module_path.data[index];
    }
    expected[write_index] = L'\0';
    *output = expected;
    *output_length = write_index;
    return OM_STATUS_OK;
}

static Om_Status windows_root_is_current(const Om_Source_Root_Storage *root)
{
    HANDLE current = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION expected_info = {0};
    BY_HANDLE_FILE_INFORMATION current_info = {0};
    current = CreateFileW(
        root->wide_path,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );
    if (current == INVALID_HANDLE_VALUE) {
        return windows_error_status(GetLastError());
    }
    if (!GetFileInformationByHandle(root->root_handle, &expected_info)
        || !GetFileInformationByHandle(current, &current_info)) {
        const DWORD error = GetLastError();
        (void)CloseHandle(current);
        return windows_error_status(error);
    }
    (void)CloseHandle(current);
    if (expected_info.dwVolumeSerialNumber != current_info.dwVolumeSerialNumber
        || expected_info.nFileIndexHigh != current_info.nFileIndexHigh
        || expected_info.nFileIndexLow != current_info.nFileIndexLow) {
        return OM_STATUS_PATH_ESCAPE;
    }
    return OM_STATUS_OK;
}

static Om_Status read_windows_file(HANDLE handle, size_t maximum_byte_length,
                                  uint8_t **output, size_t *output_length)
{
    LARGE_INTEGER file_size = {0};
    uint8_t *buffer = NULL;
    size_t length = 0;
    if (!GetFileSizeEx(handle, &file_size)) {
        return windows_error_status(GetLastError());
    }
    if (file_size.QuadPart < 0
        || (uint64_t)file_size.QuadPart > (uint64_t)maximum_byte_length
        || (uint64_t)file_size.QuadPart >= (uint64_t)UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    length = (size_t)file_size.QuadPart;
    buffer = om_alloc(length == 0 ? 1 : length);
    if (buffer == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = length - offset;
        const DWORD request = remaining > (size_t)UINT32_MAX
                                  ? UINT32_MAX
                                  : (DWORD)remaining;
        DWORD received = 0;
        if (!ReadFile(handle, buffer + offset, request, &received, NULL)) {
            om_free(buffer);
            return windows_error_status(GetLastError());
        }
        if (received == 0) {
            om_free(buffer);
            return OM_STATUS_IO_ERROR;
        }
        offset += (size_t)received;
    }
    uint8_t extra = 0;
    DWORD received_extra = 0;
    if (!ReadFile(handle, &extra, 1, &received_extra, NULL)) {
        om_free(buffer);
        return windows_error_status(GetLastError());
    }
    if (received_extra != 0) {
        om_free(buffer);
        return OM_STATUS_IO_ERROR;
    }
    *output = buffer;
    *output_length = length;
    return OM_STATUS_OK;
}
#else
static Om_Status errno_status(int error)
{
    switch (error) {
    case ENOENT:
        return OM_STATUS_NOT_FOUND;
    case ENOMEM:
        return OM_STATUS_OUT_OF_MEMORY;
    case ELOOP:
        return OM_STATUS_PATH_ESCAPE;
    case ENOTDIR:
        return OM_STATUS_NOT_A_DIRECTORY;
    case EACCES:
    case EPERM:
        return OM_STATUS_IO_ERROR;
    default:
        return OM_STATUS_IO_ERROR;
    }
}

static Om_Status read_posix_file(int descriptor, size_t maximum_byte_length,
                                 uint8_t **output, size_t *output_length)
{
    struct stat attributes = {0};
    uint8_t *buffer = NULL;
    size_t length = 0;
    if (fstat(descriptor, &attributes) != 0) {
        return errno_status(errno);
    }
    if (!S_ISREG(attributes.st_mode)) {
        return S_ISDIR(attributes.st_mode) ? OM_STATUS_NOT_A_DIRECTORY
                                           : OM_STATUS_INVALID_ARGUMENT;
    }
    if (attributes.st_size < 0
        || (uintmax_t)attributes.st_size > (uintmax_t)maximum_byte_length
        || (uintmax_t)attributes.st_size >= (uintmax_t)UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    length = (size_t)attributes.st_size;
    buffer = om_alloc(length == 0 ? 1 : length);
    if (buffer == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t offset = 0;
    while (offset < length) {
        size_t request = length - offset;
        if (request > (size_t)SSIZE_MAX) {
            request = (size_t)SSIZE_MAX;
        }
        const ssize_t received = read(descriptor, buffer + offset, request);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            om_free(buffer);
            return errno_status(errno);
        }
        if (received == 0) {
            om_free(buffer);
            return OM_STATUS_IO_ERROR;
        }
        offset += (size_t)received;
    }
    uint8_t extra = 0;
    ssize_t received_extra = 0;
    do {
        received_extra = read(descriptor, &extra, 1);
    } while (received_extra < 0 && errno == EINTR);
    if (received_extra < 0) {
        om_free(buffer);
        return errno_status(errno);
    }
    if (received_extra != 0) {
        om_free(buffer);
        return OM_STATUS_IO_ERROR;
    }
    *output = buffer;
    *output_length = length;
    return OM_STATUS_OK;
}

static Om_Status require_exact_directory_entry(int directory, const char *name)
{
    int duplicate = -1;
    DIR *stream = NULL;
    bool found = false;
    int read_error = 0;
    duplicate = openat(directory, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (duplicate < 0) {
        return errno_status(errno);
    }
    stream = fdopendir(duplicate);
    if (stream == NULL) {
        const int error = errno;
        (void)close(duplicate);
        return errno_status(error);
    }
    errno = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, name) == 0) {
            found = true;
            break;
        }
    }
    if (entry == NULL) {
        read_error = errno;
    }
    (void)closedir(stream);
    if (read_error != 0) {
        return errno_status(read_error);
    }
    return found ? OM_STATUS_OK : OM_STATUS_NOT_FOUND;
}

static Om_Status open_relative_posix(
    const Om_Source_Root_Storage *root,
    Om_String_View module_path,
    int *file_descriptor
)
{
    size_t path_storage_size = 0;
    char *path_copy = NULL;
    int current = -1;
    Om_Status status = OM_STATUS_OK;
    if (!om_checked_size_add(module_path.length, 1, &path_storage_size)) {
        return OM_STATUS_OVERFLOW;
    }
    path_copy = om_alloc(path_storage_size);
    if (path_copy == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(path_copy, module_path.data, module_path.length);
    path_copy[module_path.length] = '\0';
    current = fcntl(root->root_descriptor, F_DUPFD_CLOEXEC, 0);
    if (current < 0) {
        status = errno_status(errno);
        goto cleanup;
    }

    char *segment = path_copy;
    for (size_t index = 0; index <= module_path.length; ++index) {
        if (path_copy[index] != '/' && path_copy[index] != '\0') {
            continue;
        }
        const bool is_final = path_copy[index] == '\0';
        path_copy[index] = '\0';
        status = require_exact_directory_entry(current, segment);
        if (status != OM_STATUS_OK) {
            goto cleanup;
        }
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
        if (!is_final) {
            flags |= O_DIRECTORY;
        }
        const int next = openat(current, segment, flags);
        if (next < 0) {
            status = errno_status(errno);
            goto cleanup;
        }
        (void)close(current);
        current = next;
        if (is_final) {
            *file_descriptor = current;
            current = -1;
            status = OM_STATUS_OK;
            goto cleanup;
        }
        segment = path_copy + index + 1;
    }
    status = OM_STATUS_IO_ERROR;

cleanup:
    if (current >= 0) {
        (void)close(current);
    }
    om_free(path_copy);
    return status;
}
#endif

Om_Status om_source_root_init(Om_Source_Root *root, Om_String_View path)
{
    Om_Status status = OM_STATUS_OK;
    Om_Source_Root_Storage *storage = NULL;
    if (root == NULL || root->owned_storage != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = validate_root_path(path);
    if (status != OM_STATUS_OK) {
        return status;
    }
    storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Source_Root_Storage){0};
#if defined(_WIN32)
    storage->root_handle = INVALID_HANDLE_VALUE;
#else
    storage->root_descriptor = -1;
#endif

#if defined(_WIN32)
    wchar_t *input_path = NULL;
    size_t input_length = 0;
    wchar_t *absolute_path = NULL;
    wchar_t *final_path = NULL;
    size_t final_length = 0;
    char *utf8_path = NULL;
    size_t utf8_length = 0;
    HANDLE directory = INVALID_HANDLE_VALUE;
    DWORD full_length = 0;
    FILE_ATTRIBUTE_TAG_INFO attributes = {0};

    status = utf8_to_wide(path, &input_path, &input_length);
    if (status != OM_STATUS_OK) {
        goto windows_cleanup;
    }
    (void)input_length;
    full_length = GetFullPathNameW(input_path, 0, NULL, NULL);
    if (full_length == 0) {
        status = windows_error_status(GetLastError());
        goto windows_cleanup;
    }
    if (full_length == MAXDWORD) {
        status = OM_STATUS_OUT_OF_RANGE;
        goto windows_cleanup;
    }
    absolute_path = om_alloc_array((size_t)full_length + 1, sizeof(*absolute_path));
    if (absolute_path == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto windows_cleanup;
    }
    const DWORD absolute_length = GetFullPathNameW(
        input_path,
        full_length + 1,
        absolute_path,
        NULL
    );
    if (absolute_length == 0 || absolute_length > full_length) {
        status = windows_error_status(GetLastError());
        goto windows_cleanup;
    }
    directory = CreateFileW(
        absolute_path,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );
    if (directory == INVALID_HANDLE_VALUE) {
        status = windows_error_status(GetLastError());
        goto windows_cleanup;
    }
    if (!GetFileInformationByHandleEx(
            directory,
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)
        )) {
        status = windows_error_status(GetLastError());
        goto windows_cleanup;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        status = OM_STATUS_NOT_A_DIRECTORY;
        goto windows_cleanup;
    }
    status = windows_final_path(directory, &final_path, &final_length);
    if (status != OM_STATUS_OK) {
        goto windows_cleanup;
    }
    status = wide_to_utf8(final_path, &utf8_path, &utf8_length);
    if (status != OM_STATUS_OK) {
        goto windows_cleanup;
    }
    storage->wide_path = final_path;
    storage->wide_length = final_length;
    storage->utf8_path = utf8_path;
    storage->utf8_length = utf8_length;
    storage->root_handle = directory;
    directory = INVALID_HANDLE_VALUE;
    final_path = NULL;
    utf8_path = NULL;
    root->canonical_path = (Om_String_View){storage->utf8_path, storage->utf8_length};
    root->owned_storage = storage;
    storage = NULL;

windows_cleanup:
    if (directory != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(directory);
    }
    om_free(input_path);
    om_free(absolute_path);
    om_free(final_path);
    om_free(utf8_path);
#else
    size_t path_storage_size = 0;
    char *input_path = NULL;
    char *canonical_path = NULL;
    struct stat attributes = {0};
    if (!om_checked_size_add(path.length, 1, &path_storage_size)) {
        status = OM_STATUS_OVERFLOW;
        goto posix_cleanup;
    }
    input_path = om_alloc(path_storage_size);
    if (input_path == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto posix_cleanup;
    }
    (void)memcpy(input_path, path.data, path.length);
    input_path[path.length] = '\0';
    canonical_path = realpath(input_path, NULL);
    if (canonical_path == NULL) {
        status = errno_status(errno);
        goto posix_cleanup;
    }
    if (stat(canonical_path, &attributes) != 0) {
        status = errno_status(errno);
        goto posix_cleanup;
    }
    if (!S_ISDIR(attributes.st_mode)) {
        status = OM_STATUS_NOT_A_DIRECTORY;
        goto posix_cleanup;
    }
    const int root_descriptor = open(
        canonical_path,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (root_descriptor < 0) {
        status = errno_status(errno);
        goto posix_cleanup;
    }
    struct stat opened_attributes = {0};
    storage->root_descriptor = root_descriptor;
    if (fstat(root_descriptor, &opened_attributes) != 0) {
        status = errno_status(errno);
        goto posix_cleanup;
    }
    if (attributes.st_dev != opened_attributes.st_dev || attributes.st_ino != opened_attributes.st_ino) {
        status = OM_STATUS_PATH_ESCAPE;
        goto posix_cleanup;
    }
    storage->utf8_length = strlen(canonical_path);
    status = om_utf8_validate((Om_Const_Byte_Slice){
        (const uint8_t *)canonical_path,
        storage->utf8_length
    });
    if (status != OM_STATUS_OK) {
        goto posix_cleanup;
    }
    storage->utf8_path = canonical_path;
    canonical_path = NULL;
    root->canonical_path = (Om_String_View){storage->utf8_path, storage->utf8_length};
    root->owned_storage = storage;
    storage = NULL;

posix_cleanup:
    om_free(input_path);
    om_free(canonical_path);
#endif
    root_storage_destroy(storage);
    return status;
}

void om_source_root_destroy(Om_Source_Root *root)
{
    if (root == NULL) {
        return;
    }
    root_storage_destroy(root->owned_storage);
    *root = (Om_Source_Root){0};
}

Om_Status om_source_root_load_file(
    const Om_Source_Root *root,
    Om_Source_Id source_id,
    Om_String_View path,
    Om_String_View required_extension,
    size_t maximum_byte_length,
    Om_Source_Text *source
)
{
    Om_Status status = OM_STATUS_OK;
    Om_Source_Root_Storage *storage = NULL;
    uint8_t *bytes = NULL;
    size_t byte_length = 0;
    size_t max_bytes = maximum_byte_length == 0
                           ? OM_SOURCE_TEXT_DEFAULT_MAX_BYTES
                           : maximum_byte_length;
    if (root == NULL || root->owned_storage == NULL || source == NULL
        || source->owned_storage != NULL || source_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_String_View filesystem_path = path;
    if (required_extension.length == 4 && required_extension.data != NULL
        && memcmp(required_extension.data, ".bas", 4) == 0
        && path.length > 5 && path.data != NULL
        && memcmp(path.data, "@std/", 5) == 0) {
        filesystem_path.data += 5;
        filesystem_path.length -= 5;
    }
    status = om_source_path_validate(filesystem_path, required_extension);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (max_bytes >= UINT32_MAX) {
        max_bytes = (size_t)UINT32_MAX - 1;
    }
    storage = root->owned_storage;

#if defined(_WIN32)
    wchar_t *expected_path = NULL;
    size_t expected_length = 0;
    wchar_t *actual_path = NULL;
    size_t actual_length = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    FILE_ATTRIBUTE_TAG_INFO attributes = {0};
    status = windows_expected_path(storage, filesystem_path, &expected_path, &expected_length);
    if (status != OM_STATUS_OK) {
        goto windows_load_cleanup;
    }
    status = windows_root_is_current(storage);
    if (status != OM_STATUS_OK) {
        goto windows_load_cleanup;
    }
    (void)expected_length;
    file = CreateFileW(
        expected_path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) {
        status = windows_error_status(GetLastError());
        goto windows_load_cleanup;
    }
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        status = windows_error_status(GetLastError());
        goto windows_load_cleanup;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        status = OM_STATUS_PATH_ESCAPE;
        goto windows_load_cleanup;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        status = OM_STATUS_NOT_A_DIRECTORY;
        goto windows_load_cleanup;
    }
    status = windows_final_path(file, &actual_path, &actual_length);
    if (status != OM_STATUS_OK) {
        goto windows_load_cleanup;
    }
    if (actual_length > (size_t)INT_MAX || expected_length > (size_t)INT_MAX) {
        status = OM_STATUS_OUT_OF_RANGE;
        goto windows_load_cleanup;
    }
    if (actual_length != expected_length
        || CompareStringOrdinal(
               actual_path,
               (int)actual_length,
               expected_path,
               (int)expected_length,
               TRUE
           ) != CSTR_EQUAL) {
        status = OM_STATUS_PATH_ESCAPE;
        goto windows_load_cleanup;
    }
    if (CompareStringOrdinal(
            actual_path,
            (int)actual_length,
            expected_path,
            (int)expected_length,
            FALSE
        ) != CSTR_EQUAL) {
        status = OM_STATUS_NOT_FOUND;
        goto windows_load_cleanup;
    }
    status = read_windows_file(file, max_bytes, &bytes, &byte_length);
    if (status != OM_STATUS_OK) {
        goto windows_load_cleanup;
    }
    status = om_source_text_init(
        source,
        source_id,
        path,
        (Om_Const_Byte_Slice){bytes, byte_length},
        max_bytes
    );

windows_load_cleanup:
    if (file != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(file);
    }
    om_free(expected_path);
    om_free(actual_path);
#else
    int file = -1;
    status = open_relative_posix(storage, filesystem_path, &file);
    if (status != OM_STATUS_OK) {
        goto posix_load_cleanup;
    }
    status = read_posix_file(file, max_bytes, &bytes, &byte_length);
    if (status != OM_STATUS_OK) {
        goto posix_load_cleanup;
    }
    status = om_source_text_init(
        source,
        source_id,
        path,
        (Om_Const_Byte_Slice){bytes, byte_length},
        max_bytes
    );

posix_load_cleanup:
    if (file >= 0) {
        (void)close(file);
    }
#endif
    om_free(bytes);
    return status;
}

Om_Status om_source_root_load(
    const Om_Source_Root *root,
    Om_Source_Id source_id,
    Om_String_View module_path,
    size_t maximum_byte_length,
    Om_Source_Text *source)
{
    return om_source_root_load_file(
        root, source_id, module_path, (Om_String_View){".bas", 4},
        maximum_byte_length, source);
}
