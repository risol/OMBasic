#include "native_runtime_test_support.h"

int main(void)
{
    return om_runtime_test_json() ? 0 : 1;
}
