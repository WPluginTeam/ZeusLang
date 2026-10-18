#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../ZeusBuiltins/NativeTypeABI.hpp"
#include "IRType.hpp"

namespace IRStaticOperator {

    struct FunctionSignature {
        std::string name;
        std::string returnType;
        std::string ownerType;
        std::string operatorSymbol;
        std::vector<std::string> parameterTypes;
        std::vector<std::string> parameterNames;
        std::vector<bool> parameterIsReference;
        bool isStaticMethod = false;
    };

    struct ClassLayout {
        std::vector<std::string> fieldNames;
        std::vector<std::string> fieldTypes;
    };

    inline std::string operatorSymbol(VMOperator op) {
        switch (op) {
            case VMOperator::ADD: return "+";
            case VMOperator::SUBTRACT: return "-";
            case VMOperator::MULTIPLY: return "*";
            case VMOperator::DIVIDE: return "/";
            case VMOperator::MODULUS: return "%";
            case VMOperator::EQUL: return "==";
            case VMOperator::NOT_EQUAL: return "!=";
            case VMOperator::GREATER_THAN: return ">";
            case VMOperator::LESS_THAN: return "<";
            case VMOperator::GREATER_EQUAL: return ">=";
            case VMOperator::LESS_EQUAL: return "<=";
            default: return {};
        }
    }

    inline bool isBinaryOperator(VMOperator op) {
        return !operatorSymbol(op).empty();
    }

    inline std::string literalType(const LexerType::Value& value) {
        if (std::holds_alternative<ZeusType::BigInteger>(value.data)) return "int";
        if (std::holds_alternative<ZeusType::BigDecimal>(value.data)) return "float";
        if (std::holds_alternative<std::string>(value.data)) return "string";
        if (std::holds_alternative<bool>(value.data)) return "bool";
        return {};
    }

    class Resolver {
        std::unordered_map<std::string, FunctionSignature> functions;
        std::unordered_map<std::string, ClassLayout> classLayouts;
        std::unordered_map<int, std::string> globalBindingTypes;
        std::unordered_map<std::string, std::string> sharedBindingTypes;
        std::unordered_set<std::string> ambiguousSharedBindingTypes;

        static bool isLocalLoad(const IRType::IRInstr& ins, int& index) {
            if (ins.op != VMOperator::LOAD_VALUE || ins.args.size() < 2 ||
                !std::holds_alternative<std::string>(ins.args[0]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[1])) {
                return false;
            }

            if (std::get<std::string>(ins.args[0]) != "local") return false;

            const auto& value = std::get<LexerType::Value>(ins.args[1]);
            if (!std::holds_alternative<ZeusType::BigInteger>(value.data)) return false;

            int64_t raw = 0;
            if (!std::get<ZeusType::BigInteger>(value.data).tryToInt64(raw) || raw < 0) {
                return false;
            }

            index = static_cast<int>(raw);
            return true;
        }

        static bool isGlobalLoad(const IRType::IRInstr& ins, int& index) {
            if (ins.op != VMOperator::LOAD_VALUE || ins.args.size() < 2 ||
                !std::holds_alternative<std::string>(ins.args[0]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[1]) ||
                std::get<std::string>(ins.args[0]) != "global") {
                return false;
            }

            const auto& value = std::get<LexerType::Value>(ins.args[1]);
            if (!std::holds_alternative<ZeusType::BigInteger>(value.data)) {
                return false;
            }

            int64_t raw = 0;
            if (!std::get<ZeusType::BigInteger>(value.data).tryToInt64(raw) ||
                raw < 0 || raw > std::numeric_limits<int>::max()) {
                return false;
            }

            index = static_cast<int>(raw);
            return true;
        }

        static bool readGlobalStoreIndex(
            const IRType::IRInstr& ins,
            int& index
        ) {
            if ((ins.op != VMOperator::STORE_VALUE_GLOBAL &&
                 ins.op != VMOperator::ASSIGN_VALUE_GLOBAL) ||
                ins.args.empty() ||
                !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                return false;
            }

            const auto& value = std::get<LexerType::Value>(ins.args[0]);
            if (!std::holds_alternative<ZeusType::BigInteger>(value.data)) {
                return false;
            }

            int64_t raw = 0;
            if (!std::get<ZeusType::BigInteger>(value.data).tryToInt64(raw) ||
                raw < 0 || raw > std::numeric_limits<int>::max()) {
                return false;
            }

            index = static_cast<int>(raw);
            return true;
        }

        const FunctionSignature* findOperator(
            const std::string& lhsType,
            const std::string& rhsType,
            const std::string& op
        ) const {
            if (lhsType.empty() || rhsType.empty()) return nullptr;

            const FunctionSignature* untypedRightOperandMatch = nullptr;
            for (const auto& entry : functions) {
                const auto& signature = entry.second;
                if (signature.ownerType != lhsType ||
                    signature.operatorSymbol != op ||
                    signature.parameterTypes.size() < 2) {
                    continue;
                }

                // Class operators may declare the VM-injected receiver as an
                // ordinary, untyped first parameter (`func operator[+](this,
                // other)`). Its owner metadata still proves the receiver type.
                if ((!signature.parameterTypes[0].empty() &&
                     signature.parameterTypes[0] != lhsType) ||
                    (!signature.parameterTypes[1].empty() &&
                     signature.parameterTypes[1] != rhsType)) {
                    continue;
                }

                if (signature.parameterTypes[1] == rhsType) return &signature;
                untypedRightOperandMatch = &signature;
            }

            return untypedRightOperandMatch;
        }

