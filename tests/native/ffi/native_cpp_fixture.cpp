struct Cpp_Point {
    double x;
    double y;
};

extern "C" Cpp_Point om_ffi_make_cpp_point(void)
{
    return Cpp_Point{19.5, 22.5};
}

extern "C" int om_ffi_take_cpp_point(Cpp_Point value)
{
    return value.x == 19.5 && value.y == 22.5 ? 1 : 0;
}
