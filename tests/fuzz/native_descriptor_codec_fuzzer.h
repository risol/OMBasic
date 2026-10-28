#ifndef OMBASIC_TEST_NATIVE_DESCRIPTOR_CODEC_FUZZER_H
#define OMBASIC_TEST_NATIVE_DESCRIPTOR_CODEC_FUZZER_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"

Om_Status om_runtime_distribution_fuzz_descriptor_codec(
    const uint8_t *data,
    size_t size
);

#endif
