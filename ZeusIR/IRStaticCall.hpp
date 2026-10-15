#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRType.hpp"

namespace IRStaticCall {

    struct FunctionSignature {
        std::string name;
        std::string returnType;
        std::string ownerType;
        int captureCount = 0;
        bool isExternal = false;
        bool isStaticMethod = false;
        std::vector<std::string> parameterTypes;
        std::vector<std::string> parameterNames;
        std::vector<bool> parameterIsReference;
    };

    class Annotator {
        struct FunctionValue {
            std::string name;
            std::string className;
        };

        std::unordered_map<std::string, FunctionSignature> functions;
        std::unordered_map<int64_t, FunctionSignature> functionsByChunk;
        std::unordered_map<int64_t, std::string> classGlobalTypes;
        std::unordered_set<int64_t> functionGlobalWrites;
        std::vector<std::unordered_set<int64_t>> chunkGlobalWrites;

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
                signature.isExternal = ins.metadata.kind ==
                    IRType::IRMetadataKind::ExternalFunctionSignature;
                signature.isStaticMethod = ins.metadata.isStaticMethod;
                return ins.metadata.readFunctionSignature(
                    signature.name,
                    signature.returnType,
                    signature.captureCount,
                    signature.parameterTypes,
                    &signature.ownerType,
                    nullptr,
                    nullptr,
                    &signature.parameterNames,
                    &signature.parameterIsReference
                );
            }

