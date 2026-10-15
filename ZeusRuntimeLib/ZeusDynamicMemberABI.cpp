#include "ZeusDynamicMemberABI.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusDynamicABI.hpp"
#include "ZeusObjectLifetime.hpp"
#include "ZeusRuntimeDebug.hpp"
#include "ZeusType.hpp"
#include "ZeusVMUtils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using MethodMap = std::unordered_map<std::string, ZeusDynamicMemberFunction>;
using FieldMap = std::unordered_map<std::string, ZeusDynamicFieldGetter>;
struct FieldWritePolicy {
    bool writable = true;
    std::string expectedType;
    ZeusDynamicFieldSetter setter = nullptr;
};
struct TraceFieldBinding {
    ZeusDynamicFieldGetter getter = nullptr;
    ZeusDynamicFieldPreparer preparer = nullptr;
};
using TraceFieldMap = std::unordered_map<std::string, TraceFieldBinding>;
using FieldWritePolicyMap = std::unordered_map<std::string, FieldWritePolicy>;
using DynamicFieldValues = std::unordered_map<std::string, ZeusValue>;

std::shared_mutex& dynamicMemberMutex() {
    static std::shared_mutex mutex;
    return mutex;
}

std::unordered_map<std::string, MethodMap>& dynamicMemberFunctions() {
    static std::unordered_map<std::string, MethodMap> methods;
    return methods;
}

std::unordered_map<std::string, FieldMap>& dynamicFieldGetters() {
    static std::unordered_map<std::string, FieldMap> fields;
    return fields;
}

std::unordered_map<std::string, TraceFieldMap>& dynamicFieldTraceGetters() {
    static std::unordered_map<std::string, TraceFieldMap> fields;
    return fields;
}

std::unordered_map<std::string, FieldWritePolicyMap>& dynamicFieldWritePolicies() {
    static std::unordered_map<std::string, FieldWritePolicyMap> fields;
    return fields;
}

std::mutex& dynamicFieldValuesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<void*, DynamicFieldValues>& dynamicFieldValues() {
    static std::unordered_map<void*, DynamicFieldValues> values;
    return values;
}

void releaseDynamicFieldValues(DynamicFieldValues& fields) {
    for (auto& field : fields) zeus_value_release(&field.second);
    fields.clear();
}

void cloneDynamicFieldValues(
    const DynamicFieldValues& source,
    DynamicFieldValues& destination
) {
    try {
        for (const auto& field : source) {
            ZeusValue cloned{};
            zeus_value_init_nil(&cloned);
            zeus_value_clone_internal(&cloned, &field.second);
            try {
                destination.emplace(field.first, cloned);
                zeus_value_init_nil(&cloned);
            } catch (...) {
                zeus_value_release(&cloned);
                throw;
            }
        }
    } catch (...) {
        releaseDynamicFieldValues(destination);
        throw;
    }
}

[[noreturn]] void dynamicMemberPanic(const char* method, const char* reason) {
    throw std::runtime_error(std::string("[Zeus dynamic member ABI] ") +
        (method ? method : "<null method>") + ": " +
        (reason ? reason : "unknown error"));
}

bool getDynamicTypeName(
    const ZeusValue* value,
    std::string& typeName,
    void** classObject = nullptr
) {
    if (!value) dynamicMemberPanic(nullptr, "null receiver");
    if (classObject) *classObject = nullptr;

    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(value), &zeus_box_release);
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v);
    if (handle) {
        if (!handle->state) dynamicMemberPanic(nullptr, "invalid LLVM class handle");

        void* object = handle->state->object.load(std::memory_order_acquire);
        if (object == nullptr) {
            dynamicMemberPanic(nullptr, "LLVM class object has already been deleted");
        }
        zeus_class_ref_require_live(object);
        if (classObject) *classObject = object;
        typeName = handle->state->typeName;
        return !typeName.empty();
    }

    if (std::holds_alternative<ZeusType::Bytes>(box->v)) {
        typeName = "bytes";
        return true;
    }
    if (const auto* variant = std::get_if<ZeusType::Variant>(&box->v)) {
        if (std::holds_alternative<ZeusType::Array>(*variant)) {
            typeName = "Array";
            return true;
        }
        if (std::holds_alternative<ZeusType::Bytes>(*variant)) {
            typeName = "bytes";
            return true;
        }
    }

    LexerType::Value nativeValue(0);
    if (!ZeusVMUtils::convertToValue(box->v, nativeValue)) return false;
    typeName = nativeValue.getStringType();
    return !typeName.empty();
}

