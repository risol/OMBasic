#include "runtime_internal.h"

Om_Status om_runtime_object_type(
    const Om_Execution_Context *context,
    const void *object,
    const Om_Runtime_Type_Descriptor **result)
{
    if (context == NULL || object == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Object_Header *header = find_object(context, object);
    if (header == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = header->fields.type;
    return OM_STATUS_OK;
}
