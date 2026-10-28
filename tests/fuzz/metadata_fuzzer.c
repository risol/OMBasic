#include <stddef.h>
#include <stdint.h>

#include "ombasic/native/metadata/omi.h"
#include "ombasic/source/source_text.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char path[] = "fuzz.omi";
    const size_t maximum_input_size = 1024 * 1024;
    if (size > maximum_input_size) return 0;

    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    if (om_source_text_init(&source, (Om_Source_Id){1},
            (Om_String_View){path, sizeof(path) - 1},
            (Om_Const_Byte_Slice){data, size}, maximum_input_size)
        == OM_STATUS_OK) {
        (void)om_omi_parse(&source, &module);
    }

    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return 0;
}
