#include <cstdint>

namespace {

int64_t g_registered_handler = 0;
int64_t g_registered_user_ctx = 0;

} // namespace

extern "C" int32_t zeus_test_invoke_i32_callback(
    int64_t callbackAddr,
    int32_t left,
    int32_t right
) {
    auto fn = reinterpret_cast<int32_t (*)(int32_t, int32_t)>(
        static_cast<uintptr_t>(callbackAddr));
    return fn ? fn(left, right) : -1;
}

extern "C" void zeus_test_invoke_void_callback(
    int64_t callbackAddr,
    int32_t delta
) {
    auto fn = reinterpret_cast<void (*)(int32_t)>(
        static_cast<uintptr_t>(callbackAddr));
    if (fn) {
        fn(delta);
    }
}

extern "C" void zeus_test_register_ctx_handler(
    int64_t callbackAddr,
    int64_t userCtx
) {
    g_registered_handler = callbackAddr;
    g_registered_user_ctx = userCtx;
}

extern "C" int32_t zeus_test_dispatch_ctx_handler(int32_t input) {
    auto fn = reinterpret_cast<int32_t (*)(int64_t, int32_t)>(
        static_cast<uintptr_t>(g_registered_handler));
    return fn ? fn(g_registered_user_ctx, input) : -1;
}

extern "C" int32_t zeus_test_c_target(int32_t value) {
    return value * 3 + 1;
}

extern "C" int32_t zeus_test_verify_c_target_addr(int64_t addr, int32_t arg) {
    const auto expected = static_cast<int64_t>(
        reinterpret_cast<uintptr_t>(&zeus_test_c_target));
    if (addr != expected) {
        return -999;
    }
    auto fn = reinterpret_cast<int32_t (*)(int32_t)>(
        static_cast<uintptr_t>(addr));
    return fn(arg);
}

extern "C" int32_t zeus_test_only_addr_target(int32_t value) {
    return value + 100;
}

extern "C" int32_t zeus_test_call_unary(int64_t fnAddr, int32_t arg) {
    auto fn = reinterpret_cast<int32_t (*)(int32_t)>(
        static_cast<uintptr_t>(fnAddr));
    return fn ? fn(arg) : -1;
}
