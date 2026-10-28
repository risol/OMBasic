#include "native_runtime_test_support.h"

int main(void)
{
    return om_runtime_test_fault_boundary() ? 0 : 1;
}
