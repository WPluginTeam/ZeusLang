#include "ZeusABI.hpp"

#include "ZeusABIInternal.hpp"
#include "../ZeusBuiltins/BuiltinNativeABI.hpp"
#include "../ZeusRunner/ZeusBuiltins/type.hpp"
#include "../ZeusRunner/ZeusBuiltins/strings.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

[[noreturn]] void nativeCallPanic(const char* name, const char* reason) {
    std::fprintf(stderr, "[ZeusValue native ABI] %s: %s\n",
                 name ? name : "<null builtin>", reason ? reason : "unknown error");
    std::abort();
}

void initializeStandaloneBuiltins() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        ZeusBuiltins::registerTypeBuiltins();
        ZeusBuiltins::registerStringBuiltins();
    });
}

} // namespace

void zeus_value_native_call(
    ZeusValue* out,
    const char* builtin_name,
    int32_t argc,
    const ZeusValue* const* values
) {
    if (!out) nativeCallPanic(builtin_name, "null result pointer");
    zeus_value_init_nil(out);

    if (!builtin_name || !*builtin_name) {
        nativeCallPanic(builtin_name, "empty builtin name");
    }
    if (argc < 0 || (argc > 0 && !values)) {
        nativeCallPanic(builtin_name, "invalid argument array");
    }
    if (!ZeusBuiltins::hasStandaloneValueCallABI(builtin_name)) {
        nativeCallPanic(builtin_name, "callback is not registered for standalone ZeusValue calls");
    }

    try {
        initializeStandaloneBuiltins();
        const int id = ZeusBuiltins::findId(builtin_name);
        const auto* descriptor = ZeusBuiltins::get(id);
        if (!descriptor) nativeCallPanic(builtin_name, "builtin is not registered");
        if (argc < descriptor->minArity ||
            (descriptor->maxArity >= 0 && argc > descriptor->maxArity)) {
            nativeCallPanic(builtin_name, "argument count does not match builtin signature");
        }

        std::vector<ZeusType::UniversalVariant> arguments;
        arguments.reserve(static_cast<size_t>(argc));
        for (int32_t i = 0; i < argc; ++i) {
            std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                zeus_value_to_box(values[i]), &zeus_box_release);
            arguments.push_back(box->v);
        }

        ZeusBox result;
        result.v = descriptor->fn(arguments);
        zeus_value_from_box(out, &result);
    } catch (const std::exception& error) {
        nativeCallPanic(builtin_name, error.what());
    } catch (...) {
        nativeCallPanic(builtin_name, "callback threw a non-standard exception");
    }
}
