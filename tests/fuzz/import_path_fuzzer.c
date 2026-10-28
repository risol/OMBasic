#include <stddef.h>
#include <stdint.h>

#include "ombasic/source/import_path.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t maximum_input_size = 4096;
    if (size > maximum_input_size) return 0;
    (void)om_import_path_validate((Om_String_View){
        (const char *)data,
        size,
    });
    return 0;
}
