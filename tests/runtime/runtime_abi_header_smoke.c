#include "ombasic/runtime/abi.h"
#include "ombasic/runtime/embedding.h"
#include "ombasic/runtime/distribution.h"

int main(void)
{
    return OM_RUNTIME_ABI_VERSION == 1 ? 0 : 1;
}
