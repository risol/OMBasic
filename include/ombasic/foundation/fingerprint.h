#ifndef OMBASIC_FOUNDATION_FINGERPRINT_H
#define OMBASIC_FOUNDATION_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

/* Stable FNV-1a 64-bit hash for a domain-separated nominal schema. */
Om_Status om_fingerprint64_schema(
    Om_String_View domain,
    Om_String_View canonical_module_path,
    Om_String_View entity_name,
    const uint64_t *field_type_fingerprints,
    size_t field_count,
    uint64_t *fingerprint
);

#endif
