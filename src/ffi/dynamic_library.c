#include "ombasic/ffi/dynamic_library.h"

#include <limits.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static Om_Status validate_c_string(Om_String_View text)
{
    if (text.data == NULL || text.length == 0
        || memchr(text.data, '\0', text.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_utf8_validate((Om_Const_Byte_Slice){
        (const uint8_t *)text.data, text.length
    });
}

Om_Status om_native_library_open(Om_Native_Library *library,
                                 Om_String_View path)
{
    if (library == NULL || library->handle != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status path_status = validate_c_string(path);
    if (path_status != OM_STATUS_OK) {
        return path_status;
    }
#if defined(_WIN32)
    if (path.length > (size_t)INT_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const int path_length = (int)path.length;
    const int wide_length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data, path_length, NULL, 0);
    if (wide_length <= 0) {
        return OM_STATUS_INVALID_UTF8;
    }
    size_t wide_count = 0;
    if (!om_checked_size_add((size_t)wide_length, 1, &wide_count)) {
        return OM_STATUS_OVERFLOW;
    }
    wchar_t *wide_path = om_alloc_array(wide_count, sizeof(*wide_path));
    if (wide_path == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data, path_length,
        wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        return OM_STATUS_INVALID_UTF8;
    }
    wide_path[wide_length] = L'\0';
    HMODULE handle = LoadLibraryW(wide_path);
    om_free(wide_path);
    if (handle == NULL) {
        return OM_STATUS_IO_ERROR;
    }
    library->handle = (void *)handle;
#else
    size_t path_size = 0;
    if (!om_checked_size_add(path.length, 1, &path_size)) {
        return OM_STATUS_OVERFLOW;
    }
    char *terminated_path = om_alloc(path_size);
    if (terminated_path == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(terminated_path, path.data, path.length);
    terminated_path[path.length] = '\0';
    void *handle = dlopen(terminated_path, RTLD_NOW | RTLD_LOCAL);
    om_free(terminated_path);
    if (handle == NULL) {
        return OM_STATUS_IO_ERROR;
    }
    library->handle = handle;
#endif
    return OM_STATUS_OK;
}

Om_Status om_native_library_lookup(const Om_Native_Library *library,
                                   Om_String_View symbol_name,
                                   void **symbol_address)
{
    if (library == NULL || library->handle == NULL
        || symbol_address == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status symbol_status = validate_c_string(symbol_name);
    if (symbol_status != OM_STATUS_OK) {
        return symbol_status;
    }
    *symbol_address = NULL;
    size_t name_size = 0;
    if (!om_checked_size_add(symbol_name.length, 1, &name_size)) {
        return OM_STATUS_OVERFLOW;
    }
    char *terminated_name = om_alloc(name_size);
    if (terminated_name == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(terminated_name, symbol_name.data, symbol_name.length);
    terminated_name[symbol_name.length] = '\0';
#if defined(_WIN32)
    FARPROC address = GetProcAddress((HMODULE)library->handle,
                                     terminated_name);
    om_free(terminated_name);
    if (address == NULL) {
        return OM_STATUS_NOT_FOUND;
    }
    _Static_assert(sizeof(address) == sizeof(*symbol_address),
                   "Windows code and data pointers must have equal size");
    (void)memcpy(symbol_address, &address, sizeof(address));
#else
    (void)dlerror();
    void *address = dlsym(library->handle, terminated_name);
    const char *error = dlerror();
    om_free(terminated_name);
    if (error != NULL) {
        return OM_STATUS_NOT_FOUND;
    }
    *symbol_address = address;
#endif
    return OM_STATUS_OK;
}

void om_native_library_close(Om_Native_Library *library)
{
    if (library == NULL || library->handle == NULL) {
        return;
    }
#if defined(_WIN32)
    (void)FreeLibrary((HMODULE)library->handle);
#else
    (void)dlclose(library->handle);
#endif
    library->handle = NULL;
}