        const FunctionSignature* findIndexOperator(
            const std::string& containerType,
            const std::vector<std::string>& argumentTypes,
            const std::string& op,
            bool allowUniqueUnknownArguments = false
        ) const {
            if (containerType.empty() || argumentTypes.empty()) return nullptr;

            const FunctionSignature* match = nullptr;
            for (const auto& entry : functions) {
                const auto& signature = entry.second;
                if (signature.ownerType != containerType ||
                    signature.operatorSymbol != op ||
                    signature.parameterTypes.size() != argumentTypes.size() + 1 ||
                    signature.parameterTypes[0] != containerType) {
                    continue;
                }

                bool matches = true;
                for (size_t i = 0; i < argumentTypes.size(); ++i) {
                    if (argumentTypes[i].empty()) {
                        if (!allowUniqueUnknownArguments) matches = false;
                        continue;
                    }
                    if (signature.parameterTypes[i + 1] != argumentTypes[i]) {
                        matches = false;
                        break;
                    }
                }
                if (!matches) continue;
                if (match != nullptr) return nullptr;
                match = &signature;
            }
            return match;
        }

        const FunctionSignature* findClassMethod(
            const std::string& ownerType,
            const std::string& methodName,
            size_t argumentCount,
            bool staticMethod
        ) const {
            const std::string prefix = ownerType + "::";
            for (const auto& entry : functions) {
                const FunctionSignature& signature = entry.second;
                if (signature.ownerType != ownerType ||
                    signature.isStaticMethod != staticMethod ||
                    signature.name.rfind(prefix, 0) != 0 ||
                    signature.name.substr(prefix.size()) != methodName) {
                    continue;
                }
                const size_t receiverCount = staticMethod ? 0 : 1;
                if (signature.parameterTypes.size() < argumentCount + receiverCount) {
                    continue;
                }
                if (!staticMethod &&
                    (signature.parameterTypes.empty() ||
                     signature.parameterTypes.front() != ownerType)) {
                    continue;
                }
                return &signature;
            }
            return nullptr;
        }

        std::string findFieldType(
            const std::string& objectType,
            const std::string& fieldName
        ) const {
            auto layout = classLayouts.find(objectType);
            if (layout == classLayouts.end()) return {};

            for (size_t i = 0; i < layout->second.fieldNames.size(); ++i) {
                if (layout->second.fieldNames[i] == fieldName &&
                    i < layout->second.fieldTypes.size()) {
                    return layout->second.fieldTypes[i];
                }
            }
            return {};
        }

        void collectSignatures(
            const std::vector<std::vector<IRType::IRInstr>>& program
        ) {
            functions.clear();
            classLayouts.clear();
            globalBindingTypes.clear();

            for (const auto& chunk : program) {
                for (const auto& ins : chunk) {
                    if (ins.op == VMOperator::STORE_VALUE_GLOBAL &&
                        ins.metadata.kind == IRType::IRMetadataKind::GlobalBindingType &&
                        !ins.metadata.typeName.empty()) {
                        int globalIndex = -1;
                        if (readGlobalStoreIndex(ins, globalIndex)) {
                            globalBindingTypes[globalIndex] = ins.metadata.typeName;
                        }
                    }

                    if (ins.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
                        std::string className;
                        ClassLayout layout;
                        if (ins.metadata.readClassLayout(
                                className,
                                layout.fieldNames,
                                layout.fieldTypes)) {
                            classLayouts[className] = std::move(layout);
                        }
                        continue;
                    }

                    if (ins.op != VMOperator::IR_ANNOTATION ||
                        ins.metadata.kind != IRType::IRMetadataKind::FunctionSignature) {
                        continue;
                    }

                    FunctionSignature signature;
                    int captures = 0;
                    if (!ins.metadata.readFunctionSignature(
                            signature.name,
                            signature.returnType,
                            captures,
                            signature.parameterTypes,
                            &signature.ownerType,
                            &signature.operatorSymbol,
                            nullptr,
                            &signature.parameterNames,
                            &signature.parameterIsReference)) {
                        continue;
                    }
                    signature.isStaticMethod = ins.metadata.isStaticMethod;

                    functions[signature.name] = std::move(signature);
                }
            }
        }

        void collectSharedBindingTypes(
            const std::vector<std::vector<IRType::IRInstr>>& program
        ) {
            sharedBindingTypes.clear();
            ambiguousSharedBindingTypes.clear();

            for (const auto& chunk : program) {
                for (const auto& ins : chunk) {
                    if (ins.op != VMOperator::STORE_VALUE_SHARED ||
                        ins.args.empty() ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        continue;
                    }

                    const std::string& name = std::get<std::string>(ins.args[0]);
                    if (name.empty() || ins.metadata.kind !=
                            IRType::IRMetadataKind::SharedBindingType ||
                        ins.metadata.typeName.empty()) {
                        ambiguousSharedBindingTypes.insert(name);
                        sharedBindingTypes.erase(name);
                        continue;
                    }

                    if (ambiguousSharedBindingTypes.find(name) !=
                        ambiguousSharedBindingTypes.end()) {
                        continue;
                    }

                    auto existing = sharedBindingTypes.find(name);
                    if (existing == sharedBindingTypes.end()) {
                        sharedBindingTypes.emplace(name, ins.metadata.typeName);
                    } else if (existing->second != ins.metadata.typeName) {
                        sharedBindingTypes.erase(existing);
                        ambiguousSharedBindingTypes.insert(name);
                    }
                }
            }
        }

