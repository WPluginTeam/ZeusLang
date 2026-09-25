#include "ZeusDynamicMemberABI.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusType.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace {

using MethodMap = std::unordered_map<std::string, ZeusDynamicMemberFunction>;

std::shared_mutex& dynamicMemberMutex() {
    static std::shared_mutex mutex;
    return mutex;
}

std::unordered_map<std::string, MethodMap>& dynamicMemberFunctions() {
    static std::unordered_map<std::string, MethodMap> methods;
    return methods;
}

[[noreturn]] void dynamicMemberPanic(const char* method, const char* reason) {
    std::fprintf(stderr, "[Zeus dynamic member ABI] %s: %s\n",
                 method ? method : "<null method>",
                 reason ? reason : "unknown error");
    std::abort();
}

bool getLLVMClassTypeName(const ZeusValue* value, std::string& typeName) {
    if (!value) dynamicMemberPanic(nullptr, "null receiver");

    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(value), &zeus_box_release);
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v);
    if (!handle) return false;
    if (!handle->state) dynamicMemberPanic(nullptr, "invalid LLVM class handle");

    if (handle->state->object.load(std::memory_order_acquire) == nullptr) {
        dynamicMemberPanic(nullptr, "LLVM class object has already been deleted");
    }
    typeName = handle->state->typeName;
    return !typeName.empty();
}

} // namespace

extern "C" {

void zeus_dynamic_member_register(
    const char* type_name,
    const char* method_name,
    ZeusDynamicMemberFunction function
) {
    if (!type_name || !*type_name || !method_name || !*method_name || !function) {
        dynamicMemberPanic(method_name, "invalid method registration");
    }

    try {
        std::unique_lock<std::shared_mutex> lock(dynamicMemberMutex());
        dynamicMemberFunctions()[type_name][method_name] = function;
    } catch (...) {
        dynamicMemberPanic(method_name, "failed to register method");
    }
}

int32_t zeus_dynamic_member_has_method(
    const char* type_name,
    const char* method_name
) {
    if (!type_name || !*type_name || !method_name || !*method_name) return 0;

    try {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicMemberFunctions().find(type_name);
        if (type == dynamicMemberFunctions().end()) return 0;
        return type->second.find(method_name) != type->second.end() ? 1 : 0;
    } catch (...) {
        dynamicMemberPanic(method_name, "failed to inspect registered methods");
    }
}

void zeus_value_has_dynamic_member(
    ZeusValue* out,
    const ZeusValue* receiver,
    const ZeusValue* method_name
) {
    if (!out || !receiver || !method_name) {
        dynamicMemberPanic(nullptr, "invalid method-presence query");
    }

    zeus_value_init_nil(out);
    const ZeusStringView* name = zeus_value_intern_string_view(method_name);
    if (!name || name->length >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicMemberPanic(nullptr, "invalid dynamic method name");
    }
    const std::string methodName(
        name->data ? name->data : "", static_cast<size_t>(name->length));

    std::string typeName;
    try {
        if (!getLLVMClassTypeName(receiver, typeName)) {
            zeus_value_init_bool(out, 0);
            return;
        }
    } catch (...) {
        dynamicMemberPanic(methodName.c_str(), "failed to inspect receiver type");
    }

    zeus_value_init_bool(
        out, zeus_dynamic_member_has_method(typeName.c_str(), methodName.c_str()));
}

int32_t zeus_value_try_dynamic_member_call(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* method_name,
    int32_t argc,
    const ZeusValue* const* arguments
) {
    if (!out || !receiver || !method_name || !*method_name) {
        dynamicMemberPanic(method_name, "invalid dynamic method call");
    }
    if (argc < 0 || (argc > 0 && !arguments)) {
        dynamicMemberPanic(method_name, "invalid argument array");
    }

    zeus_value_init_nil(out);

    std::string typeName;
    try {
        if (!getLLVMClassTypeName(receiver, typeName)) return 0;
    } catch (...) {
        dynamicMemberPanic(method_name, "failed to inspect receiver type");
    }

    ZeusDynamicMemberFunction function = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicMemberFunctions().find(typeName);
        if (type == dynamicMemberFunctions().end()) return 0;
        const auto method = type->second.find(method_name);
        if (method == type->second.end()) return 0;
        function = method->second;
    }

    try {
        function(out, receiver, argc, arguments);
        return 1;
    } catch (const std::exception&) {
        dynamicMemberPanic(method_name, "generated method adapter threw an exception");
    } catch (...) {
        dynamicMemberPanic(method_name, "generated method adapter threw a non-standard exception");
    }
}

void zeus_value_dynamic_member_call(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* method_name,
    int32_t argc,
    const ZeusValue* const* arguments
) {
    if (!zeus_value_try_dynamic_member_call(
            out, receiver, method_name, argc, arguments)) {
        dynamicMemberPanic(method_name, "method is not available for this dynamic receiver");
    }
}

[[noreturn]] void zeus_dynamic_member_arity_error(
    const char* method_name,
    int32_t expected,
    int32_t actual
) {
    std::fprintf(stderr,
                 "[Zeus dynamic member ABI] %s: expected %d arguments, got %d\n",
                 method_name ? method_name : "<null method>", expected, actual);
    std::abort();
}

} // extern "C"
