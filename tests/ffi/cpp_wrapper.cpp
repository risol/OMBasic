#include <stdexcept>

static int cpp_transform(int value)
{
    if (value < 0) {
        throw std::invalid_argument("negative input");
    }
    return value * 2;
}

extern "C" int om_ffi_cpp_transform(int value) noexcept
{
    try {
        return cpp_transform(value);
    } catch (...) {
        return -1;
    }
}
