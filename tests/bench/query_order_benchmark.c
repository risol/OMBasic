#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ombasic/runtime/abi/data.h"
#include "ombasic/runtime/abi/gc.h"
#include "ombasic/runtime/embedding.h"

static const Om_Runtime_Type_Descriptor INT_TYPE = {
    .fingerprint = UINT64_C(0x5155455259494e54),
    .kind = OM_RUNTIME_TYPE_INT,
    .size = sizeof(int64_t),
    .alignment = _Alignof(int64_t),
};

static const Om_Runtime_Type_Descriptor INT_LIST_TYPE = {
    .fingerprint = UINT64_C(0x51554552594c4953),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &INT_TYPE,
};

static double elapsed_ms(clock_t start, clock_t finish)
{
    return (double)(finish - start) * 1000.0 / (double)CLOCKS_PER_SEC;
}

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (text[0] == '\0' || end == text || *end != '\0'
        || parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

int main(int argc, char **argv)
{
    size_t count = 200000;
    size_t top_k = 100;
    size_t rounds = 5;
    if ((argc > 1 && !parse_size(argv[1], &count))
        || (argc > 2 && !parse_size(argv[2], &top_k))
        || (argc > 3 && !parse_size(argv[3], &rounds))
        || argc > 4 || count == 0 || top_k == 0 || top_k > count
        || top_k > INT64_MAX || rounds == 0 || count > INT64_MAX) {
        (void)fprintf(stderr,
            "Usage: %s [row-count=200000] [top-k=100] [rounds=5]\n",
            argv[0]);
        return 2;
    }

    Om_Execution_Context *context = om_runtime_context_create(0);
    void *roots[4] = {NULL};
    Om_Runtime_Root_Frame *frame = NULL;
    int exit_code = 1;
    if (context == NULL) return 1;
    frame = om_runtime_roots_push(context, roots, 4);
    if (frame == NULL
        || om_runtime_list_create(context, &INT_LIST_TYPE, &roots[0])
            != OM_STATUS_OK
        || om_runtime_list_create(context, &INT_LIST_TYPE, &roots[1])
            != OM_STATUS_OK) {
        goto cleanup;
    }

    for (size_t index = 0; index < count; ++index) {
        const int64_t value = (int64_t)index;
        const int64_t key = (int64_t)(((uint64_t)index * UINT64_C(48271))
                                      % UINT64_C(9973));
        const Om_Runtime_Value value_item = {.integer = value};
        const Om_Runtime_Value key_item = {.integer = key};
        if (om_runtime_list_append_value(context, roots[0], &value_item)
                != OM_STATUS_OK
            || om_runtime_list_append_value(context, roots[1], &key_item)
                != OM_STATUS_OK) {
            goto cleanup;
        }
    }
    const void *key_lists[] = {roots[1]};
    const clock_t top_start = clock();
    for (size_t round = 0; round < rounds; ++round) {
        if (om_runtime_list_stable_order(context, roots[0], key_lists, 1,
                0, (int64_t)top_k, &roots[2]) != OM_STATUS_OK) {
            goto cleanup;
        }
    }
    const clock_t top_finish = clock();
    const clock_t full_start = clock();
    for (size_t round = 0; round < rounds; ++round) {
        if (om_runtime_list_stable_order(context, roots[0], key_lists, 1,
                0, INT64_MAX, &roots[3]) != OM_STATUS_OK) {
            goto cleanup;
        }
    }
    const clock_t full_finish = clock();

    size_t output_length = 0;
    if (om_runtime_list_length(context, roots[2], &output_length) != OM_STATUS_OK
        || output_length != top_k) {
        goto cleanup;
    }
    for (size_t index = 0; index < top_k; ++index) {
        Om_Runtime_Value top_value = {0};
        Om_Runtime_Value full_value = {0};
        if (om_runtime_list_get(context, roots[2], index, &top_value)
                != OM_STATUS_OK
            || om_runtime_list_get(context, roots[3], index, &full_value)
                != OM_STATUS_OK
            || top_value.integer != full_value.integer) {
            (void)fprintf(stderr,
                "Top-K result differs from stable full-sort prefix at %zu\n",
                index);
            goto cleanup;
        }
    }

    (void)printf("rows=%zu top_k=%zu rounds=%zu top_k_ms=%.3f full_sort_ms=%.3f speedup=%.2fx\n",
        count, top_k, rounds, elapsed_ms(top_start, top_finish),
        elapsed_ms(full_start, full_finish),
        elapsed_ms(full_start, full_finish)
            / (elapsed_ms(top_start, top_finish) > 0.0
                ? elapsed_ms(top_start, top_finish) : 1.0));
    exit_code = 0;

cleanup:
    if (frame != NULL) (void)om_runtime_roots_pop(context, frame);
    om_runtime_context_destroy(context);
    return exit_code;
}