        void resolveChunk(
            std::vector<IRType::IRInstr>& chunk,
            std::unordered_map<int, std::string>& globalTypes
        ) const {
            if (chunk.empty()) return;
            const bool isFunctionChunk =
                chunk.front().op == VMOperator::FUNC_BEGIN;

            const IRType::IRInstr* signatureInstruction = nullptr;
            if (isFunctionChunk) {
                for (const auto& ins : chunk) {
                    if (ins.op == VMOperator::IR_ANNOTATION &&
                        ins.metadata.kind == IRType::IRMetadataKind::FunctionSignature) {
                        signatureInstruction = &ins;
                        break;
                    }
                }
            }

            if (isFunctionChunk && signatureInstruction == nullptr) return;

            FunctionSignature signature;
            if (signatureInstruction != nullptr) {
                int captures = 0;
                if (!signatureInstruction->metadata.readFunctionSignature(
                        signature.name,
                        signature.returnType,
                        captures,
                        signature.parameterTypes,
                        &signature.ownerType, &signature.operatorSymbol)) {
                    return;
                }
            }

            std::unordered_map<int, std::string> locals;
            for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                if (!signature.parameterTypes[i].empty()) {
                    locals[static_cast<int>(i)] = signature.parameterTypes[i];
                }
            }
            std::unordered_map<int, std::string> declaredLocalTypes;
            for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                const auto& type = signature.parameterTypes[i];
                if (ZeusBuiltins::isNativeFloatingPointType(type) ||
                    ZeusBuiltins::findNativeSignedIntegerABI(type)) {
                    declaredLocalTypes[static_cast<int>(i)] = type;
                }
            }
            for (const auto& ins : chunk) {
                if (ins.op != VMOperator::STORE_VALUE_LOCAL ||
                    ins.metadata.kind != IRType::IRMetadataKind::LocalBindingType ||
                    ins.metadata.typeName.empty() || ins.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                    continue;
                }
                const auto& indexValue = std::get<LexerType::Value>(ins.args[0]);
                if (!std::holds_alternative<ZeusType::BigInteger>(indexValue.data)) {
                    continue;
                }
                int64_t raw = -1;
                if (std::get<ZeusType::BigInteger>(indexValue.data).tryToInt64(raw) &&
                    raw >= 0 && raw <= std::numeric_limits<int>::max()) {
                    const int index = static_cast<int>(raw);
                    declaredLocalTypes[index] = ins.metadata.typeName;
                    locals[index] = ins.metadata.typeName;
                }
            }
            // Globals assigned inside a function need a local type flow too:
            // the module-level map cannot safely be updated from one function,
            // but subsequent reads in that same function must see the value
            // just stored there.
            std::unordered_map<int, std::string> functionGlobalTypes;

            struct FlowType : std::string {
                bool borrowed = true;
                std::string nativeFunctionName;
                FlowType() = default;
                FlowType(const std::string& type, bool borrow = true)
                    : std::string(type), borrowed(borrow) {}
                FlowType(const std::string& type, bool borrow,
                         std::string nativeName)
                    : std::string(type), borrowed(borrow),
                      nativeFunctionName(std::move(nativeName)) {}
                FlowType(const char* type) : std::string(type) {}
            };
            std::vector<FlowType> stack;
            std::string pendingDirectCall;

            auto popType = [&]() {
                if (stack.empty()) return FlowType();
                FlowType result = stack.back();
                stack.pop_back();
                return result;
            };

            auto readOperandCount = [](
                const IRType::IRInstr& instruction,
                size_t argumentIndex,
                int64_t& count
            ) {
                if (argumentIndex >= instruction.args.size() ||
                    !std::holds_alternative<LexerType::Value>(
                        instruction.args[argumentIndex])) {
                    return false;
                }
                const auto& value = std::get<LexerType::Value>(
                    instruction.args[argumentIndex]);
                return std::holds_alternative<ZeusType::BigInteger>(value.data) &&
                    std::get<ZeusType::BigInteger>(value.data).tryToInt64(count) &&
                    count >= 0;
            };

            auto popValues = [&](int64_t count) {
                if (count < 0 || static_cast<uint64_t>(count) > stack.size()) {
                    stack.clear();
                    return false;
                }
                for (int64_t i = 0; i < count; ++i) popType();
                return true;
            };

            auto valueResultType = [](const std::string& returnType) {
                return !returnType.empty() && returnType.front() == '&'
                    ? returnType.substr(1) : returnType;
            };