            std::string tag;
            if (!readStringArg(ins, 0, tag) ||
                (tag != "function_signature" &&
                 tag != "external_function_signature")) {
                return false;
            }
            signature.isExternal = tag == "external_function_signature";

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
                if (typeName.empty() || typeName == "unknown" || typeName == "any") {
                    return false;
                }
            }
            return signature.returnType != "unknown" &&
                signature.returnType != "any";
        }

        static bool isStaticFunctionValue(const FunctionSignature& signature) {
            return !signature.isExternal &&
                (signature.ownerType.empty() || signature.isStaticMethod) &&
                isCompleteStaticSignature(signature);
        }

        FunctionValue staticClassMethod(
            const std::string& className,
            const std::string& methodName
        ) const {
            const auto function = functions.find(className + "::" + methodName);
            if (function == functions.end() ||
                function->second.ownerType != className ||
                !function->second.isStaticMethod ||
                !isStaticFunctionValue(function->second)) {
                return {};
            }
            return FunctionValue{function->second.name, {}};
        }

        static bool canLowerFunctionCall(const FunctionSignature& signature) {
            return !signature.name.empty() && signature.captureCount == 0;
        }

        void collectFunctions(
            const std::vector<std::vector<IRType::IRInstr>>& program
        ) {
            functions.clear();
            functionsByChunk.clear();
            classGlobalTypes.clear();
            functionGlobalWrites.clear();
            chunkGlobalWrites.clear();
            chunkGlobalWrites.resize(program.size());
            std::unordered_set<std::string> classesWithLayouts;

            for (size_t chunkIndex = 0; chunkIndex < program.size(); ++chunkIndex) {
                const auto& chunk = program[chunkIndex];
                const bool isFunctionBody = std::any_of(
                    chunk.begin(), chunk.end(), [](const IRType::IRInstr& ins) {
                        return ins.op == VMOperator::FUNC_BEGIN;
                    });
                for (const auto& ins : chunk) {
                    if (ins.op == VMOperator::STORE_VALUE_GLOBAL ||
                        ins.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                        int64_t index = -1;
                        if (readIntegerArg(ins, 0, index)) {
                            chunkGlobalWrites[chunkIndex].insert(index);
                            if (isFunctionBody) {
                                functionGlobalWrites.insert(index);
                            }
                        }
                    }
                    if (ins.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
                        std::string className;
                        std::vector<std::string> fieldNames;
                        std::vector<std::string> fieldTypes;
                        if (ins.metadata.readClassLayout(
                                className, fieldNames, fieldTypes)) {
                            classesWithLayouts.insert(std::move(className));
                        }
                        continue;
                    }
                    if (ins.op != VMOperator::IR_ANNOTATION) continue;

                    FunctionSignature signature;
                    if (!readFunctionSignature(ins, signature)) continue;
                    signature.isStaticMethod = ins.metadata.isStaticMethod;
                    functionsByChunk[static_cast<int64_t>(chunkIndex)] = signature;
                    functions[signature.name] = std::move(signature);
                }
            }

            for (const auto& chunk : program) {
                for (const auto& ins : chunk) {
                    if (ins.op != VMOperator::STORE_VALUE_GLOBAL) continue;
                    int64_t globalIndex = -1;
                    std::string globalName;
                    if (!readIntegerArg(ins, 0, globalIndex) ||
                        !readStringArg(ins, 2, globalName) ||
                        classesWithLayouts.find(globalName) ==
                            classesWithLayouts.end()) {
                        continue;
                    }
                    classGlobalTypes[globalIndex] = std::move(globalName);
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
                        target->second.parameterTypes,
                        target->second.parameterNames,
                        target->second.parameterIsReference
                    );
                    if (pendingTargetIsStatic) ins.metadata = pendingTarget;
                    hasPendingTarget = true;
                    continue;
                }

                if (hasPendingTarget && ins.op == VMOperator::CALL_CLOSURE) {
                    const auto callArguments = ins.metadata.callArguments;
                    const auto explicitTypeArguments =
                        ins.metadata.explicitTypeArguments;
                    const bool genericVmDispatch =
                        ins.metadata.tag == "generic_vm_specialization";
                    ins.metadata = pendingTarget;
                    if (genericVmDispatch) {
                        ins.metadata.tag = "generic_vm_specialization";
                    }
                    ins.metadata.callArguments = callArguments;
                    ins.metadata.explicitTypeArguments = explicitTypeArguments;
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

        void annotateStaticAliasCalls(
            std::vector<IRType::IRInstr>& chunk,
            std::unordered_map<int64_t, FunctionValue>& globalValues,
            const std::unordered_set<int64_t>& chunkWrites
        ) const {
            std::vector<std::unordered_set<size_t>> predecessors(chunk.size());
            auto addPredecessor = [&](size_t source, long long target) {
                if (target >= 0 &&
                    static_cast<unsigned long long>(target) < chunk.size()) {
                    predecessors[static_cast<size_t>(target)].insert(source);
                }
            };
            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                const auto& instruction = chunk[pc];
                int64_t offset = 0;
                switch (instruction.op) {
                    case VMOperator::JUMP:
                    case VMOperator::LOOP_BREAK:
                        if (readIntegerArg(instruction, 0, offset)) {
                            addPredecessor(
                                pc, static_cast<long long>(pc) + 1 + offset);
                        }
                        break;

                    case VMOperator::JUMP_IF_TRUE:
                    case VMOperator::JUMP_IF_FALSE:
                        if (readIntegerArg(instruction, 0, offset)) {
                            addPredecessor(
                                pc, static_cast<long long>(pc) + 1 + offset);
                        }
                        addPredecessor(pc, static_cast<long long>(pc) + 1);
                        break;

                    case VMOperator::JUMP_IF_PARAM_PROVIDED:
                        if (readIntegerArg(instruction, 1, offset)) {
                            addPredecessor(
                                pc, static_cast<long long>(pc) + 1 + offset);
                        }
                        addPredecessor(pc, static_cast<long long>(pc) + 1);
                        break;

                    case VMOperator::TRY_BEGIN:
                        if (readIntegerArg(instruction, 0, offset)) {
                            addPredecessor(
                                pc, static_cast<long long>(pc) + offset);
                        }
                        addPredecessor(pc, static_cast<long long>(pc) + 1);
                        break;

                    case VMOperator::RETURN:
                    case VMOperator::THROW:
                    case VMOperator::FUNC_END:
                        break;

                    default:
                        addPredecessor(pc, static_cast<long long>(pc) + 1);
                        break;
                }
            }

            std::vector<FunctionValue> stack;
            std::unordered_map<int64_t, FunctionValue> localValues;

            auto pop = [&]() {
                if (stack.empty()) return FunctionValue{};
                FunctionValue value = std::move(stack.back());
                stack.pop_back();
                return value;
            };
            auto invalidateGlobalWrites = [&] (
                const std::unordered_set<int64_t>& writes
            ) {
                for (const auto index : writes) globalValues.erase(index);
            };
            auto staticFunction = [&](const std::string& name) {
                const auto found = functions.find(name);
                if (found == functions.end() ||
                    !isStaticFunctionValue(found->second)) {
                    return FunctionValue{};
                }
                return FunctionValue{found->second.name};
            };
            auto pushLoad = [&](const IRType::IRInstr& ins) {
                FunctionValue value;
                if (ins.args.size() == 1 &&
                    std::holds_alternative<std::string>(ins.args.front())) {
                    value = staticFunction(std::get<std::string>(ins.args.front()));
                } else if (ins.args.size() >= 2 &&
                           std::holds_alternative<std::string>(ins.args[0])) {
                    const std::string& scope = std::get<std::string>(ins.args[0]);
                    int64_t index = -1;
                    if (readIntegerArg(ins, 1, index)) {
                        if (scope == "global") {
                            const auto found = globalValues.find(index);
                            if (found != globalValues.end()) value = found->second;
                        } else if (scope == "local") {
                            const auto found = localValues.find(index);
                            if (found != localValues.end()) value = found->second;
                        }
                    }
                    if (value.name.empty() && value.className.empty() &&
                        ins.args.size() >= 3 &&
                        std::holds_alternative<std::string>(ins.args[2])) {
                        value = staticFunction(std::get<std::string>(ins.args[2]));
                    }
                }
                stack.push_back(std::move(value));
            };

            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                auto& ins = chunk[pc];
                if (predecessors[pc].size() > 1) {
                    // The linear pass cannot choose one branch's alias at a
                    // control-flow merge. Drop aliases changed by this chunk;
                    // equal aliases can be recovered by a future CFG dataflow
                    // pass without risking a wrong static call today.
                    stack.clear();
                    localValues.clear();
                    invalidateGlobalWrites(chunkWrites);
                }

                switch (ins.op) {
                    case VMOperator::FUNC_BEGIN:
                    case VMOperator::FUNC_END:
                    case VMOperator::IR_ANNOTATION:
                    case VMOperator::HOTSPOT:
                    case VMOperator::PUSH_NAMED:
                        break;

                    case VMOperator::LOAD_VALUE:
                        pushLoad(ins);
                        break;

                    case VMOperator::MAKE_FUNCTION: {
                        int64_t functionChunk = -1;
                        FunctionValue value;
                        if (readIntegerArg(ins, 0, functionChunk)) {
                            const auto found = functionsByChunk.find(functionChunk);
                            if (found != functionsByChunk.end() &&
                                isStaticFunctionValue(found->second)) {
                                value.name = found->second.name;
                            }
                        }
                        stack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::LOAD_VALUE_SHARED:
                    case VMOperator::LOAD_NATIVE:
                        stack.emplace_back();
                        break;

                    case VMOperator::PUSH:
                        stack.emplace_back();
                        break;

                    case VMOperator::COPY:
                        stack.push_back(stack.empty() ? FunctionValue{} : stack.back());
                        break;

                    case VMOperator::SWAP:
                        if (stack.size() >= 2) {
                            std::swap(stack.back(), stack[stack.size() - 2]);
                        } else {
                            stack.clear();
                            invalidateGlobalWrites(functionGlobalWrites);
                            localValues.clear();
                        }
                        break;

                    case VMOperator::STORE_VALUE_LOCAL:
                    case VMOperator::ASSIGN_VALUE_LOCAL: {
                        int64_t index = -1;
                        const FunctionValue value = pop();
                        if (readIntegerArg(ins, 0, index) &&
                            (!value.name.empty() || !value.className.empty())) {
                            localValues[index] = value;
                        } else if (readIntegerArg(ins, 0, index)) {
                            localValues.erase(index);
                        } else {
                            localValues.clear();
                        }
                        break;
                    }

                    case VMOperator::STORE_VALUE_GLOBAL:
                    case VMOperator::ASSIGN_VALUE_GLOBAL: {
                        int64_t index = -1;
                        const FunctionValue value = pop();
                        if (readIntegerArg(ins, 0, index) &&
                            ins.op == VMOperator::STORE_VALUE_GLOBAL &&
                            classGlobalTypes.find(index) != classGlobalTypes.end()) {
                            globalValues[index] = FunctionValue{
                                {}, classGlobalTypes.at(index)
                            };
                        } else if (readIntegerArg(ins, 0, index) &&
                                   (!value.name.empty() ||
                                    !value.className.empty())) {
                            globalValues[index] = value;
                        } else if (readIntegerArg(ins, 0, index)) {
                            globalValues.erase(index);
                        } else {
                            globalValues.clear();
                        }
                        break;
                    }

                    case VMOperator::POP:
                        (void)pop();
                        break;

                    case VMOperator::ADD:
                    case VMOperator::SUBTRACT:
                    case VMOperator::MULTIPLY:
                    case VMOperator::DIVIDE:
                    case VMOperator::MODULUS:
                    case VMOperator::EQUL:
                    case VMOperator::NOT_EQUAL:
                    case VMOperator::GREATER_THAN:
                    case VMOperator::LESS_THAN:
                    case VMOperator::GREATER_EQUAL:
                    case VMOperator::LESS_EQUAL:
                    case VMOperator::LOGICAL_AND:
                    case VMOperator::LOGICAL_OR:
                    case VMOperator::DICT_LOAD_VALUE:
                    case VMOperator::LIST_LOAD_ELEM:
                        (void)pop();
                        (void)pop();
                        stack.emplace_back();
                        break;

                    case VMOperator::LOGICAL_NOT:
                        (void)pop();
                        stack.emplace_back();
                        break;

                    case VMOperator::OBJ_LOAD_ATTR: {
                        std::string attribute;
                        const FunctionValue receiver = pop();
                        FunctionValue value;
                        if (!receiver.className.empty() &&
                            readStringArg(ins, 0, attribute)) {
                            value = staticClassMethod(
                                receiver.className, attribute);
                        }
                        stack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::CALL_CLOSURE: {
                        int64_t argc = -1;
                        if (!readIntegerArg(ins, 0, argc) || argc < 0 ||
                            static_cast<uint64_t>(argc) >= stack.size()) {
                            stack.clear();
                            invalidateGlobalWrites(functionGlobalWrites);
                            localValues.clear();
                            break;
                        }
                        const size_t calleeIndex = stack.size() -
                            static_cast<size_t>(argc) - 1;
                        const FunctionValue callee = stack[calleeIndex];
                        if (ins.metadata.kind != IRType::IRMetadataKind::CallTarget &&
                            !callee.name.empty()) {
                            const auto function = functions.find(callee.name);
                            if (function != functions.end() &&
                                isStaticFunctionValue(function->second)) {
                                const auto callArguments = ins.metadata.callArguments;
                                const auto explicitTypeArguments =
                                    ins.metadata.explicitTypeArguments;
                                ins.metadata = IRType::IRMetadata::callTarget(
                                    function->second.name,
                                    function->second.returnType,
                                    function->second.captureCount,
                                    function->second.parameterTypes,
                                    function->second.parameterNames,
                                    function->second.parameterIsReference);
                                ins.metadata.tag = "static_function_alias";
                                ins.metadata.callArguments = callArguments;
                                ins.metadata.explicitTypeArguments =
                                    explicitTypeArguments;
                            }
                        }
                        stack.resize(calleeIndex);
                        stack.emplace_back();
                        invalidateGlobalWrites(functionGlobalWrites);
                        localValues.clear();
                        break;
                    }

                    case VMOperator::OBJ_CALL_METHOD:
                    case VMOperator::CALL_NATIVE: {
                        int64_t argc = -1;
                        const size_t countArg =
                            ins.op == VMOperator::OBJ_CALL_METHOD ? 1 : 0;
                        if (!readIntegerArg(ins, countArg, argc) || argc < 0 ||
                            static_cast<uint64_t>(argc) + 1 > stack.size()) {
                            stack.clear();
                        } else {
                            const size_t count = static_cast<size_t>(argc) + 1;
                            stack.resize(stack.size() - count);
                            stack.emplace_back();
                        }
                        invalidateGlobalWrites(functionGlobalWrites);
                        localValues.clear();
                        break;
                    }

                    case VMOperator::JUMP:
                    case VMOperator::JUMP_IF_TRUE:
                    case VMOperator::JUMP_IF_FALSE:
                    case VMOperator::LOOP_BREAK:
                    case VMOperator::TRY_BEGIN:
                    case VMOperator::TRY_END:
                    case VMOperator::CATCH_BEGIN:
                    case VMOperator::CATCH_END:
                    case VMOperator::RETURN:
                        stack.clear();
                        invalidateGlobalWrites(chunkWrites);
                        localValues.clear();
                        break;

                    default:
                        stack.clear();
                        globalValues.clear();
                        localValues.clear();
                        break;
                }
            }
        }

    public:
        void annotate(
            std::vector<std::vector<IRType::IRInstr>>& program
        ) {
            collectFunctions(program);
            std::unordered_map<int64_t, FunctionValue> globalValues;
            for (size_t chunkIndex = 0; chunkIndex < program.size(); ++chunkIndex) {
                auto& chunk = program[chunkIndex];
                annotateChunk(chunk);
                const bool isFunctionBody = std::any_of(
                    chunk.begin(), chunk.end(), [](const IRType::IRInstr& ins) {
                        return ins.op == VMOperator::FUNC_BEGIN;
                    });
                if (isFunctionBody) {
                    auto functionGlobalValues = globalValues;
                    annotateStaticAliasCalls(
                        chunk, functionGlobalValues, chunkGlobalWrites[chunkIndex]);
                } else {
                    annotateStaticAliasCalls(
                        chunk, globalValues, chunkGlobalWrites[chunkIndex]);
                }
            }
        }
    };

    inline void annotate(
        std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        Annotator annotator;
        annotator.annotate(program);
    }

}
