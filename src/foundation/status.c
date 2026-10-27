#include "ombasic/foundation/status.h"

const char *om_status_name(Om_Status status)
{
    switch (status) {
    case OM_STATUS_OK:
        return "ok";
    case OM_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case OM_STATUS_OUT_OF_MEMORY:
        return "out of memory";
    case OM_STATUS_OVERFLOW:
        return "overflow";
    case OM_STATUS_OUT_OF_RANGE:
        return "out of range";
    case OM_STATUS_INVALID_UTF8:
        return "invalid UTF-8";
    case OM_STATUS_NOT_FOUND:
        return "not found";
    case OM_STATUS_END_OF_INPUT:
        return "end of input";
    case OM_STATUS_CYCLE_DETECTED:
        return "cycle detected";
    case OM_STATUS_IO_ERROR:
        return "I/O error";
    case OM_STATUS_PATH_ESCAPE:
        return "path escapes source root or traverses a symbolic link";
    case OM_STATUS_NOT_A_DIRECTORY:
        return "not a directory";
    case OM_STATUS_CANCELLED:
        return "cancelled";
    }
    return "unknown status";
}
