#include "ombasic/source/span.h"

bool om_source_span_is_valid(Om_Source_Span span, uint32_t source_length)
{
    return span.source.value != UINT32_MAX
           && span.start <= span.end
           && span.end <= source_length;
}
