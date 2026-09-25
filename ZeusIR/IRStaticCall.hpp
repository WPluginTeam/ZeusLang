#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRType.hpp"

namespace IRStaticCall {

    struct FunctionSignature {
        std::string name;
        std::string returnType;
        int captureCount = 0;
        std::vector<std::string> parameterTypes;
    };

    class Annotator {
        std::unordered_map<std::string, FunctionSignature> functions;

        static bool readStringArg(
            const IRType::IRInstr& ins,
            size_t index,
            std::string& value
        ) {
            if (index >= ins.args.size() ||
                !std::holds_alternative<std::string>(ins.args[index])) {
                return false;
            }

            value = std::get<std::string>(ins.args[index]);
            return true;
        }

        static bool readIntegerArg(
            const IRType::IRInstr& ins,
            size_t index,
            int64_t& value
        ) {
            if (index >= ins.args.size() ||
                !std::holds_alternative<LexerType::Value>(ins.args[index])) {
                return false;
            }

            const auto& argument = std::get<LexerType::Value>(ins.args[index]);
            if (!std::holds_alternative<ZeusType::BigInteger>(argument.data)) {
                return false;
            }

            return std::get<ZeusType::BigInteger>(argument.data).tryToInt64(value);
        }

        static bool readFunctionSignature(
            const IRType::IRInstr& ins,
            FunctionSignature& signature
        ) {
            if (ins.metadata.kind == IRType::IRMetadataKind::FunctionSignature ||
                ins.metadata.kind == IRType::IRMetadataKind::ExternalFunctionSignature) {
                return ins.metadata.readFunctionSignature(
                    signature.name,
                    signature.returnType,
                    signature.captureCount,
                    signature.parameterTypes
                );
            }

            std::string tag;
            if (!readStringArg(ins, 0, tag) ||
                (tag != "function_signature" &&
                 tag != "external_function_signature")) {
                return false;
            }

            int64_t captureCount = 0;
            int64_t parameterCount = 0;
            if (!readStringArg(ins, 1, signature.name) ||
                !readStringArg(ins, 2, signature.returnType) ||
                !readIntegerArg(ins, 3, captureCount) ||
                !readIntegerArg(ins, 4, parameterCount) ||
                captureCount < 0 ||
                parameterCount < 0 ||
                ins.args.size() < 5 + static_cast<size_t>(parameterCount)) {
                return false;
            }

            signature.captureCount = static_cast<int>(captureCount);
            signature.parameterTypes.clear();
            for (int64_t i = 0; i < parameterCount; ++i) {
                std::string typeName;
                if (!readStringArg(ins, 5 + static_cast<size_t>(i), typeName)) {
                    return false;
                }
                signature.parameterTypes.push_back(std::move(typeName));
            }

            return true;
        }

        static bool readDirectCallName(
            const IRType::IRInstr& ins,
            std::string& name
        ) {
            if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall ||
                ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                return ins.metadata.readDirectCall(name);
            }

            return readStringArg(ins, 0, name) && name == "direct_call" &&
                   readStringArg(ins, 1, name);
        }

        static bool isCompleteStaticSignature(const FunctionSignature& signature) {
            if (signature.name.empty() || signature.returnType.empty() ||
                signature.captureCount != 0) {
                return false;
            }

            for (const auto& typeName : signature.parameterTypes) {
                if (typeName.empty()) return false;
            }
            return true;
        }

        static bool canLowerFunctionCall(const FunctionSignature& signature) {
            return !signature.name.empty() && signature.captureCount == 0;
        }

        void collectFunctions(
            const std::vector<std::vector<IRType::IRInstr>>& program
        ) {
            functions.clear();

            for (const auto& chunk : program) {
                for (const auto& ins : chunk) {
                    if (ins.op != VMOperator::IR_ANNOTATION) continue;

                    FunctionSignature signature;
                    if (!readFunctionSignature(ins, signature)) continue;
                    functions[signature.name] = std::move(signature);
                }
            }
        }

        void annotateChunk(std::vector<IRType::IRInstr>& chunk) const {
            bool hasPendingTarget = false;
            bool pendingTargetIsStatic = false;
            IRType::IRMetadata pendingTarget;

            for (auto& ins : chunk) {
                if (ins.op == VMOperator::IR_ANNOTATION) {
                    std::string targetName;
                    if (!readDirectCallName(ins, targetName)) {
                        hasPendingTarget = false;
                        pendingTargetIsStatic = false;
                        continue;
                    }

                    auto target = functions.find(targetName);
                    if (target == functions.end() || !canLowerFunctionCall(target->second)) {
                        hasPendingTarget = false;
                        pendingTargetIsStatic = false;
                        continue;
                    }

                    pendingTargetIsStatic = isCompleteStaticSignature(target->second);

                    pendingTarget = IRType::IRMetadata::callTarget(
                        target->second.name,
                        target->second.returnType,
                        target->second.captureCount,
                        target->second.parameterTypes
                    );
                    if (pendingTargetIsStatic) ins.metadata = pendingTarget;
                    hasPendingTarget = true;
                    continue;
                }

                if (hasPendingTarget && ins.op == VMOperator::CALL_CLOSURE) {
                    ins.metadata = pendingTarget;
                    hasPendingTarget = false;
                    pendingTargetIsStatic = false;
                    continue;
                }

                if (hasPendingTarget && pendingTargetIsStatic) {
                    ins.metadata = pendingTarget;
                }
                hasPendingTarget = false;
                pendingTargetIsStatic = false;
            }
        }

    public:
        void annotate(
            std::vector<std::vector<IRType::IRInstr>>& program
        ) {
            collectFunctions(program);
            for (auto& chunk : program) annotateChunk(chunk);
        }
    };

    inline void annotate(
        std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        Annotator annotator;
        annotator.annotate(program);
    }

}
