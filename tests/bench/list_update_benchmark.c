#include "runtime_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const Om_Runtime_Type_Descriptor INT_TYPE = {
    .fingerprint = UINT64_C(0x4f4d42454e434831),
    .kind = OM_RUNTIME_TYPE_INT,
    .size = sizeof(int64_t),
    .alignment = _Alignof(int64_t),
};

static const Om_Runtime_Type_Descriptor INT_LIST_TYPE = {
    .fingerprint = UINT64_C(0x4f4d42454e434832),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &INT_TYPE,
};

static bool parse_size(const char *text, size_t *result)
{
    if (text == NULL || result == NULL || *text == '\0') return false;
    char *end = NULL;
    const unsigned long long value = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0
        || value > (unsigned long long)INT64_MAX
        || value > (unsigned long long)SIZE_MAX) {
        return false;
    }
    *result = (size_t)value;
    return true;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec)
        + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static Om_Status run_updates(size_t list_length,
                             size_t update_count,
                             bool consume,
                             double *seconds,
                             uint64_t *checksum)
{
    if (seconds == NULL || checksum == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Execution_Context *context = om_runtime_context_create(0);
    if (context == NULL) return OM_STATUS_OUT_OF_MEMORY;
    void *roots[2] = {NULL, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    Om_Status status = frame == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_create(context, &INT_LIST_TYPE,
                                                &roots[0]);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_reserve(context, roots[0],
            (int64_t)list_length, (int64_t)list_length);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < list_length;
         ++index) {
        const Om_Runtime_Value value = {.integer = (int64_t)index};
        status = om_runtime_list_builder_append_value(context, roots[0],
                                                      &value);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[0], &roots[0]);
    }

    struct timespec start = {0};
    struct timespec end = {0};
    if (status == OM_STATUS_OK && timespec_get(&start, TIME_UTC) == 0) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t update = 0; status == OM_STATUS_OK && update < update_count;
         ++update) {
        const int64_t index = (int64_t)(update % list_length);
        const Om_Runtime_Value replacement = {
            .integer = (int64_t)(update % 251),
        };
        status = consume
            ? om_runtime_list_set_consume(context, roots[0], index,
                &replacement, &roots[1])
            : om_runtime_list_set_copy(context, roots[0], index,
                &replacement, &roots[1]);
        if (status == OM_STATUS_OK) {
            roots[0] = roots[1];
            roots[1] = NULL;
        }
    }
    if (status == OM_STATUS_OK && timespec_get(&end, TIME_UTC) == 0) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        *seconds = elapsed_seconds(start, end);
        uint64_t total = 0;
        for (size_t index = 0; index < list_length; ++index) {
            Om_Runtime_Value value = {0};
            status = om_runtime_list_get(context, roots[0], index, &value);
            if (status != OM_STATUS_OK) break;
            total += (uint64_t)value.integer;
        }
        *checksum = total;
    }
    if (frame != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context, frame);
        if (status == OM_STATUS_OK) status = pop_status;
    }
    om_runtime_context_destroy(context);
    return status;
}

int main(int argc, char **argv)
{
    size_t list_length = 16384;
    size_t update_count = 1024;
    if ((argc > 3)
        || (argc > 1 && !parse_size(argv[1], &list_length))
        || (argc > 2 && !parse_size(argv[2], &update_count))) {
        (void)fprintf(stderr,
            "usage: ombasic_list_update_benchmark [list-length] [updates]\n");
        return 2;
    }

    double copy_seconds = 0.0;
    double consume_seconds = 0.0;
    uint64_t copy_checksum = 0;
    uint64_t consume_checksum = 0;
    Om_Status status = run_updates(list_length, update_count, false,
        &copy_seconds, &copy_checksum);
    if (status == OM_STATUS_OK) {
        status = run_updates(list_length, update_count, true,
            &consume_seconds, &consume_checksum);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "benchmark failed: %s\n", om_status_name(status));
        return 1;
    }
    if (copy_checksum != consume_checksum) {
        (void)fprintf(stderr, "copy and consume results differ\n");
        return 1;
    }
    (void)printf("list=%zu updates=%zu checksum=%" PRIu64 "\n",
                 list_length, update_count, copy_checksum);
    (void)printf("copy=%.3f ms consume=%.3f ms speedup=%.2fx\n",
        copy_seconds * 1000.0, consume_seconds * 1000.0,
        consume_seconds == 0.0 ? 0.0 : copy_seconds / consume_seconds);
    return 0;
}
