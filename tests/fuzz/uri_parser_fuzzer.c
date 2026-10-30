#include "ombasic/foundation/string_view.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *om_runtime_uri_remove_dot_segments(const Om_String_View *input);
int64_t om_runtime_uri_ascii_status(const Om_String_View *input);
int64_t om_runtime_uri_ascii_offset(const Om_String_View *input);
int64_t om_runtime_uri_percent_syntax_status(const Om_String_View *input);
int64_t om_runtime_uri_percent_syntax_offset(const Om_String_View *input);
int64_t om_runtime_uri_percent_decode_status(const Om_String_View *input);
int64_t om_runtime_uri_percent_decode_offset(const Om_String_View *input);
Om_String_View *om_runtime_uri_percent_decode(const Om_String_View *input);
void om_runtime_uri_slice_release(void *value);
void om_runtime_uri_percent_release(void *value);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024 * 1024) return 0;
    Om_String_View input = {(const char *)data, size};
    (void)om_runtime_uri_ascii_status(&input);
    (void)om_runtime_uri_ascii_offset(&input);
    (void)om_runtime_uri_percent_syntax_status(&input);
    (void)om_runtime_uri_percent_syntax_offset(&input);
    (void)om_runtime_uri_percent_decode_status(&input);
    (void)om_runtime_uri_percent_decode_offset(&input);
    char *normalized = om_runtime_uri_remove_dot_segments(&input);
    om_runtime_uri_percent_release(normalized);
    Om_String_View *decoded = om_runtime_uri_percent_decode(&input);
    om_runtime_uri_slice_release(decoded);
    return 0;
}
