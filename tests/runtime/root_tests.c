#include "native_runtime_test_support.h"

int main(void)
{
    return om_runtime_test_root_order() ? 0 : 1;
}