            auto annotateCopy = [&](IRType::IRInstr& store, const FlowType& value) {
                store.metadata.classAssignmentCopy = false;
                store.metadata.classCopyType.clear();
                store.metadata.classCopyHook.clear();
                if (!value.borrowed || (!value.empty() && value.front() == '&') ||
                    (!store.metadata.typeName.empty() && store.metadata.typeName.front() == '&')) return;
                store.metadata.classAssignmentCopy = true;
                store.metadata.classCopyType = value;
                for (const auto& function : functions) {
                    const auto& candidate = function.second;
                    if (candidate.ownerType == value && candidate.operatorSymbol == "@=" &&
                        !candidate.isStaticMethod && candidate.parameterTypes.size() == 1) {
                        store.metadata.classCopyHook = candidate.name;
                        break;
                    }
                }
            };

            for (size_t instructionIndex = 0;
                 instructionIndex < chunk.size(); ++instructionIndex) {
                auto& ins = chunk[instructionIndex];

                // Branch instructions consume their condition at runtime.
                // Keep the abstract operand stack aligned when scanning a
                // function linearly; stale condition types otherwise shift
                // every later LOAD_VALUE type after an if/loop.
                if (ins.op == VMOperator::JUMP_IF_TRUE ||
                    ins.op == VMOperator::JUMP_IF_FALSE) {
                    popType();
                    stack.clear();
                    pendingDirectCall.clear();
                    continue;
                }
                if (ins.op == VMOperator::JUMP ||
                    ins.op == VMOperator::LOOP_BREAK) {
                    stack.clear();
                    pendingDirectCall.clear();
                    continue;
                }

                // A value-type operator[[]=] returns the replacement value.
                // The assignment generator already emitted POP for the old
                // in-place container path; turn that POP into the base-slot
                // assignment only after the resolver proves this is a
                // same-type, non-class value setter.
                if (ins.op == VMOperator::POP && instructionIndex > 0) {
                    const auto& store = chunk[instructionIndex - 1];
                    if (store.op == VMOperator::DICT_STORE_VALUE &&
                        store.metadata.isSubscriptValueWriteback) {
                        const auto& target = store.metadata.subscriptWritebackTarget;
                        ins.line = store.line;
                        ins.args.clear();
                        switch (target.kind) {
                            case IRType::SymbolRefKind::Local:
                                ins.op = VMOperator::ASSIGN_VALUE_LOCAL;
                                ins.args.push_back(LexerType::Value(target.index));
                                break;
                            case IRType::SymbolRefKind::Global:
                                ins.op = VMOperator::ASSIGN_VALUE_GLOBAL;
                                ins.args.push_back(LexerType::Value(target.index));
                                break;
                            case IRType::SymbolRefKind::Upvalue:
                                ins.op = VMOperator::ASSIGN_VALUE_UPVALUE;
                                ins.args.push_back(LexerType::Value(target.index));
                                break;
                            case IRType::SymbolRefKind::Shared:
                                ins.op = VMOperator::ASSIGN_VALUE_SHARED;
                                ins.args.push_back(target.name);
                                break;
                            default:
                                break;
                        }
                    }
                }

                if (ins.op == VMOperator::IR_ANNOTATION) {
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall ||
                        ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        ins.metadata.readDirectCall(pendingDirectCall);
                    }
                    continue;
                }

                if (ins.op == VMOperator::HOTSPOT && ins.args.size() >= 4) {
                    int64_t hotspotId = -1;
                    int64_t globalIndex = -1;
                    if (readOperandCount(ins, 0, hotspotId) && hotspotId == 0 &&
                        readOperandCount(ins, 2, globalIndex) &&
                        globalIndex <= std::numeric_limits<int>::max()) {
                        std::string valueType;
                        if (std::holds_alternative<LexerType::Value>(ins.args[1])) {
                            valueType = literalType(
                                std::get<LexerType::Value>(ins.args[1]));
                        } else if (std::holds_alternative<ZeusType::Bytes>(ins.args[1])) {
                            valueType = "bytes";
                        }
                        if (!valueType.empty()) {
                            auto& types = isFunctionChunk
                                ? functionGlobalTypes : globalTypes;
                            types[static_cast<int>(globalIndex)] = valueType;
                        }
                    }
                    continue;
                }

                if (ins.op == VMOperator::PUSH && !ins.args.empty() &&
                    std::holds_alternative<LexerType::Value>(ins.args[0])) {
                    stack.push_back(literalType(std::get<LexerType::Value>(ins.args[0])));
                    continue;
                }

                if (ins.op == VMOperator::PUSH && !ins.args.empty() &&
                    std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                    stack.push_back("bytes");
                    continue;
                }

                if (ins.op == VMOperator::PUSH && !ins.args.empty() &&
                    std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                    stack.push_back("nil");
                    continue;
                }

                if (ins.op == VMOperator::LOAD_NATIVE) {
                    stack.push_back(ins.metadata.kind ==
                            IRType::IRMetadataKind::NativeFunction
                        ? FlowType(std::string(), true, ins.metadata.nativeSymbol)
                        : FlowType());
                    continue;
                }

                if (ins.op == VMOperator::LOAD_VALUE &&
                    ins.metadata.symbol.kind == IRType::SymbolRefKind::Class) {
                    stack.push_back("class:" + ins.metadata.symbol.name);
                    continue;
                }

