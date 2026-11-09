#include <cstdint>

extern "C" double zeus_native_decl_add_double(double left, double right) {
    return left + right;
}

extern "C" int64_t zeus_native_decl_add_i64(int64_t left, int64_t right) {
    return left + right;
}

extern "C" int32_t zeus_native_decl_add_i32(int32_t left, int32_t right) {
    return left + right;
}

extern "C" int8_t zeus_native_decl_add_i8(int8_t left, int8_t right) {
    return static_cast<int8_t>(left + right);
}

extern "C" double zeus_native_decl_mixed(double real, int64_t wide, int32_t medium, int8_t small) {
    return real + static_cast<double>(wide) + medium + small;
}