bool loadDynamicFieldValue(
    void* object,
    const char* fieldName,
    ZeusValue* out
) {
    if (!object || !fieldName || !out) return false;
    std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
    const auto objectIt = dynamicFieldValues().find(object);
    if (objectIt == dynamicFieldValues().end()) return false;
    const auto fieldIt = objectIt->second.find(fieldName);
    if (fieldIt == objectIt->second.end()) return false;
    zeus_value_clone(out, &fieldIt->second);
    return true;
}

void storeDynamicFieldValue(
    void* object,
    const char* fieldName,
    const ZeusValue* value
) {
    ZeusValue replacement{};
    ZeusValue previous{};
    zeus_value_init_nil(&replacement);
    zeus_value_init_nil(&previous);
    struct ValueGuard {
        ZeusValue* value;
        ~ValueGuard() { zeus_value_release(value); }
    } replacementGuard{&replacement}, previousGuard{&previous};
    // Dynamic instance fields are outgoing graph edges, not independent roots.
    zeus_value_clone_internal(&replacement, value);

    {
        std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
        auto owner = dynamicFieldValues().find(object);
        if (owner == dynamicFieldValues().end()) {
            owner = dynamicFieldValues().emplace(object, DynamicFieldValues{}).first;
            ZeusRuntimeDebug::trackCreated(
                ZeusRuntimeDebug::ResourceKind::DynamicFieldOwner,
                object,
                "dynamic-field-sidecar",
                __FILE__,
                __LINE__
            );
        }
        auto& fields = owner->second;
        const auto existing = fields.find(fieldName);
        if (existing == fields.end()) {
            fields.emplace(fieldName, replacement);
        } else {
            previous = existing->second;
            existing->second = replacement;
        }
        zeus_value_init_nil(&replacement);
    }
}

void clearDynamicFieldValue(void* object, const char* fieldName) {
    if (!object || !fieldName) return;
    ZeusValue removed{};
    zeus_value_init_nil(&removed);
    bool foundValue = false;
    {
        std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
        const auto objectIt = dynamicFieldValues().find(object);
        if (objectIt == dynamicFieldValues().end()) return;
        const auto fieldIt = objectIt->second.find(fieldName);
        if (fieldIt == objectIt->second.end()) return;
        std::swap(removed, fieldIt->second);
        objectIt->second.erase(fieldIt);
        if (objectIt->second.empty()) dynamicFieldValues().erase(objectIt);
        foundValue = true;
    }
    if (foundValue) zeus_value_release(&removed);
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

void zeus_dynamic_field_register(
    const char* type_name,
    const char* field_name,
    ZeusDynamicFieldGetter getter
) {
    if (!type_name || !*type_name || !field_name || !*field_name || !getter) {
        dynamicMemberPanic(field_name, "invalid dynamic field registration");
    }

    try {
        std::unique_lock<std::shared_mutex> lock(dynamicMemberMutex());
        dynamicFieldGetters()[type_name][field_name] = getter;
    } catch (...) {
        dynamicMemberPanic(field_name, "failed to register dynamic field");
    }
}

void zeus_dynamic_field_trace_register(
    const char* type_name,
    const char* field_name,
    ZeusDynamicFieldGetter getter,
    ZeusDynamicFieldPreparer preparer
) {
    if (!type_name || !*type_name || !field_name || !*field_name || !getter) {
        dynamicMemberPanic(field_name, "invalid GC field trace registration");
    }

    try {
        std::unique_lock<std::shared_mutex> lock(dynamicMemberMutex());
        dynamicFieldTraceGetters()[type_name][field_name] =
            TraceFieldBinding{ getter, preparer };
    } catch (...) {
        dynamicMemberPanic(field_name, "failed to register GC field trace getter");
    }
}

void zeus_dynamic_field_setter_register(
    const char* type_name,
    const char* field_name,
    int32_t writable,
    const char* expected_type,
    ZeusDynamicFieldSetter setter
) {
    if (!type_name || !*type_name || !field_name || !*field_name) {
        dynamicMemberPanic(field_name, "invalid dynamic field setter registration");
    }

    try {
        std::unique_lock<std::shared_mutex> lock(dynamicMemberMutex());
        FieldWritePolicy policy;
        policy.writable = writable != 0;
        if (expected_type) policy.expectedType = expected_type;
        policy.setter = setter;
        dynamicFieldWritePolicies()[type_name][field_name] = std::move(policy);
    } catch (...) {
        dynamicMemberPanic(field_name, "failed to register dynamic field setter");
    }
}

void zeus_value_dynamic_field_load(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* field_name,
    int32_t line
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out || !receiver || !field_name || !*field_name) {
        dynamicMemberPanic(field_name, "invalid dynamic field load");
    }

    zeus_value_init_nil(out);

    std::string typeName;
    void* object = nullptr;
    if (!getDynamicTypeName(receiver, typeName, &object)) {
        dynamicMemberPanic(field_name, "receiver has no dynamic type");
    }

    if (loadDynamicFieldValue(object, field_name, out)) return;

    ZeusDynamicFieldGetter getter = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicFieldGetters().find(typeName);
        if (type != dynamicFieldGetters().end()) {
            const auto field = type->second.find(field_name);
            if (field != type->second.end()) getter = field->second;
        }
    }

    if (!getter) {
        dynamicMemberPanic(field_name,
            ("field is not available on dynamic receiver type '" + typeName + "'").c_str());
    }

    getter(out, receiver, line);
}

