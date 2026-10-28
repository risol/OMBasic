#include "checked_size.h"

#include <stdint.h>

int main(void)
{
    size_t result = 0;
    if (om_size_add_overflows(&result, 17, 25) || result != 42) {
        return 1;
    }
    if (!om_size_add_overflows(&result, SIZE_MAX, 1)) {
        return 2;
    }
    if (om_size_mul_overflows(&result, 6, 7) || result != 42) {
        return 3;
    }
    if (!om_size_mul_overflows(&result, SIZE_MAX, 2)) {
        return 4;
    }
    return 0;
}
