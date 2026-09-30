#pragma once

#include "../ZeusBuiltins/NativeTypeABI.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ZeusLLVM::DynamicFieldBridge {

struct FieldBinding {
    std::string typeName;
    std::string fieldName;
    std::string fieldTypeName;
    size_t fieldIndex = 0;
    bool hasInitializationFlag = false;
    size_t initializationFlagIndex = 0;
};

struct TypeRules {
    std::function<std::string(const std::string&)> llvmFieldType;
    std::function<std::string(const std::string&)> llvmStructType;
    std::function<bool(const std::string&)> usesZeusValueABI;
    std::function<bool(const std::string&)> isClassType;
};

struct GeneratedIR {
    std::string constants;
    std::string adapters;
    std::string initializer;
    std::string error;

    bool empty() const { return adapters.empty(); }
};

inline std::string escapeBytes(const std::string& text) {
    std::string escaped;
    for (unsigned char c : text) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            escaped += static_cast<char>(c);
        } else {
            char encoded[5];
            std::snprintf(encoded, sizeof(encoded), "\\%02X", static_cast<unsigned>(c));
            escaped += encoded;
        }
    }
    return escaped;
}

inline GeneratedIR generate(
    const std::vector<FieldBinding>& fields,
    const TypeRules& rules
) {
    GeneratedIR generated;
    if (fields.empty()) return generated;

    auto addString = [&](size_t id, const char* suffix, const std::string& value) {
        const std::string symbol = "@.zeus_dynamic_field_" +
            std::to_string(id) + "_" + suffix;
        generated.constants += symbol + " = private unnamed_addr constant [" +
            std::to_string(value.size() + 1) + " x i8] c\"" + escapeBytes(value) +
            "\\00\", align 1\n";
        return symbol;
    };

    size_t adapterIndex = 0;
    for (const FieldBinding& field : fields) {
        const std::string storageType = rules.llvmFieldType(field.fieldTypeName);
        if (storageType.empty()) {
            generated.error = "dynamic field '" + field.typeName + "." +
                field.fieldName + "' has no LLVM storage type";
            return generated;
        }

        const std::string classString =
            addString(adapterIndex, "type", field.typeName);
        const std::string fieldString =
            addString(adapterIndex, "name", field.fieldName);
        const std::string thunkName = "@__zeus_dynamic_field_" +
            std::to_string(adapterIndex);

        generated.adapters += "define internal void " + thunkName +
            "(ptr %out, ptr %receiver, i32 %line) {\nentry:\n";
        generated.adapters += "  call void @zeus_value_init_nil(ptr %out)\n";
        generated.adapters += "  %object = call ptr @zeus_value_require_class_ptr(ptr %receiver, ptr " +
            classString + ")\n";
        const std::string structType = rules.llvmStructType(field.typeName);

        if (field.hasInitializationFlag) {
            generated.adapters += "  %initialized.address = getelementptr inbounds " +
                structType + ", ptr %object, i32 0, i32 " +
                std::to_string(field.initializationFlagIndex) + "\n";
            generated.adapters += "  %initialized = load i1, ptr %initialized.address\n";
            generated.adapters += "  %initialized.i32 = zext i1 %initialized to i32\n";
            generated.adapters += "  call void @zeus_class_field_require_initialized(i32 %initialized.i32, ptr " +
                classString + ", ptr " + fieldString + ", i32 %line)\n";
        }

        generated.adapters += "  %field.address = getelementptr inbounds " +
            structType + ", ptr %object, i32 0, i32 " +
            std::to_string(field.fieldIndex) + "\n";

        const std::string& fieldTypeName = field.fieldTypeName;
        if (rules.usesZeusValueABI(fieldTypeName)) {
            generated.adapters += "  call void @zeus_value_clone(ptr %out, ptr %field.address)\n";
        } else if (!fieldTypeName.empty() && fieldTypeName.front() == '&') {
            const std::string targetType = fieldTypeName.substr(1);
            generated.adapters += "  %native.address = load ptr, ptr %field.address\n";
            if (targetType == "int" || targetType == "int32" || targetType == "int64") {
                generated.adapters += "  call void @zeus_value_from_i64_ptr(ptr %out, ptr %native.address)\n";
            } else if (targetType == "bool") {
                generated.adapters += "  call void @zeus_value_from_bool_ptr(ptr %out, ptr %native.address)\n";
            } else if (rules.isClassType(targetType)) {
                const std::string targetClass =
                    addString(adapterIndex, "reference_type", targetType);
                generated.adapters += "  call void @zeus_value_from_class_ptr(ptr %out, ptr %native.address, ptr " +
                    targetClass + ")\n";
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                       ZeusBuiltins::isNativeViewABI(abi)) {
                generated.adapters += "  call void @" +
                    std::string(ZeusBuiltins::nativeViewToDynamicHelper(abi)) +
                    "(ptr %out, ptr %native.address)\n";
            } else {
                generated.error = "dynamic field '" + field.typeName + "." +
                    field.fieldName + "' cannot box reference type '&" + targetType + "'";
                return generated;
            }
        } else if (const auto* integerABI =
                       ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                   integerABI && integerABI->valueABI ==
                       ZeusBuiltins::NativeValueABI::Int64) {
            generated.adapters += "  %native.value = load i64, ptr %field.address\n";
            generated.adapters += "  call void @zeus_value_init_i64(ptr %out, i64 %native.value)\n";
        } else if (const auto* booleanABI =
                       ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                   booleanABI && booleanABI->valueABI ==
                       ZeusBuiltins::NativeValueABI::Bool1) {
            generated.adapters += "  %native.value = load i1, ptr %field.address\n";
            generated.adapters += "  %native.value.i32 = zext i1 %native.value to i32\n";
            generated.adapters += "  call void @zeus_value_init_bool(ptr %out, i32 %native.value.i32)\n";
        } else if (const auto* viewABI =
                       ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                   ZeusBuiltins::isNativeViewABI(viewABI)) {
            generated.adapters += "  %native.value = load ptr, ptr %field.address\n";
            generated.adapters += "  call void @" +
                std::string(ZeusBuiltins::nativeViewToDynamicHelper(viewABI)) +
                "(ptr %out, ptr %native.value)\n";
        } else if (rules.isClassType(fieldTypeName)) {
            const std::string targetClass =
                addString(adapterIndex, "field_type", fieldTypeName);
            generated.adapters += "  %native.value = load ptr, ptr %field.address\n";
            generated.adapters += "  call void @zeus_value_from_class_ptr(ptr %out, ptr %native.value, ptr " +
                targetClass + ")\n";
        } else {
            generated.error = "dynamic field '" + field.typeName + "." +
                field.fieldName + "' cannot box field type '" + fieldTypeName + "'";
            return generated;
        }

        generated.adapters += "  ret void\n}\n\n";
        generated.initializer += "  call void @zeus_dynamic_field_register(ptr " +
            classString + ", ptr " + fieldString + ", ptr " + thunkName + ")\n";
        ++adapterIndex;
    }

    generated.adapters += "define internal void @__zeus_register_dynamic_fields() {\nentry:\n";
    generated.adapters += generated.initializer;
    generated.adapters += "  ret void\n}\n\n";
    return generated;
}

} // namespace ZeusLLVM::DynamicFieldBridge