void zeus_value_dynamic_attribute_load(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* member_name,
    int32_t line,
    ZeusClosureInvoke method_invoke
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out || !receiver || !member_name || !*member_name) {
        dynamicMemberPanic(member_name, "invalid dynamic attribute load");
    }

    zeus_value_init_nil(out);

    std::string typeName;
    void* object = nullptr;
    if (!getDynamicTypeName(receiver, typeName, &object)) {
        dynamicMemberPanic(member_name, "receiver has no dynamic type");
    }

    // Preserve OBJ_LOAD_ATTR semantics: data stored in a field takes priority
    // over an instance method with the same name.
    if (loadDynamicFieldValue(object, member_name, out)) return;

    ZeusDynamicFieldGetter getter = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicFieldGetters().find(typeName);
        if (type != dynamicFieldGetters().end()) {
            const auto field = type->second.find(member_name);
            if (field != type->second.end()) getter = field->second;
        }
    }
    if (getter) {
        getter(out, receiver, line);
        return;
    }

    if (!zeus_dynamic_member_has_method(typeName.c_str(), member_name)) {
        dynamicMemberPanic(member_name,
            ("field is not available on dynamic receiver type '" + typeName + "'").c_str());
    }
    if (!method_invoke) {
        dynamicMemberPanic(member_name, "dynamic method has no bound-call adapter");
    }

    void* captureCell = zeus_capture_cell_create(receiver);
    try {
        void* captures[] = { captureCell };
        zeus_closure_create_range(
            out,
            method_invoke,
            0,
            std::numeric_limits<int32_t>::max(),
            1,
            captures);
    } catch (...) {
        zeus_capture_cell_release(captureCell);
        throw;
    }
    zeus_capture_cell_release(captureCell);
}

