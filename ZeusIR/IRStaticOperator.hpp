#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRType.hpp"

namespace IRStaticOperator {

    struct FunctionSignature {
        std::string name;
        std::string returnType;
        std::string ownerType;
        std::string operatorSymbol;
        std::vector<std::string> parameterTypes;
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

            for (const auto& entry : functions) {
                const auto& signature = entry.second;
                if (signature.ownerType != lhsType ||
                    signature.operatorSymbol != op ||
                    signature.parameterTypes.size() < 2) {
                    continue;
                }

                if (signature.parameterTypes[0] != lhsType ||
                    signature.parameterTypes[1] != rhsType) {
                    continue;
                }

                return &signature;
            }

            return nullptr;
        }

        const FunctionSignature* findIndexOperator(
            const std::string& containerType,
            const std::vector<std::string>& argumentTypes,
            const std::string& op
        ) const {
            if (containerType.empty() || argumentTypes.empty()) return nullptr;

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
                    if (argumentTypes[i].empty() ||
                        signature.parameterTypes[i + 1] != argumentTypes[i]) {
                        matches = false;
                        break;
                    }
                }
                if (matches) return &signature;
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

            for (const auto& chunk : program) {
                for (const auto& ins : chunk) {
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
                            &signature.operatorSymbol)) {
                        continue;
                    }

                    functions[signature.name] = std::move(signature);
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
                        signature.parameterTypes)) {
                    return;
                }
            }

            std::unordered_map<int, std::string> locals;
            for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                if (!signature.parameterTypes[i].empty()) {
                    locals[static_cast<int>(i)] = signature.parameterTypes[i];
                }
            }

            std::vector<std::string> stack;
            std::string pendingDirectCall;

            auto popType = [&]() {
                if (stack.empty()) return std::string();
                std::string result = stack.back();
                stack.pop_back();
                return result;
            };

            for (auto& ins : chunk) {
                if (ins.op == VMOperator::IR_ANNOTATION) {
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        ins.metadata.readDirectCall(pendingDirectCall);
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

                int localIndex = -1;
                if (isLocalLoad(ins, localIndex)) {
                    auto local = locals.find(localIndex);
                    stack.push_back(local == locals.end() ? std::string() : local->second);
                    continue;
                }

                int globalIndex = -1;
                if (isGlobalLoad(ins, globalIndex)) {
                    auto global = globalTypes.find(globalIndex);
                    stack.push_back(global == globalTypes.end()
                        ? std::string()
                        : global->second);
                    continue;
                }

                if (ins.op == VMOperator::STORE_VALUE_LOCAL ||
                    ins.op == VMOperator::ASSIGN_VALUE_LOCAL) {
                    std::string valueType = popType();
                    if (!ins.args.empty() && std::holds_alternative<LexerType::Value>(ins.args[0]) &&
                        std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(ins.args[0]).data)) {
                        int64_t raw = 0;
                        const auto& indexValue = std::get<LexerType::Value>(ins.args[0]);
                        if (std::get<ZeusType::BigInteger>(indexValue.data).tryToInt64(raw)) {
                            locals[static_cast<int>(raw)] = valueType;
                        }
                    }
                    continue;
                }

                if (ins.op == VMOperator::STORE_VALUE_GLOBAL ||
                    ins.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                    const std::string valueType = popType();
                    if (!isFunctionChunk &&
                        readGlobalStoreIndex(ins, globalIndex)) {
                        globalTypes[globalIndex] = valueType;
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
                    popType();
                    popType();
                    continue;
                }

                if (ins.op == VMOperator::DICT_LOAD_VALUE) {
                    const std::string keyType = popType();
                    const std::string containerType = popType();
                    const FunctionSignature* target = isFunctionChunk
                        ? nullptr
                        : findIndexOperator(containerType, { keyType }, "[]");
                    if (target != nullptr) {
                        ins.metadata = IRType::IRMetadata::directCall(target->name);
                        stack.push_back(target->returnType);
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
                    const FunctionSignature* target = isFunctionChunk
                        ? nullptr
                        : findIndexOperator(
                            containerType, { keyType, valueType }, "[]=");
                    if (target != nullptr) {
                        ins.metadata = IRType::IRMetadata::directCall(target->name);
                    }
                    pendingDirectCall.clear();
                    continue;
                }

                if (isBinaryOperator(ins.op)) {
                    const std::string rhsType = popType();
                    const std::string lhsType = popType();

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
                        stack.push_back(target->returnType);
                    } else {
                        stack.push_back({});
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

                    for (int64_t i = 0; i < argc; ++i) popType();
                    popType();

                    auto target = functions.find(pendingDirectCall);
                    stack.push_back(target == functions.end() ? std::string() : target->second.returnType);
                    pendingDirectCall.clear();
                    continue;
                }

                if (ins.op == VMOperator::POP || ins.op == VMOperator::RETURN) {
                    popType();
                }
            }
        }

    public:
        void annotate(std::vector<std::vector<IRType::IRInstr>>& program) {
            collectSignatures(program);
            std::unordered_map<int, std::string> globalTypes;
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
