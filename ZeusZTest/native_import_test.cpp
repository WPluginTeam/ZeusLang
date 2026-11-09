#include <cstdint>
#include <string>

#ifdef _WIN32
#define ZEUS_NATIVE_TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define ZEUS_NATIVE_TEST_EXPORT extern "C"
#endif

extern "C" int64_t zeus_test_add(int64_t left, int64_t right) {
    return left + right;
}

extern "C" bool zeus_test_bool(bool value) {
    return !value;
}

extern "C" const char* zeus_test_greeting(const char* name) {
    static thread_local std::string greeting;
    greeting = "Hello, ";
    greeting += name ? name : "";
    return greeting.c_str();
}

extern "C" void zeus_test_noop() {}

namespace {
int32_t nativeInt8CallCount = 0;
}

ZEUS_NATIVE_TEST_EXPORT double zeus_test_add_double(double left, double right) {
    return left + right;
}

ZEUS_NATIVE_TEST_EXPORT double zeus_test_mixed_double(
    double first,
    int64_t second,
    double third,
    int64_t fourth,
    double fifth,
    int64_t sixth
) {
    return first + second * 10.0 + third * 100.0 + fourth * 1000.0 +
        fifth * 10000.0 + sixth * 100000.0;
}

ZEUS_NATIVE_TEST_EXPORT int8_t zeus_test_native_int8(int8_t value) {
    ++nativeInt8CallCount;
    return value;
}

ZEUS_NATIVE_TEST_EXPORT int32_t zeus_test_native_int8_call_count() {
    return nativeInt8CallCount;
}

ZEUS_NATIVE_TEST_EXPORT float zeus_test_native_float(float value) {
    return value * 1.5f;
}

ZEUS_NATIVE_TEST_EXPORT float zeus_test_identity_float(float value) {
    return value;
}

ZEUS_NATIVE_TEST_EXPORT float zeus_test_mixed_float(
    float first,
    int16_t second,
    uint64_t third,
    float fourth,
    uint8_t fifth
) {
    return first + static_cast<float>(second) * 10.0f +
        static_cast<float>(third) * 100.0f + fourth * 1000.0f +
        static_cast<float>(fifth) * 10000.0f;
}

ZEUS_NATIVE_TEST_EXPORT int16_t zeus_test_native_int16(int16_t value) {
    return value;
}

ZEUS_NATIVE_TEST_EXPORT uint8_t zeus_test_native_uint8(uint8_t value) {
    return value;
}

ZEUS_NATIVE_TEST_EXPORT uint16_t zeus_test_native_uint16(uint16_t value) {
    return value;
}

ZEUS_NATIVE_TEST_EXPORT uint32_t zeus_test_native_uint32(uint32_t value) {
    return value;
}

ZEUS_NATIVE_TEST_EXPORT uint64_t zeus_test_native_uint64(uint64_t value) {
    return value;
}