                if (ins.op == VMOperator::LIST_BUILD || ins.op == VMOperator::DICT_BUILD) {
                    int64_t count = 0;
                    if (!readOperandCount(ins, 0, count) ||
                        (ins.op == VMOperator::DICT_BUILD &&
                         count > std::numeric_limits<int64_t>::max() / 2) ||
                        !popValues(count * (ins.op == VMOperator::DICT_BUILD ? 2 : 1))) {
                        stack.clear();
                        continue;
                    }
                    stack.push_back(ins.op == VMOperator::DICT_BUILD ? "dict" : "Array");
                    continue;
                }

                if (ins.op == VMOperator::LIST_LENGTH || ins.op == VMOperator::DICT_KEYS_BUILD) {
                    popType();
                    stack.push_back(ins.op == VMOperator::LIST_LENGTH ? "int" : "Array");
                    continue;
                }

                if (ins.op == VMOperator::LLVM_ADDR) {
                    stack.push_back("int");
                    continue;
                }
                if (ins.op == VMOperator::LLVM_SIZEOF) {
                    stack.push_back("int");
                    continue;
                }
                if (ins.op == VMOperator::LLVM_FADDR) {
                    if (ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        (std::get<std::string>(ins.args[1]) == "length" ||
                         std::get<std::string>(ins.args[1]) == "raw_range")) {
                        popType();
                    }
                    popType();
                    stack.push_back(!ins.args.empty() &&
                        std::holds_alternative<std::string>(ins.args[0])
                        ? std::get<std::string>(ins.args[0]) : std::string());
                    continue;
                }
                if (ins.op == VMOperator::LLVM_FSTORE) {
                    const auto value = popType();
                    annotateCopy(ins, value);
                    if (ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        (std::get<std::string>(ins.args[1]) == "length" ||
                         std::get<std::string>(ins.args[1]) == "raw_range")) {
                        popType();
                    }
                    popType();
                    continue;
                }

