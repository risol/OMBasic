#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/source/import_path.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char std_importer[] = "@std/math/math.bas";
    const size_t maximum_input_size = 4096;
    if (size > maximum_input_size) return 0;
    (void)om_import_path_validate((Om_String_View){
        (const char *)data,
        size,
    });
    Om_Resolved_Import_Path resolved = {0};
    (void)om_import_path_resolve(
        (Om_String_View){"vendor/json/json.bas",
                         sizeof("vendor/json/json.bas") - 1},
        (Om_String_View){(const char *)data, size}, &resolved);
    om_resolved_import_path_destroy(&resolved);
    Om_Resolved_Import_Path std_resolved = {0};
    (void)om_import_path_resolve(
        (Om_String_View){std_importer, sizeof(std_importer) - 1},
        (Om_String_View){(const char *)data, size}, &std_resolved);
    if (std_resolved.data != NULL) {
        if (std_resolved.length == 0
            || std_resolved.data[std_resolved.length] != '\0'
            || om_import_path_validate((Om_String_View){
                std_resolved.data, std_resolved.length}) != OM_STATUS_OK
            || std_resolved.root_kind != OM_IMPORT_ROOT_STDLIB) {
            __builtin_trap();
        }
    }
    om_resolved_import_path_destroy(&std_resolved);
    if (size >= 2) {
        const size_t split = size / 2;
        Om_Resolved_Import_Path split_resolved = {0};
        (void)om_import_path_resolve(
            (Om_String_View){(const char *)data, split},
            (Om_String_View){(const char *)data + split, size - split},
            &split_resolved);
        om_resolved_import_path_destroy(&split_resolved);
    }
    return 0;
}