void zeus_value_dynamic_field_store(
    const ZeusValue* receiver,
    const char* field_name,
    const ZeusValue* value,
    int32_t line
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!receiver || !field_name || !*field_name || !value) {
        dynamicMemberPanic(field_name, "invalid dynamic field store");
    }

    // Match VM OBJ_STORE_ATTR: an overloaded attribute assignment takes
    // priority over a normal field write and receives (this, name, value).
    ZeusValue nameValue{};
    ZeusValue ignoredResult{};
    zeus_value_init_nil(&nameValue);
    zeus_value_init_nil(&ignoredResult);
    struct ValueGuard {
        ZeusValue* value;
        ~ValueGuard() { zeus_value_release(value); }
    } nameGuard{&nameValue}, resultGuard{&ignoredResult};
    const size_t fieldNameLength = std::strlen(field_name);
    if (static_cast<uint64_t>(fieldNameLength) >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicMemberPanic(field_name, "dynamic field name is too long");
    }
    zeus_value_from_string_data(
        &nameValue, field_name, static_cast<int64_t>(fieldNameLength));
    const ZeusValue* operatorArguments[] = { &nameValue, value };
    if (zeus_value_try_dynamic_member_call(
            &ignoredResult, receiver, "operator[.=]", 2, operatorArguments)) {
        return;
    }

    std::string typeName;
    void* object = nullptr;
    if (!getDynamicTypeName(receiver, typeName, &object)) {
        dynamicMemberPanic(field_name, "receiver has no dynamic class type");
    }
    if (!object) {
        dynamicMemberPanic(field_name, "receiver is not an LLVM class object");
    }

    bool registered = false;
    FieldWritePolicy policy;
    {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicFieldWritePolicies().find(typeName);
        if (type != dynamicFieldWritePolicies().end()) {
            const auto field = type->second.find(field_name);
            if (field != type->second.end()) {
                registered = true;
                policy = field->second;
            }
        }
    }

    if (registered && !policy.writable) {
        dynamicMemberPanic(field_name, "cannot assign to const field");
    }
    if (registered && !policy.expectedType.empty()) {
        const std::string memberName = typeName + "." + field_name;
        zeus_value_validate_argument_type(
            value, memberName.c_str(), 0, policy.expectedType.c_str());
    }
    if (registered && policy.setter) {
        policy.setter(object, value, line);
        clearDynamicFieldValue(object, field_name);
        return;
    }
    storeDynamicFieldValue(object, field_name, value);
}

void zeus_value_dynamic_field_object_destroy(void* object) {
    if (!object) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    DynamicFieldValues fields;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
        const auto found = dynamicFieldValues().find(object);
        if (found == dynamicFieldValues().end()) return;
        fields.swap(found->second);
        dynamicFieldValues().erase(found);
        removed = true;
    }
    if (removed) {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::DynamicFieldOwner, object);
    }
    for (auto& field : fields) zeus_value_release(&field.second);
}

void zeus_value_dynamic_field_replace_state(void* destination, void* source) {
    if (!destination || !source) {
        dynamicMemberPanic(nullptr, "invalid dynamic field state replacement");
    }
    if (destination == source) return;

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    DynamicFieldValues sourceSnapshot;
    DynamicFieldValues replacementFields;
    DynamicFieldValues previousFields;
    bool sourceHasOwner = false;
    bool destinationCreated = false;
    bool destinationRemoved = false;

    try {
        {
            std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
            const auto sourceIt = dynamicFieldValues().find(source);
            if (sourceIt != dynamicFieldValues().end()) {
                sourceHasOwner = true;
                sourceSnapshot = sourceIt->second;
            }
        }

        cloneDynamicFieldValues(sourceSnapshot, replacementFields);

        {
            std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
            auto destinationIt = dynamicFieldValues().find(destination);
            if (sourceHasOwner) {
                if (destinationIt == dynamicFieldValues().end()) {
                    destinationIt = dynamicFieldValues().emplace(
                        destination, DynamicFieldValues{}).first;
                    destinationCreated = true;
                }
                destinationIt->second.swap(replacementFields);
            } else if (destinationIt != dynamicFieldValues().end()) {
                previousFields.swap(destinationIt->second);
                dynamicFieldValues().erase(destinationIt);
                destinationRemoved = true;
            }
        }
    } catch (...) {
        releaseDynamicFieldValues(replacementFields);
        releaseDynamicFieldValues(previousFields);
        throw;
    }

    if (destinationCreated) {
        ZeusRuntimeDebug::trackCreated(
            ZeusRuntimeDebug::ResourceKind::DynamicFieldOwner,
            destination,
            "dynamic-field-sidecar",
            __FILE__,
            __LINE__
        );
    } else if (destinationRemoved) {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::DynamicFieldOwner, destination);
    }

    releaseDynamicFieldValues(replacementFields);
    releaseDynamicFieldValues(previousFields);
}

