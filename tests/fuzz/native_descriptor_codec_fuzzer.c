#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "native_descriptor_codec_fuzzer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t maximum_input_size = 4096;
    if (size > maximum_input_size) return 0;
    const Om_Status status =
        om_runtime_distribution_fuzz_descriptor_codec(data, size);
    if (status != OM_STATUS_OK && status != OM_STATUS_OUT_OF_MEMORY) {
        abort();
    }
    return 0;
}