                if (ins.op == VMOperator::OBJ_BUILD ||
                    ins.op == VMOperator::OBJ_BUILD_SHARED ||
                    ins.op == VMOperator::NEW_OBJ) {
                    int64_t argumentCount = 0;
                    if (!readOperandCount(ins, 0, argumentCount) ||
                        !popValues(argumentCount)) {
                        continue;
                    }
                    std::string classType = popType();
                    std::string genericClassName;
                    std::vector<std::string> genericTypeArguments;
                    std::string specializedClassName;
                    if (ins.metadata.readClassInstantiation(
                            genericClassName, genericTypeArguments,
                            &specializedClassName) &&
                        !specializedClassName.empty()) {
                        classType = specializedClassName;
                    }
                    if (ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1])) {
                        const std::string& explicitType = std::get<std::string>(ins.args[1]);
                        if (classLayouts.find(explicitType) != classLayouts.end()) {
                            classType = explicitType;
                        }
                    }
                    const std::string classPrefix = "class:";
                    if (classType.rfind(classPrefix, 0) == 0) {
                        classType.erase(0, classPrefix.size());
                    }
                    stack.push_back(FlowType(classLayouts.find(classType) == classLayouts.end()
                        ? std::string() : classType, false));
                    continue;
                }

                if (ins.op == VMOperator::OBJ_CALL_METHOD) {
                    int64_t argumentCount = 0;
                    if (ins.args.empty() ||
                        !std::holds_alternative<std::string>(ins.args[0]) ||
                        !readOperandCount(ins, 1, argumentCount)) {
                        stack.clear();
                        continue;
                    }
                    if (stack.size() < static_cast<size_t>(argumentCount + 1)) {
                        stack.clear();
                        continue;
                    }
                    std::vector<std::string> argumentTypes(static_cast<size_t>(argumentCount));
                    for (int64_t i = argumentCount - 1; i >= 0; --i) {
                        argumentTypes[static_cast<size_t>(i)] = popType();
                    }
                    const std::string receiverType = popType();
                    const std::string classPrefix = "class:";
                    const bool staticMethod = receiverType.rfind(classPrefix, 0) == 0;
                    const std::string ownerType = staticMethod
                        ? receiverType.substr(classPrefix.size()) : receiverType;
                    const FunctionSignature* method = findClassMethod(
                        ownerType,
                        std::get<std::string>(ins.args[0]),
                        static_cast<size_t>(argumentCount),
                        staticMethod
                    );
                    std::string existingTarget;
                    if (ins.metadata.readDirectCall(existingTarget)) {
                        const auto existing = functions.find(existingTarget);
                        if (existing != functions.end() &&
                            existing->second.ownerType == ownerType &&
                            existing->second.isStaticMethod == staticMethod) {
                            method = &existing->second;
                        }
                    }
                    if (method != nullptr) {
                        // Preserve the fact that this call has a statically
                        // resolved receiver for later backend analyses. The VM
                        // ignores this metadata; LLVM still performs its usual
                        // lowering from the runtime NativeValue.
                        const auto argumentSources = ins.metadata.callArguments;
                        const auto explicitTypeArguments =
                            ins.metadata.explicitTypeArguments;
                        const bool genericVmDispatch =
                            ins.metadata.tag == "generic_vm_specialization";
                        ins.metadata = IRType::IRMetadata::callTarget(
                            method->name, method->returnType, 0, method->parameterTypes,
                            method->parameterNames, method->parameterIsReference);
                        ins.metadata.tag = genericVmDispatch
                            ? "generic_vm_specialization"
                            : "statically_resolved_method";
                        ins.metadata.callArguments = argumentSources;
                        ins.metadata.explicitTypeArguments = explicitTypeArguments;
                        if (!staticMethod) argumentTypes.insert(argumentTypes.begin(), ownerType);
                        ins.metadata.callArgumentTypes = std::move(argumentTypes);
                    }
                    const std::string methodReturnType = method == nullptr
                        ? std::string() : method->returnType;
                    stack.push_back(FlowType(
                        methodReturnType,
                        !methodReturnType.empty() && methodReturnType.front() == '&'));
                    continue;
                }

                if (ins.op == VMOperator::LOAD_VALUE_SHARED) {
                    std::string typeName;
                    if (!ins.args.empty() &&
                        std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        const auto sharedType = sharedBindingTypes.find(name);
                        if (sharedType != sharedBindingTypes.end() &&
                            ambiguousSharedBindingTypes.find(name) ==
                                ambiguousSharedBindingTypes.end()) {
                            typeName = sharedType->second;
                            ins.metadata = IRType::IRMetadata::sharedBindingType(typeName);
                        }
                    }
                    stack.push_back(typeName);
                    continue;
                }

                int localIndex = -1;
                if (isLocalLoad(ins, localIndex)) {
                    auto local = locals.find(localIndex);
                    stack.push_back(local == locals.end() ? std::string() : local->second);
                    continue;
                }

                int globalIndex = -1;
                if (isGlobalLoad(ins, globalIndex)) {
                    const auto localGlobal = isFunctionChunk
                        ? functionGlobalTypes.find(globalIndex)
                        : functionGlobalTypes.end();
                    const auto global = globalTypes.find(globalIndex);
                    if (localGlobal != functionGlobalTypes.end()) {
                        if (!localGlobal->second.empty()) {
                            stack.push_back(localGlobal->second);
                        } else {
                            stack.push_back(global == globalTypes.end()
                                ? std::string() : global->second);
                        }
                    } else {
                        stack.push_back(global == globalTypes.end()
                            ? std::string()
                            : global->second);
                    }
                    continue;
                }

                if (ins.op == VMOperator::STORE_VALUE_LOCAL ||
                    ins.op == VMOperator::ASSIGN_VALUE_LOCAL) {
                    const auto value = popType();
                    annotateCopy(ins, value);
                    std::string valueType = value;
                    if (!ins.args.empty() && std::holds_alternative<LexerType::Value>(ins.args[0]) &&
                        std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(ins.args[0]).data)) {
                        int64_t raw = 0;
                        const auto& indexValue = std::get<LexerType::Value>(ins.args[0]);
                        if (std::get<ZeusType::BigInteger>(indexValue.data).tryToInt64(raw)) {
                            const int index = static_cast<int>(raw);
                            const auto declared = declaredLocalTypes.find(index);
                            if (declared != declaredLocalTypes.end() &&
                                (ZeusBuiltins::isNativeFloatingPointType(declared->second) ||
                                 ZeusBuiltins::findNativeSignedIntegerABI(declared->second))) {
                                locals[index] = declared->second;
                            } else if (!valueType.empty()) {
                                locals[index] = valueType;
                            } else {
                                locals[index] = declared == declaredLocalTypes.end()
                                    ? std::string() : declared->second;
                            }
                        }
                    }
                    continue;
                }

                if (ins.op == VMOperator::STORE_VALUE_SHARED ||
                    ins.op == VMOperator::ASSIGN_VALUE_SHARED) {
                    const auto sharedValue = popType();
                    if (ins.op == VMOperator::ASSIGN_VALUE_SHARED &&
                        !ins.args.empty() &&
                        std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        const auto sharedType = sharedBindingTypes.find(name);
                        if (sharedType != sharedBindingTypes.end() &&
                            ambiguousSharedBindingTypes.find(name) ==
                                ambiguousSharedBindingTypes.end()) {
                            ins.metadata = IRType::IRMetadata::sharedBindingType(
                                sharedType->second);
                        }
                    }
                    annotateCopy(ins, sharedValue);
                    continue;
                }

                if (ins.op == VMOperator::STORE_VALUE_GLOBAL ||
                    ins.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                    const auto value = popType();
                    annotateCopy(ins, value);
                    const std::string valueType = value;
                    if (readGlobalStoreIndex(ins, globalIndex)) {
                        std::string storedClassType;
                        for (const auto& argument : ins.args) {
                            if (!std::holds_alternative<std::string>(argument)) continue;
                            const std::string& candidate = std::get<std::string>(argument);
                            if (classLayouts.find(candidate) != classLayouts.end()) {
                                storedClassType = "class:" + candidate;
                            }
                        }
                        auto& types = isFunctionChunk
                            ? functionGlobalTypes : globalTypes;
                        std::string storedType = storedClassType.empty()
                            ? valueType : storedClassType;
                        const auto declared = globalBindingTypes.find(globalIndex);
                        if (declared != globalBindingTypes.end()) {
                            if (storedType.empty() ||
                                ZeusBuiltins::isNativeFloatingPointType(declared->second) ||
                                ZeusBuiltins::findNativeSignedIntegerABI(declared->second)) {
                                storedType = declared->second;
                            }
                        }
                        types[globalIndex] = std::move(storedType);
                    }
                    continue;
                }

                if (ins.op == VMOperator::OBJ_LOAD_ATTR) {
                    const std::string objectType = popType();
                    std::string fieldName;
                    if (!ins.args.empty() && std::holds_alternative<std::string>(ins.args[0])) {
                        fieldName = std::get<std::string>(ins.args[0]);
                    }
                    stack.push_back(findFieldType(objectType, fieldName));
                    continue;
                }

                if (ins.op == VMOperator::OBJ_STORE_ATTR) {
                    const auto value = popType();
                    const auto object = popType();
                    annotateCopy(ins, value);
                    if (!ins.args.empty() && std::holds_alternative<std::string>(ins.args[0])) {
                        const auto field = findFieldType(object, std::get<std::string>(ins.args[0]));
                        if (!field.empty() && field.front() == '&') ins.metadata.classAssignmentCopy = false;
                    }
                    continue;
                }

                if (ins.op == VMOperator::DICT_LOAD_VALUE) {
                    const std::string keyType = popType();
                    const std::string containerType = popType();
                    if (containerType == "&bytes") {
                        ins.metadata = IRType::IRMetadata::rawBytesRangeIndexLoad();
                        stack.push_back("int");
                        pendingDirectCall.clear();
                        continue;
                    }
                    const bool nativeValueType =
                        ZeusBuiltins::findNativeTypeABI(containerType) != nullptr;
                    const FunctionSignature* target =
                        (!isFunctionChunk || nativeValueType)
                            ? findIndexOperator(
                                containerType, { keyType }, "[]", nativeValueType)
                            : nullptr;
                    if (target != nullptr) {
                        ins.metadata = IRType::IRMetadata::directCall(target->name);
                        stack.push_back(FlowType(valueResultType(target->returnType), false));
                    } else {
                        stack.push_back({});
                    }
                    pendingDirectCall.clear();
                    continue;
                }

                if (ins.op == VMOperator::DICT_STORE_VALUE) {
                    const std::string valueType = popType();
                    const std::string keyType = popType();
                    const std::string containerType = popType();
                    if (containerType == "&bytes") {
                        ins.metadata = IRType::IRMetadata::rawBytesRangeIndexStore();
                        pendingDirectCall.clear();
                        continue;
                    }
                    const bool nativeValueType =
                        ZeusBuiltins::findNativeTypeABI(containerType) != nullptr;
                    const FunctionSignature* candidate = findIndexOperator(
                        containerType, { keyType, valueType }, "[]=", nativeValueType);
                    const bool isValueSetter = candidate != nullptr &&
                        candidate->returnType == containerType &&
                        classLayouts.find(containerType) == classLayouts.end();
                    const FunctionSignature* target =
                        (!isFunctionChunk || isValueSetter) ? candidate : nullptr;
                    if (target != nullptr) {
                        const IRType::SymbolRef writebackTarget =
                            ins.metadata.subscriptWritebackTarget;
                        ins.metadata = IRType::IRMetadata::directCall(target->name);
                        if (isValueSetter && writebackTarget.isResolved()) {
                            ins.metadata.subscriptWritebackTarget = writebackTarget;
                            ins.metadata.isSubscriptValueWriteback = true;
                            stack.push_back(FlowType(valueResultType(target->returnType), false));
                        }
                    }
                    pendingDirectCall.clear();
                    continue;
                }

                if (isBinaryOperator(ins.op)) {
                    const std::string rhsType = popType();
                    const std::string lhsType = popType();

                    const bool leftDouble = ZeusBuiltins::isNativeFloatingPointType(lhsType);
                    const bool rightDouble = ZeusBuiltins::isNativeFloatingPointType(rhsType);
                    const auto isNumeric = [](const std::string& type) {
                        return type == "int" || type == "bigint" || type == "float" ||
                            ZeusBuiltins::isNativeFloatingPointType(type) ||
                            ZeusBuiltins::findNativeSignedIntegerABI(type) != nullptr;
                    };
                    if ((leftDouble && isNumeric(rhsType)) ||
                        (rightDouble && isNumeric(lhsType))) {
                        const auto* nativeType = ZeusBuiltins::findNativeTypeABI(
                            leftDouble ? lhsType : rhsType);
                        ins.metadata.typeName = std::string(nativeType->canonicalName);
                        const std::string symbol = operatorSymbol(ins.op);
                        const bool comparison = symbol == "==" || symbol == "!=" ||
                            symbol == "<" || symbol == ">" || symbol == "<=" || symbol == ">=";
                        stack.push_back(comparison ? "bool" : ins.metadata.typeName);
                        pendingDirectCall.clear();
                        continue;
                    }

                    if (ins.metadata.kind ==
                        IRType::IRMetadataKind::IntrinsicOperator) {
                        const std::string symbol = operatorSymbol(ins.op);
                        const bool isComparison = symbol == "==" || symbol == "!=" ||
                            symbol == ">" || symbol == "<" || symbol == ">=" ||
                            symbol == "<=";
                        stack.push_back(isComparison ? "bool" : lhsType);
                        pendingDirectCall.clear();
                        continue;
                    }

                    const FunctionSignature* target = findOperator(
                        lhsType,
                        rhsType,
                        operatorSymbol(ins.op)
                    );

                    if (target != nullptr) {
                        ins.metadata = IRType::IRMetadata::directCall(target->name);
                        stack.push_back(FlowType(valueResultType(target->returnType), false));
                    } else {
                        // Primitive operations need no operator function. Their
                        // result types must still reach subsequent typed calls.
                        const std::string symbol = operatorSymbol(ins.op);
                        const bool comparison = symbol == "==" || symbol == "!=" ||
                            symbol == "<" || symbol == ">" || symbol == "<=" || symbol == ">=";
                        const bool leftNumeric = lhsType == "int" || lhsType == "bigint" ||
                            lhsType == "float";
                        const bool rightNumeric = rhsType == "int" || rhsType == "bigint" ||
                            rhsType == "float";
                        if (leftNumeric && rightNumeric) {
                            stack.push_back(comparison ? "bool" :
                                lhsType == "float" || rhsType == "float" ? "float" :
                                lhsType == "bigint" || rhsType == "bigint" ? "bigint" : "int");
                        } else if (lhsType == rhsType &&
                                   (lhsType == "string" || lhsType == "bytes") &&
                                   (comparison || symbol == "+")) {
                            stack.push_back(comparison ? "bool" : lhsType);
                        } else if (lhsType == "bool" && rhsType == "bool" && comparison) {
                            stack.push_back("bool");
                        } else {
                            stack.push_back({});
                        }
                    }
                    pendingDirectCall.clear();
                    continue;
                }

                if (ins.op == VMOperator::CALL_CLOSURE) {
                    int64_t argc = 0;
                    if (!ins.args.empty() && std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        const auto& value = std::get<LexerType::Value>(ins.args[0]);
                        if (std::holds_alternative<ZeusType::BigInteger>(value.data)) {
                            std::get<ZeusType::BigInteger>(value.data).tryToInt64(argc);
                        }
                    }

                    std::vector<std::string> argumentTypes(
                        static_cast<size_t>(std::max<int64_t>(0, argc)));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        argumentTypes[static_cast<size_t>(i)] = popType();
                    }
                    popType();

                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        ins.metadata.callArgumentTypes = std::move(argumentTypes);
                    }

                    auto target = functions.find(pendingDirectCall);
                    const std::string returnType = target == functions.end()
                        ? std::string() : target->second.returnType;
                    stack.push_back(FlowType(
                        returnType,
                        !returnType.empty() && returnType.front() == '&'));
                    pendingDirectCall.clear();
                    continue;
                }

                if (ins.op == VMOperator::CALL_NATIVE) {
                    int64_t argc = 0;
                    if (!readOperandCount(ins, 0, argc) ||
                        static_cast<uint64_t>(argc) + 1 > stack.size()) {
                        stack.clear();
                        pendingDirectCall.clear();
                        continue;
                    }
                    for (int64_t i = 0; i < argc; ++i) popType();
                    const FlowType callee = popType();
                    std::string returnType;
                    if (!callee.nativeFunctionName.empty()) {
                        const auto function = functions.find(
                            callee.nativeFunctionName);
                        if (function != functions.end()) {
                            returnType = function->second.returnType;
                        } else if (const auto* builtin =
                                       ZeusBuiltins::findBuiltinFunctionType(
                                           callee.nativeFunctionName)) {
                            returnType = std::string(builtin->returnType);
                        }
                    }
                    stack.push_back(returnType.empty()
                        ? FlowType()
                        : FlowType(valueResultType(returnType), false));
                    pendingDirectCall.clear();
                    continue;
                }

                if (ins.op == VMOperator::RETURN) {
                    const auto value = popType();
                    // Locals already own their result; returning them transfers it.
                    // Receivers, parameters and field reads remain borrowed.
                    bool transfersLocal = false;
                    if (instructionIndex > 0) {
                        int localIndex = -1;
                        const bool returnsSharedValue =
                            !value.empty() && value.front() == '&';
                        transfersLocal = !returnsSharedValue &&
                            isLocalLoad(chunk[instructionIndex - 1], localIndex) &&
                            localIndex >= static_cast<int>(signature.parameterTypes.size());
                    }
                    if (!transfersLocal && !signature.returnType.empty() &&
                        signature.returnType.front() != '&') {
                        FlowType copySource = value;
                        if (!copySource.empty() && copySource.front() == '&') {
                            copySource = FlowType(valueResultType(copySource), true);
                        }
                        annotateCopy(ins, copySource);
                        ins.metadata.classCopySkipHook = signature.operatorSymbol == "@=";
                    }
                    continue;
                }
                if (ins.op == VMOperator::POP) {
                    popType();
                }
            }
        }

    public:
        void annotate(std::vector<std::vector<IRType::IRInstr>>& program) {
            collectSignatures(program);
            collectSharedBindingTypes(program);
            std::unordered_map<int, std::string> globalTypes = globalBindingTypes;
            for (auto& chunk : program) resolveChunk(chunk, globalTypes);
        }
    };

    inline void annotate(
        std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        Resolver resolver;
        resolver.annotate(program);
    }

}