void zeus_value_dynamic_field_trace(
    void* object,
    const char* type_name,
    ZeusDynamicValueTraceCallback callback,
    void* context
) {
    if (!object || !callback) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());

    {
        std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
        const auto owner = dynamicFieldValues().find(object);
        if (owner != dynamicFieldValues().end()) {
            for (const auto& field : owner->second) callback(context, &field.second);
        }
    }

    // AOT class fields with native layouts live inside the class allocation,
    // not in the dynamic sidecar above. Visit their registered getters too so
    // an object -> container -> object cycle is represented in the same graph.
    if (!type_name || !*type_name) return;
    std::vector<ZeusDynamicFieldGetter> getters;
    {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicFieldTraceGetters().find(type_name);
        if (type == dynamicFieldTraceGetters().end()) return;
        getters.reserve(type->second.size());
        for (const auto& field : type->second) {
            getters.push_back(field.second.getter);
        }
    }

    ZeusValue receiver{};
    zeus_value_init_nil(&receiver);
    zeus_value_from_class_ptr(&receiver, object, type_name);
    struct ValueGuard {
        ZeusValue* value;
        ~ValueGuard() { zeus_value_release(value); }
    } receiverGuard{&receiver};

    for (const auto getter : getters) {
        if (!getter) continue;
        ZeusValue value{};
        zeus_value_init_nil(&value);
        ValueGuard valueGuard{&value};
        try {
            getter(&value, &receiver, 0);
            callback(context, &value);
        } catch (...) {
            // Uninitialized native fields are not outgoing edges yet.
        }
    }
}

void zeus_value_dynamic_field_prepare(void* object, const char* type_name) {
    if (!object || !type_name || !*type_name) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::vector<ZeusDynamicFieldPreparer> preparers;
    {
        std::shared_lock<std::shared_mutex> lock(dynamicMemberMutex());
        const auto type = dynamicFieldTraceGetters().find(type_name);
        if (type == dynamicFieldTraceGetters().end()) return;
        preparers.reserve(type->second.size());
        for (const auto& field : type->second) {
            if (field.second.preparer) preparers.push_back(field.second.preparer);
        }
    }
    for (const auto prepare : preparers) prepare(object);
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
    const ZeusNativeString name = zeus_value_intern_string(method_name);
    uint64_t nameLength = 0;
    if (name) std::memcpy(&nameLength, name - sizeof(uint64_t), sizeof(nameLength));
    if (!name || nameLength >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        nameLength > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicMemberPanic(nullptr, "invalid dynamic method name");
    }
    const std::string methodName(name, static_cast<size_t>(nameLength));

    std::string typeName;
    try {
        if (!getDynamicTypeName(receiver, typeName)) {
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
        if (!getDynamicTypeName(receiver, typeName)) return 0;
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

    function(out, receiver, argc, arguments);
    return 1;
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

void zeus_value_subscript_load(
    ZeusValue* out,
    const ZeusValue* container,
    const ZeusValue* key
) {
    if (!out || !container || !key) {
        dynamicMemberPanic("operator[[]]", "invalid subscript load arguments");
    }

    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(container), &zeus_box_release);
    if (!box) dynamicMemberPanic("operator[[]]", "failed to inspect subscript receiver");

    if (std::holds_alternative<ZeusType::LLVMClassHandle>(box->v)) {
        const ZeusValue* arguments[] = { key };
        zeus_value_dynamic_member_call(out, container, "operator[[]]", 1, arguments);
        return;
    }

    zeus_value_dict_load(out, container, key);
}

void zeus_value_subscript_store(
    ZeusValue* out,
    const ZeusValue* container,
    const ZeusValue* key,
    const ZeusValue* value
) {
    if (!out || !container || !key || !value) {
        dynamicMemberPanic("operator[[]=]", "invalid subscript store arguments");
    }

    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(container), &zeus_box_release);
    if (!box) dynamicMemberPanic("operator[[]=]", "failed to inspect subscript receiver");

    if (std::holds_alternative<ZeusType::LLVMClassHandle>(box->v)) {
        const ZeusValue* arguments[] = { key, value };
        ZeusValue ignored{};
        zeus_value_init_nil(&ignored);
        zeus_value_dynamic_member_call(
            &ignored, container, "operator[[]=]", 2, arguments);
        zeus_value_release(&ignored);
        zeus_value_init_nil(out);
        zeus_value_clone(out, value);
        return;
    }

    zeus_value_dict_store(out, container, key, value);
}

[[noreturn]] void zeus_dynamic_member_arity_error(
    const char* method_name,
    int32_t expected,
    int32_t actual
) {
    throw std::runtime_error(std::string("[Zeus dynamic member ABI] ") +
        (method_name ? method_name : "<null method>") + ": expected " +
        std::to_string(expected) + " arguments, got " + std::to_string(actual));
}

[[noreturn]] void zeus_dynamic_member_arity_range_error(
    const char* method_name,
    int32_t minimum,
    int32_t maximum,
    int32_t actual
) {
    throw std::runtime_error(std::string("[Zeus dynamic member ABI] ") +
        (method_name ? method_name : "<null method>") + ": expected between " +
        std::to_string(minimum) + " and " + std::to_string(maximum) +
        " arguments, got " + std::to_string(actual));
}


void zeus_value_copy_class_graph(ZeusValue* out, const ZeusValue* source, void* context) noexcept(false) {
    if (!out || !source) throw std::runtime_error("invalid class value copy");
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(source), &zeus_box_release);
    if (const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v)) {
        if (!handle->state) throw std::runtime_error("invalid class copy handle");
        void* object = handle->state->object.load(std::memory_order_acquire);
        if (!object) throw std::runtime_error("copy of deleted class object");
        void* copied = zeus_class_copy_object(object, context);
        try {
            zeus_value_from_class_ptr(out, copied, handle->state->typeName.c_str());
        } catch (...) {
            zeus_class_ref_release(copied);
            throw;
        }
        zeus_class_ref_release(copied);
    } else {
        zeus_value_clone(out, source);
    }
}

