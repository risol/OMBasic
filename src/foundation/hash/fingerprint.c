#include "ombasic/foundation/fingerprint.h"

#include <stdbool.h>

static const uint64_t fnv1a64_offset = UINT64_C(14695981039346656037);

static uint64_t hash_bytes(uint64_t hash, const uint8_t *bytes, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8] = {0};
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        const size_t shift = (sizeof(bytes) - index - 1) * 8;
        bytes[index] = (uint8_t)(value >> shift);
    }
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_string(uint64_t hash, Om_String_View value)
{
    hash = hash_u32(hash, (uint32_t)value.length);
    return hash_bytes(hash, (const uint8_t *)value.data, value.length);
}

static bool string_is_valid(Om_String_View value, bool allow_empty)
{
    return (allow_empty || value.length != 0)
        && (value.length == 0 || value.data != NULL);
}

Om_Status om_fingerprint64_schema(
    Om_String_View domain,
    Om_String_View canonical_module_path,
    Om_String_View entity_name,
    const uint64_t *field_type_fingerprints,
    size_t field_count,
    uint64_t *fingerprint)
{
    if (fingerprint == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *fingerprint = 0;
    if (!string_is_valid(domain, false)
        || !string_is_valid(canonical_module_path, true)
        || !string_is_valid(entity_name, false)
        || (field_count != 0 && field_type_fingerprints == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (domain.length > UINT32_MAX
        || canonical_module_path.length > UINT32_MAX
        || entity_name.length > UINT32_MAX || field_count > UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }

    uint64_t hash = fnv1a64_offset;
    hash = hash_string(hash, domain);
    hash = hash_string(hash, canonical_module_path);
    hash = hash_string(hash, entity_name);
    hash = hash_u32(hash, (uint32_t)field_count);
    for (size_t index = 0; index < field_count; ++index) {
        hash = hash_u64(hash, field_type_fingerprints[index]);
    }
    *fingerprint = hash;
    return OM_STATUS_OK;
}
