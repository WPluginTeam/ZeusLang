#include "ZeusABI.hpp"

#include "ZeusABIInternal.hpp"
#include "../ZeusBuiltins/BuiltinNativeABI.hpp"
#include "../ZeusRunner/ZeusBuiltins/array.hpp"
#include "../ZeusRunner/ZeusBuiltins/dict.hpp"
#include "../ZeusRunner/ZeusBuiltins/gc.hpp"
#include "../ZeusRunner/ZeusBuiltins/ThreadNativeBuiltins.hpp"
#include "../ZeusRunner/ZeusBuiltins/type.hpp"
#include "../ZeusRunner/ZeusBuiltins/strings.hpp"
#include "../ZeusRunner/ZeusBuiltins/time.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[noreturn]] void nativeCallPanic(const char* name, const char* reason) {
    throw std::runtime_error(std::string("[ZeusValue native ABI] ") +
        (name ? name : "<null builtin>") + ": " +
        (reason ? reason : "unknown error"));
}

// Returns the text of a boxed native String instance, or nullptr when the value
// is not a native string. The caller converts it before invoking a builtin.
const ZeusStringView* nativeStringTextOf(const ZeusValue* value,
                                         const ZeusType::UniversalVariant& boxed) {
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) return nullptr;
    const auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&boxed);
    if (!classHandle || !classHandle->state ||
        classHandle->state->typeName != "String") {
        return nullptr;
    }
    return zeus_value_intern_string_view(value);
}

void initializeStandaloneBuiltins() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        ZeusBuiltins::registerTypeBuiltins();
        ZeusBuiltins::registerStringBuiltins();
        ZeusBuiltins::registerArrayBuiltins();
        ZeusBuiltins::registerDictBuiltins();
        ZeusBuiltins::registerGcBuiltins();
        // Lock/RLock in imported Zeus modules use these VM-independent native
        // operations through the standalone ZeusValue call ABI.
        ZeusBuiltins::registerStandaloneThreadBuiltins();
        // The time builtins need no VM state, so imported packages that use them
        // work in AOT binaries too.
        ZeusBuiltins::registerTimeBuiltins();
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
            // Builtins work on universal values. A native String instance only
            // carries its text behind the registered toString method, so it is
            // normalized here instead of in every builtin.
            const ZeusStringView* text = nativeStringTextOf(values[i], box->v);
            arguments.push_back(text
                ? ZeusType::UniversalVariant(std::string(text->data, static_cast<size_t>(text->length)))
                : box->v);
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