void zeus_value_copy_class_graph_default(ZeusValue* out, const ZeusValue* source, void* context) noexcept(false) {
    if (!out || !source) throw std::runtime_error("invalid class value copy");
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(source), &zeus_box_release);
    if (const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v)) {
        if (!handle->state) throw std::runtime_error("invalid class copy handle");
        void* object = handle->state->object.load(std::memory_order_acquire);
        if (!object) throw std::runtime_error("copy of deleted class object");
        void* copied = zeus_class_copy_default_object(object, context);
        try {
            zeus_value_from_class_ptr(out, copied, handle->state->typeName.c_str());
        } catch (...) {
            zeus_class_ref_release(copied);
            throw;
        }
        zeus_class_ref_release(copied);
    } else {
        zeus_value_clone(out, source);
    }
}

void zeus_value_copy_class_graph_internal(ZeusValue* out, const ZeusValue* source, void* context) noexcept(false) {
    ZeusValue owned{};
    zeus_value_init_nil(&owned);
    try {
        zeus_value_copy_class_graph(&owned, source, context);
        zeus_value_clone_internal(out, &owned);
    } catch (...) {
        zeus_value_release(&owned);
        throw;
    }
    zeus_value_release(&owned);
}


void zeus_value_dynamic_field_copy_state(void* destination, void* source, void* context) noexcept(false) {
    if (!destination || !source) throw std::runtime_error("invalid class field graph copy");
    if (destination == source) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    DynamicFieldValues snapshot;
    {
        std::lock_guard<std::mutex> lock(dynamicFieldValuesMutex());
        const auto found = dynamicFieldValues().find(source);
        if (found == dynamicFieldValues().end()) return;
        cloneDynamicFieldValues(found->second, snapshot);
    }
    try {
        for (const auto& field : snapshot) {
            ZeusValue copied{};
            zeus_value_init_nil(&copied);
            try {
                zeus_value_copy_class_graph(&copied, &field.second, context);
                storeDynamicFieldValue(destination, field.first.c_str(), &copied);
            } catch (...) {
                zeus_value_release(&copied);
                throw;
            }
            zeus_value_release(&copied);
        }
    } catch (...) {
        releaseDynamicFieldValues(snapshot);
        throw;
    }
    releaseDynamicFieldValues(snapshot);
}


void zeus_value_copy_class_owned(ZeusValue* out, const ZeusValue* source, int32_t skip_hook) noexcept(false) {
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
        zeus_value_to_box(source), &zeus_box_release);
    if (const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v)) {
        if (!handle->state) throw std::runtime_error("invalid owned class copy handle");
        void* object = handle->state->object.load(std::memory_order_acquire);
        if (!object) throw std::runtime_error("copy of deleted class object");
        void* result = zeus_class_copy_owned(object, skip_hook);
        try { zeus_value_from_class_ptr(out, result, handle->state->typeName.c_str()); }
        catch (...) { zeus_class_ref_release(result); throw; }
        zeus_class_ref_release(result);
    } else zeus_value_clone(out, source);
}

} // extern "C"
