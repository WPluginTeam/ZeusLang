#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace IRType {

    enum class SymbolRefKind {
        Unknown,
        Local,
        Global,
        Upvalue,
        Shared,
        Function,
        ListElement,
        DictKey,
        DictValue,
        CountIndex,
        Class
    };

    // IR 层的符号引用：把“名字 + 槽位 + 引用类别”从 args 字符串协议中提取出来。
    // args 仍然保留给 VM，SymbolRef 供 LLVM 和其它后端使用。
    struct SymbolRef {
        SymbolRefKind kind = SymbolRefKind::Unknown;
        std::string name;
        int index = -1;
        int aux = -1;
        bool isConst = false;

        static SymbolRef function(std::string functionName) {
            SymbolRef result;
            result.kind = SymbolRefKind::Function;
            result.name = std::move(functionName);
            return result;
        }

        static SymbolRef classType(std::string className) {
            SymbolRef result;
            result.kind = SymbolRefKind::Class;
            result.name = std::move(className);
            return result;
        }

        bool isResolved() const {
            return kind != SymbolRefKind::Unknown;
        }
    };

    enum class IRMetadataKind {
        None,
        FunctionSignature,
        ExternalFunctionSignature,
        DirectCall,
        CallTarget,
        Symbol,
        ClassLayout,
        ModuleImportInit
    };

    enum class ClassFieldDefaultKind {
        Zero,
        Integer,
        Boolean,
        String,
        Null,
        EmptyArray,
        EmptyDict,
        Unsupported
    };

    struct ClassFieldDefault {
        ClassFieldDefaultKind kind = ClassFieldDefaultKind::Zero;
        int64_t integerValue = 0;
        bool booleanValue = false;
        std::string stringValue;
    };

    // 指令旁路元信息。它不参与 VM 执行，只为 LLVM lowering、调试和后续后端提供结构化信息。
    struct IRMetadata {
        IRMetadataKind kind = IRMetadataKind::None;
        std::string tag;
        SymbolRef symbol;
        std::string typeName;
        std::vector<std::string> typeNames;
        std::vector<std::string> fieldNames;
        std::vector<bool> fieldConstness;
        std::vector<ClassFieldDefault> fieldDefaults;
        std::string ownerType;
        std::string operatorSymbol;
        std::string nativeSymbol;
        std::string moduleName;
        int captureCount = 0;
        bool hasCaptureCount = false;

        static IRMetadata functionSignature(
            const std::string& name,
            const std::string& returnType,
            int captures,
            std::vector<std::string> parameterTypes,
            std::string owner = {},
            std::string op = {}
        ) {
            IRMetadata result;
            result.kind = IRMetadataKind::FunctionSignature;
            result.tag = "function_signature";
            result.symbol = SymbolRef::function(name);
            result.typeName = returnType;
            result.typeNames = std::move(parameterTypes);
            result.ownerType = std::move(owner);
            result.operatorSymbol = std::move(op);
            result.captureCount = captures;
            result.hasCaptureCount = true;
            return result;
        }

        static IRMetadata externalFunctionSignature(
            const std::string& name,
            const std::string& returnType,
            std::vector<std::string> parameterTypes,
            std::string nativeSymbol = {}
        ) {
            IRMetadata result = functionSignature(
                name, returnType, 0, std::move(parameterTypes)
            );
            result.kind = IRMetadataKind::ExternalFunctionSignature;
            result.tag = "external_function_signature";
            result.nativeSymbol = std::move(nativeSymbol);
            return result;
        }

        static IRMetadata directCall(const std::string& name) {
            IRMetadata result;
            result.kind = IRMetadataKind::DirectCall;
            result.tag = "direct_call";
            result.symbol = SymbolRef::function(name);
            return result;
        }

        static IRMetadata callTarget(
            const std::string& name,
            const std::string& returnType,
            int captures,
            std::vector<std::string> parameterTypes
        ) {
            IRMetadata result;
            result.kind = IRMetadataKind::CallTarget;
            result.tag = "direct_call";
            result.symbol = SymbolRef::function(name);
            result.typeName = returnType;
            result.typeNames = std::move(parameterTypes);
            result.captureCount = captures;
            result.hasCaptureCount = true;
            return result;
        }

        static IRMetadata classLayout(
            const std::string& className,
            std::vector<std::string> names,
            std::vector<std::string> fieldTypes,
            std::vector<bool> constFields = {},
            std::vector<ClassFieldDefault> defaults = {}
        ) {
            IRMetadata result;
            result.kind = IRMetadataKind::ClassLayout;
            result.tag = "class_layout";
            result.symbol = SymbolRef::classType(className);
            result.typeName = className;
            result.fieldNames = std::move(names);
            result.typeNames = std::move(fieldTypes);
            result.fieldConstness = std::move(constFields);
            result.fieldDefaults = std::move(defaults);
            return result;
        }

        static IRMetadata moduleImportInit(const std::string& name) {
            IRMetadata result;
            result.kind = IRMetadataKind::ModuleImportInit;
            result.tag = "module_import_init";
            result.moduleName = name;
            return result;
        }

        static IRMetadata symbolRef(SymbolRef value) {
            IRMetadata result;
            result.kind = IRMetadataKind::Symbol;
            result.tag = "symbol";
            result.symbol = std::move(value);
            return result;
        }

        bool readFunctionSignature(
            std::string& name,
            std::string& returnType,
            int& captures,
            std::vector<std::string>& parameterTypes,
            std::string* owner = nullptr,
            std::string* op = nullptr,
            std::string* native = nullptr
        ) const {
            if ((kind != IRMetadataKind::FunctionSignature &&
                 kind != IRMetadataKind::ExternalFunctionSignature) ||
                symbol.kind != SymbolRefKind::Function ||
                symbol.name.empty() ||
                !hasCaptureCount) {
                return false;
            }

            name = symbol.name;
            returnType = typeName;
            captures = captureCount;
            parameterTypes = typeNames;
            if (owner) *owner = ownerType;
            if (op) *op = operatorSymbol;
            if (native) *native = nativeSymbol;
            return true;
        }

        bool readDirectCall(std::string& name) const {
            if (kind != IRMetadataKind::DirectCall &&
                kind != IRMetadataKind::CallTarget ||
                symbol.kind != SymbolRefKind::Function ||
                symbol.name.empty()) {
                return false;
            }

            name = symbol.name;
            return true;
        }

        bool readCallTarget(
            std::string& name,
            std::string& returnType,
            int& captures,
            std::vector<std::string>& parameterTypes
        ) const {
            if (kind != IRMetadataKind::CallTarget ||
                symbol.kind != SymbolRefKind::Function ||
                symbol.name.empty() ||
                !hasCaptureCount) {
                return false;
            }

            name = symbol.name;
            returnType = typeName;
            captures = captureCount;
            parameterTypes = typeNames;
            return true;
        }

        bool readClassLayout(
            std::string& className,
            std::vector<std::string>& names,
            std::vector<std::string>& fieldTypes,
            std::vector<bool>* constFields = nullptr,
            std::vector<ClassFieldDefault>* defaults = nullptr
        ) const {
            if (kind != IRMetadataKind::ClassLayout ||
                symbol.kind != SymbolRefKind::Class ||
                symbol.name.empty() ||
                fieldNames.size() != typeNames.size() ||
                (!fieldConstness.empty() && fieldConstness.size() != fieldNames.size()) ||
                (!fieldDefaults.empty() && fieldDefaults.size() != fieldNames.size())) {
                return false;
            }

            className = symbol.name;
            names = fieldNames;
            fieldTypes = typeNames;
            if (constFields != nullptr) *constFields = fieldConstness;
            if (defaults != nullptr) *defaults = fieldDefaults;
            return true;
        }

        bool empty() const {
            return kind == IRMetadataKind::None;
        }
    };

}
