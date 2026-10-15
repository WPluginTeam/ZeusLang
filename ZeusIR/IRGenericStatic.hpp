#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "IRType.hpp"
#include "IRStaticOperator.hpp"
#include "../ZeusBuiltins/BuiltinRegistry.hpp"
#include "../ZeusTyping/GenericTypeName.hpp"

namespace IRGenericStatic {

    struct FunctionInfo {
        std::string name;
        std::string returnType;
        std::vector<std::string> parameterTypes;
        std::vector<std::string> parameterNames;
        std::vector<bool> parameterIsReference;
        std::vector<std::string> typeParameters;
        std::string ownerType;
        std::string operatorSymbol;
        int captureCount = 0;
        size_t chunkIndex = 0;
        bool isStaticMethod = false;
        bool isInline = false;
        bool isNoSafepoint = false;
    };

    inline bool readStringArg(
        const IRType::IRInstr& instruction,
        size_t index,
        std::string& value
    ) {
        if (index >= instruction.args.size() ||
            !std::holds_alternative<std::string>(instruction.args[index])) {
            return false;
        }
        value = std::get<std::string>(instruction.args[index]);
        return true;
    }

    inline bool readIntegerArg(
        const IRType::IRInstr& instruction,
        size_t index,
        int64_t& value
    ) {
        if (index >= instruction.args.size() ||
            !std::holds_alternative<LexerType::Value>(instruction.args[index])) {
            return false;
        }
        const auto& literal = std::get<LexerType::Value>(instruction.args[index]);
        const auto* integer = std::get_if<ZeusType::BigInteger>(&literal.data);
        return integer && integer->tryToInt64(value);
    }

    inline bool readFunctionSignature(
        const IRType::IRInstr& instruction,
        FunctionInfo& function
    ) {
        if (instruction.metadata.kind == IRType::IRMetadataKind::FunctionSignature) {
            return instruction.metadata.readFunctionSignature(
                function.name,
                function.returnType,
                function.captureCount,
                function.parameterTypes,
                &function.ownerType,
                &function.operatorSymbol,
                nullptr,
                &function.parameterNames,
                &function.parameterIsReference
            );
        }

        std::string tag;
        int64_t captures = 0;
        int64_t parameterCount = 0;
        if (!readStringArg(instruction, 0, tag) || tag != "function_signature" ||
            !readStringArg(instruction, 1, function.name) ||
            !readStringArg(instruction, 2, function.returnType) ||
            !readIntegerArg(instruction, 3, captures) ||
            !readIntegerArg(instruction, 4, parameterCount) ||
            captures < 0 || parameterCount < 0 ||
            instruction.args.size() < 5 + static_cast<size_t>(parameterCount)) {
            return false;
        }
        function.captureCount = static_cast<int>(captures);
        function.parameterTypes.clear();
        function.parameterTypes.reserve(static_cast<size_t>(parameterCount));
        for (int64_t i = 0; i < parameterCount; ++i) {
            std::string type;
            if (!readStringArg(instruction, 5 + static_cast<size_t>(i), type)) return false;
            function.parameterTypes.push_back(std::move(type));
        }
        return true;
    }

    inline bool readTypeParameters(
        const IRType::IRInstr& instruction,
        std::vector<std::string>& parameters
    ) {
        std::string marker;
        if (instruction.op != VMOperator::IR_ANNOTATION ||
            !readStringArg(instruction, 0, marker) ||
            marker != "generic_type_parameters") {
            return false;
        }
        parameters.clear();
        for (size_t i = 1; i < instruction.args.size(); ++i) {
            std::string parameter;
            if (!readStringArg(instruction, i, parameter) || parameter.empty()) {
                parameters.clear();
                return false;
            }
            parameters.push_back(std::move(parameter));
        }
        return !parameters.empty();
    }

    inline bool readDirectCallAnnotation(
        const IRType::IRInstr& instruction,
        std::string& name
    ) {
        std::string marker;
        return instruction.op == VMOperator::IR_ANNOTATION &&
            readStringArg(instruction, 0, marker) && marker == "direct_call" &&
            readStringArg(instruction, 1, name);
    }

    inline std::string substituteType(
        const std::string& type,
        const std::map<std::string, std::string>& substitutions
    ) {
        const bool isReference = !type.empty() && type.front() == '&';
        const std::string baseType = isReference ? type.substr(1) : type;
        auto found = substitutions.find(baseType);
        if (found != substitutions.end()) {
            return (isReference ? "&" : "") + found->second;
        }

        constexpr const char* runtimePrefix = "__zeus_typevar:";
        if (baseType.rfind(runtimePrefix, 0) == 0) {
            const std::string parameter = baseType.substr(
                std::char_traits<char>::length(runtimePrefix));
            found = substitutions.find(parameter);
            if (found != substitutions.end()) {
                return (isReference ? "&" : "") + found->second;
            }
        }

        std::string genericBase;
        std::vector<std::string> genericArguments;
        if (!ZeusTypeInfer::decodeGenericSpecializationName(
                baseType, genericBase, genericArguments)) {
            return type;
        }
        bool changed = false;
        for (auto& argument : genericArguments) {
            const std::string substituted = substituteType(argument, substitutions);
            changed = changed || substituted != argument;
            argument = substituted;
        }
        if (!changed) return type;
        const std::string specialized = ZeusTypeInfer::genericSpecializationName(
            genericBase, genericArguments);
        return (isReference ? "&" : "") + specialized;
    }

    inline bool containsTypeParameter(
        const std::string& type,
        const std::vector<std::string>& parameters
    ) {
        const std::string baseType = !type.empty() && type.front() == '&'
            ? type.substr(1) : type;
        if (std::find(parameters.begin(), parameters.end(), baseType) !=
            parameters.end()) {
            return true;
        }
        std::string genericBase;
        std::vector<std::string> genericArguments;
        if (!ZeusTypeInfer::decodeGenericSpecializationName(
                baseType, genericBase, genericArguments)) {
            return false;
        }
        return std::any_of(genericArguments.begin(), genericArguments.end(),
            [&](const std::string& argument) {
                return containsTypeParameter(argument, parameters);
            });
    }

    inline void collectTypeParameterCandidates(
        const std::string& formalType,
        const std::string& actualType,
        const std::string& parameter,
        std::vector<std::string>& candidates
    ) {
        const std::string formalBase = !formalType.empty() && formalType.front() == '&'
            ? formalType.substr(1) : formalType;
        const std::string actualBase = !actualType.empty() && actualType.front() == '&'
            ? actualType.substr(1) : actualType;
        if (formalBase == parameter) {
            candidates.push_back(actualBase);
            return;
        }

        std::string formalGenericBase;
        std::string actualGenericBase;
        std::vector<std::string> formalArguments;
        std::vector<std::string> actualArguments;
        if (!ZeusTypeInfer::decodeGenericSpecializationName(
                formalBase, formalGenericBase, formalArguments) ||
            !ZeusTypeInfer::decodeGenericSpecializationName(
                actualBase, actualGenericBase, actualArguments) ||
            formalGenericBase != actualGenericBase ||
            formalArguments.size() != actualArguments.size()) {
            return;
        }

        for (size_t i = 0; i < formalArguments.size(); ++i) {
            collectTypeParameterCandidates(
                formalArguments[i], actualArguments[i], parameter, candidates);
        }
    }

    inline std::string encodeSpecializationType(const std::string& type) {
        static const char digits[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(type.size() * 2);
        for (unsigned char byte : type) {
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0f]);
        }
        return encoded;
    }

    inline std::string specializationKey(
        const std::string& functionName,
        const std::string& concreteType
    ) {
        return functionName + "\n" + concreteType;
    }

    inline std::string specializationKey(
        const std::string& functionName,
        const std::vector<std::string>& concreteTypes
    ) {
        std::string key = functionName;
        for (const auto& type : concreteTypes) {
            key += "\n" + std::to_string(type.size()) + ":" + type;
        }
        return key;
    }

    inline std::string specializationName(
        const std::string& functionName,
        const std::string& concreteType
    ) {
        return functionName + "$__zeus_type_" + encodeSpecializationType(concreteType);
    }

    inline std::string specializationName(
        const std::string& functionName,
        const std::vector<std::string>& concreteTypes
    ) {
        if (concreteTypes.size() == 1) {
            return specializationName(functionName, concreteTypes.front());
        }
        std::string name = functionName + "$__zeus_types_";
        for (size_t i = 0; i < concreteTypes.size(); ++i) {
            if (i != 0) name += "_";
            name += std::to_string(concreteTypes[i].size()) + "x" +
                encodeSpecializationType(concreteTypes[i]);
        }
        return name;
    }

    inline void writeSignature(
        IRType::IRInstr& instruction,
        const FunctionInfo& function
    ) {
        instruction.metadata = IRType::IRMetadata::functionSignature(
            function.name,
            function.returnType,
            function.captureCount,
            function.parameterTypes,
            function.ownerType,
            function.operatorSymbol,
            function.isStaticMethod,
            function.parameterNames,
            function.parameterIsReference,
            function.isInline,
            function.isNoSafepoint
        );

        if (instruction.args.size() < 5 + function.parameterTypes.size()) return;
        instruction.args[1] = function.name;
        instruction.args[2] = function.returnType;
        for (size_t i = 0; i < function.parameterTypes.size(); ++i) {
            instruction.args[5 + i] = function.parameterTypes[i];
        }
    }

    inline void substituteFunctionBody(
        std::vector<IRType::IRInstr>& chunk,
        const std::map<std::string, std::string>& substitutions
    ) {
        chunk.erase(std::remove_if(chunk.begin(), chunk.end(),
            [](const IRType::IRInstr& instruction) {
                std::vector<std::string> parameters;
                return readTypeParameters(instruction, parameters);
            }), chunk.end());

        for (auto& instruction : chunk) {
            instruction.metadata.typeName = substituteType(
                instruction.metadata.typeName, substitutions);
            for (auto& type : instruction.metadata.typeNames) {
                type = substituteType(type, substitutions);
            }
            for (auto& type : instruction.metadata.explicitTypeArguments) {
                type = substituteType(type, substitutions);
            }

            if (instruction.metadata.symbol.kind == IRType::SymbolRefKind::Class) {
                const std::string originalName = instruction.metadata.symbol.name;
                const auto replacement = substitutions.find(originalName);
                if (replacement != substitutions.end()) {
                    instruction.metadata.symbol.name = replacement->second;
                    if (instruction.op == VMOperator::LOAD_VALUE &&
                        !instruction.args.empty()) {
                        if (auto* rawName =
                                std::get_if<std::string>(&instruction.args.front())) {
                            if (*rawName == originalName) *rawName = replacement->second;
                        } else if (auto* literal =
                                       std::get_if<LexerType::Value>(
                                           &instruction.args.front())) {
                            if (auto* className =
                                    std::get_if<std::string>(&literal->data)) {
                                if (*className == originalName) {
                                    *className = replacement->second;
                                }
                            }
                        }
                    }
                }
            }

            if ((instruction.op == VMOperator::LLVM_FADDR ||
                 instruction.op == VMOperator::LLVM_FSTORE) &&
                !instruction.args.empty() &&
                std::holds_alternative<std::string>(instruction.args[0])) {
                instruction.args[0] = substituteType(
                    std::get<std::string>(instruction.args[0]), substitutions);
            }

            if (instruction.op == VMOperator::LLVM_FADDR) {
                instruction.args.erase(std::remove_if(
                    instruction.args.begin() +
                        std::min<size_t>(1, instruction.args.size()),
                    instruction.args.end(),
                    [](const IRType::IRArgs& argument) {
                        if (!std::holds_alternative<std::string>(argument)) return false;
                        return std::get<std::string>(argument).rfind(
                            "__zeus_typevar_source:", 0) == 0;
                    }), instruction.args.end());
            }
        }
    }

    class Specializer {
        struct SpecializationEntry {
            size_t chunkIndex = 0;
            std::string functionName;
        };

        std::map<std::string, FunctionInfo> templates;
        std::map<std::string, SpecializationEntry> specializations;
        std::set<std::string> names;
        std::map<size_t, std::map<std::string, std::string>> substitutionsByChunk;
        std::string error;
        int errorLine = 0;

        bool fail(const IRType::IRInstr& instruction, std::string message) {
            error = std::move(message);
            errorLine = instruction.line;
            return false;
        }

        bool readCallName(const IRType::IRInstr& instruction, std::string& name) const {
            return (instruction.op == VMOperator::CALL_CLOSURE ||
                    instruction.op == VMOperator::OBJ_CALL_METHOD) &&
                instruction.metadata.readDirectCall(name) &&
                instruction.metadata.kind == IRType::IRMetadataKind::CallTarget;
        }

        bool findActualArgumentForFormal(
            const IRType::IRInstr& call,
            const FunctionInfo& function,
            size_t formalIndex,
            size_t& actualIndex,
            bool& supplied
        ) const {
            const size_t receiverOffset =
                call.op == VMOperator::OBJ_CALL_METHOD &&
                !function.ownerType.empty() && !function.isStaticMethod ? 1 : 0;
            const bool hasCompleteArgumentMap =
                call.metadata.callArguments.size() + receiverOffset ==
                call.metadata.callArgumentTypes.size();
            actualIndex = formalIndex;
            supplied = false;

            if (!hasCompleteArgumentMap) {
                supplied = actualIndex < call.metadata.callArgumentTypes.size();
                return true;
            }

            size_t positional = receiverOffset;
            for (size_t i = 0; i < call.metadata.callArguments.size(); ++i) {
                const auto& source = call.metadata.callArguments[i];
                size_t mappedFormal = function.parameterTypes.size();
                if (!source.namedParameter.empty()) {
                    const auto named = std::find(
                        function.parameterNames.begin(),
                        function.parameterNames.end(), source.namedParameter);
                    if (named != function.parameterNames.end()) {
                        mappedFormal = static_cast<size_t>(
                            named - function.parameterNames.begin());
                    }
                } else {
                    mappedFormal = positional++;
                }
                if (mappedFormal == formalIndex) {
                    actualIndex = i + receiverOffset;
                    supplied = true;
                    return true;
                }
            }
            return true;
        }

        bool inferTypeArguments(
            const IRType::IRInstr& call,
            const FunctionInfo& function,
            const std::map<std::string, std::string>& callerSubstitutions,
            std::map<std::string, std::string>& concreteTypes
        ) {
            concreteTypes.clear();
            for (const auto& parameter : function.typeParameters) {
                std::vector<std::string> candidates;
                for (size_t formalIndex = 0;
                     formalIndex < function.parameterTypes.size(); ++formalIndex) {
                    std::string formalType = function.parameterTypes[formalIndex];
                    if (!containsTypeParameter(formalType, { parameter })) continue;

                    size_t actualIndex = formalIndex;
                    bool supplied = false;
                    findActualArgumentForFormal(
                        call, function, formalIndex, actualIndex, supplied);
                    if (!supplied || actualIndex >= call.metadata.callArgumentTypes.size()) {
                        continue;
                    }
                    const std::string actualType = substituteType(
                        call.metadata.callArgumentTypes[actualIndex], callerSubstitutions);
                    if (!actualType.empty()) {
                        collectTypeParameterCandidates(
                            formalType, actualType, parameter, candidates);
                    }
                }

                if (candidates.empty()) return false;
                const std::string concreteType = candidates.front();
                if (std::any_of(candidates.begin(), candidates.end(),
                        [&](const std::string& candidate) {
                            return canonicalTypeName(candidate) !=
                                canonicalTypeName(concreteType);
                        })) {
                    return false;
                }
                if (concreteType.empty() || concreteType == "unknown" ||
                    concreteType == "type parameter" || concreteType == parameter ||
                    concreteType.rfind("__zeus_typevar:", 0) == 0) {
                    return false;
                }
                concreteTypes[parameter] = concreteType;
            }
            return concreteTypes.size() == function.typeParameters.size();
        }

        bool readExplicitTypeArguments(
            const IRType::IRInstr& call,
            const FunctionInfo& function,
            const std::map<std::string, std::string>& callerSubstitutions,
            std::map<std::string, std::string>& concreteTypes
        ) {
            if (call.metadata.explicitTypeArguments.size() !=
                function.typeParameters.size()) {
                return fail(call, "generic call has the wrong number of explicit type arguments");
            }
            concreteTypes.clear();
            for (size_t i = 0; i < function.typeParameters.size(); ++i) {
                const std::string concreteType = substituteType(
                    call.metadata.explicitTypeArguments[i], callerSubstitutions);
                if (concreteType.empty() || concreteType == "unknown" ||
                    concreteType == "type parameter" ||
                    concreteType == function.typeParameters[i] ||
                    concreteType.rfind("__zeus_typevar:", 0) == 0) {
                    return fail(call, "explicit generic type argument is not concrete");
                }
                concreteTypes[function.typeParameters[i]] = concreteType;
            }
            return true;
        }

        static std::string canonicalTypeName(const std::string& typeName) {
            const auto* implementation = ZeusBuiltins::findTypeImplementation(typeName);
            return implementation == nullptr
                ? typeName : std::string(implementation->canonicalName);
        }

        bool hasSuppliedTypeParameterArgument(
            const IRType::IRInstr& call,
            const FunctionInfo& function,
            const std::string& parameter
        ) const {
            for (size_t formalIndex = 0;
                 formalIndex < function.parameterTypes.size(); ++formalIndex) {
                std::string formalType = function.parameterTypes[formalIndex];
                if (!formalType.empty() && formalType.front() == '&') {
                    formalType.erase(formalType.begin());
                }
                if (formalType != parameter) continue;
                size_t actualIndex = formalIndex;
                bool supplied = false;
                findActualArgumentForFormal(
                    call, function, formalIndex, actualIndex, supplied);
                if (supplied) return true;
            }
            return false;
        }

        bool validateExplicitTypeAgainstArguments(
            const IRType::IRInstr& call,
            const FunctionInfo& function,
            const std::map<std::string, std::string>& callerSubstitutions,
            const std::map<std::string, std::string>& concreteTypes,
            bool& argumentTypesPending
        ) {
            argumentTypesPending = false;
            for (const auto& typeParameter : function.typeParameters) {
                if (concreteTypes.find(typeParameter) == concreteTypes.end()) return false;
            }

            for (size_t formalIndex = 0;
                 formalIndex < function.parameterTypes.size(); ++formalIndex) {
                const std::string& formalType = function.parameterTypes[formalIndex];
                if (!containsTypeParameter(formalType, function.typeParameters)) continue;

                size_t actualIndex = formalIndex;
                bool supplied = false;
                findActualArgumentForFormal(
                    call, function, formalIndex, actualIndex, supplied);
                if (!supplied) continue;
                if (actualIndex >= call.metadata.callArgumentTypes.size()) {
                    argumentTypesPending = true;
                    continue;
                }

                std::string actualType = substituteType(
                    call.metadata.callArgumentTypes[actualIndex], callerSubstitutions);
                if (actualType.empty() || actualType == "unknown" ||
                    actualType == "type parameter" ||
                    actualType.rfind("__zeus_typevar:", 0) == 0) {
                    argumentTypesPending = true;
                    continue;
                }

                std::string specializedFormal = substituteType(formalType, concreteTypes);
                if (!specializedFormal.empty() && specializedFormal.front() == '&') {
                    specializedFormal.erase(specializedFormal.begin());
                }
                if (!actualType.empty() && actualType.front() == '&') {
                    actualType.erase(actualType.begin());
                }
                if (canonicalTypeName(actualType) !=
                    canonicalTypeName(specializedFormal)) {
                    return fail(call,
                        "explicit generic type argument does not match the argument type");
                }
            }
            return true;
        }

        bool createSpecialization(
            std::vector<std::vector<IRType::IRInstr>>& program,
            const FunctionInfo& function,
            const std::map<std::string, std::string>& concreteTypes,
            size_t& chunkIndex,
            std::string& specializedName
        ) {
            std::vector<std::string> concreteTypeList;
            concreteTypeList.reserve(function.typeParameters.size());
            for (const auto& parameter : function.typeParameters) {
                const auto resolved = concreteTypes.find(parameter);
                if (resolved == concreteTypes.end()) return false;
                concreteTypeList.push_back(resolved->second);
            }
            const std::string key = specializationKey(function.name, concreteTypeList);
            const auto existing = specializations.find(key);
            if (existing != specializations.end()) {
                chunkIndex = existing->second.chunkIndex;
                specializedName = existing->second.functionName;
                return true;
            }
            if (function.chunkIndex >= program.size()) return false;
            if (specializations.size() >= 2048) {
                error = "泛型静态函数专门化数量超过 2048 个；请检查递归类型推导";
                return false;
            }

            FunctionInfo specialized = function;
            specialized.name = specializationName(function.name, concreteTypeList);
            size_t suffix = 2;
            const std::string baseName = specialized.name;
            while (names.count(specialized.name) != 0) {
                specialized.name = baseName + "_" + std::to_string(suffix++);
            }
            names.insert(specialized.name);
            std::map<std::string, std::string> mapping = concreteTypes;
            specialized.returnType = substituteType(function.returnType, mapping);
            for (auto& type : specialized.parameterTypes) {
                type = substituteType(type, mapping);
            }

            auto clone = program[function.chunkIndex];
            substituteFunctionBody(clone, mapping);
            for (auto& instruction : clone) {
                if (instruction.op != VMOperator::IR_ANNOTATION) continue;
                FunctionInfo signature;
                if (!readFunctionSignature(instruction, signature) ||
                    signature.name != function.name) {
                    continue;
                }
                signature.name = specialized.name;
                signature.returnType = specialized.returnType;
                signature.parameterTypes = specialized.parameterTypes;
                signature.captureCount = specialized.captureCount;
                signature.ownerType = specialized.ownerType;
                signature.operatorSymbol = specialized.operatorSymbol;
                signature.parameterNames = specialized.parameterNames;
                signature.parameterIsReference = specialized.parameterIsReference;
                signature.isStaticMethod = specialized.isStaticMethod;
                signature.isInline = specialized.isInline;
                signature.isNoSafepoint = specialized.isNoSafepoint;
                writeSignature(instruction, signature);
                break;
            }

            IRType::IRInstr specializationMarker;
            specializationMarker.op = VMOperator::IR_ANNOTATION;
            specializationMarker.line = clone.empty() ? 0 : clone.front().line;
            specializationMarker.args = {
                std::string("generic_static_specialization"),
                function.name
            };
            for (const auto& parameter : function.typeParameters) {
                const auto resolved = concreteTypes.find(parameter);
                specializationMarker.args.push_back(parameter);
                specializationMarker.args.push_back(resolved->second);
            }
            const size_t insertion = std::min<size_t>(2, clone.size());
            clone.insert(clone.begin() + static_cast<std::ptrdiff_t>(insertion),
                         std::move(specializationMarker));

            chunkIndex = program.size();
            specializedName = specialized.name;
            program.push_back(std::move(clone));
            substitutionsByChunk[chunkIndex] = std::move(mapping);
            specializations.emplace(
                key, SpecializationEntry{ chunkIndex, specializedName });
            return true;
        }

        bool retargetCall(
            std::vector<IRType::IRInstr>& chunk,
            size_t callIndex,
            const std::string& oldName,
            const FunctionInfo& target,
            const std::map<std::string, std::string>& mapping,
            const std::string& specializedName,
            bool vmDispatch
        ) {
            auto& call = chunk[callIndex];
            int captures = target.captureCount;
            std::vector<std::string> parameterTypes = target.parameterTypes;
            for (auto& type : parameterTypes) type = substituteType(type, mapping);
            const std::string returnType = substituteType(target.returnType, mapping);
            IRType::IRMetadata rewritten = IRType::IRMetadata::callTarget(
                specializedName,
                returnType,
                captures,
                std::move(parameterTypes),
                target.parameterNames,
                target.parameterIsReference
            );
            rewritten.callArguments = call.metadata.callArguments;
            rewritten.explicitTypeArguments.clear();
            if (vmDispatch) {
                rewritten.tag = "generic_vm_specialization";
            } else if (call.op == VMOperator::OBJ_CALL_METHOD) {
                rewritten.tag = "statically_resolved_method";
            }
            rewritten.callArgumentTypes = call.metadata.callArgumentTypes;
            for (auto& argumentType : rewritten.callArgumentTypes) {
                argumentType = substituteType(argumentType, mapping);
            }
            call.metadata = std::move(rewritten);

            if (callIndex > 0) {
                std::string annotatedName;
                auto& annotation = chunk[callIndex - 1];
                if (readDirectCallAnnotation(annotation, annotatedName) &&
                    annotatedName == oldName && annotation.args.size() > 1) {
                    annotation.args[1] = specializedName;
                    annotation.metadata = IRType::IRMetadata::directCall(specializedName);
                }
            }
            return true;
        }

        bool prepareRuntimeTemplate(
            std::vector<IRType::IRInstr>& chunk,
            const FunctionInfo& function
        ) {
            std::map<std::string, int> sourceParameters;
            for (const auto& parameter : function.typeParameters) {
                for (size_t i = 0; i < function.parameterTypes.size(); ++i) {
                    std::string type = function.parameterTypes[i];
                    if (!type.empty() && type.front() == '&') type.erase(type.begin());
                    if (type == parameter) {
                        sourceParameters[parameter] = static_cast<int>(i);
                        break;
                    }
                }
            }

            for (auto& instruction : chunk) {
                if ((instruction.op != VMOperator::LLVM_FADDR &&
                     instruction.op != VMOperator::LLVM_FSTORE) ||
                    instruction.args.empty() ||
                    !std::holds_alternative<std::string>(instruction.args[0])) {
                    continue;
                }
                const std::string targetType = std::get<std::string>(instruction.args[0]);
                const auto source = sourceParameters.find(targetType);
                if (source != sourceParameters.end()) {
                    if (instruction.op == VMOperator::LLVM_FADDR) {
                        if (instruction.args.size() > 1) {
                            return fail(instruction,
                                "VM 泛型 @faddr -> T 需要一个 T 类型形参，且暂不支持长度参数");
                        }
                        instruction.args[0] = "__zeus_typevar:" + targetType;
                        instruction.args.push_back(
                            "__zeus_typevar_source:" + std::to_string(source->second));
                    } else {
                        instruction.args[0] = "__zeus_typevar:" + targetType;
                    }
                }
            }
            return true;
        }

    public:
        bool specialize(
            std::vector<std::vector<IRType::IRInstr>>& program,
            bool requireStaticCalls,
            std::string& outError,
            int& outErrorLine
        ) {
            templates.clear();
            specializations.clear();
            names.clear();
            substitutionsByChunk.clear();
            error.clear();
            errorLine = 0;
            struct DiagnosticOutput {
                const std::string& error;
                const int& line;
                std::string& output;
                int& outputLine;
                ~DiagnosticOutput() {
                    if (!error.empty()) {
                        output = error;
                        outputLine = line;
                    }
                }
            } diagnosticOutput{ error, errorLine, outError, outErrorLine };

            std::map<size_t, std::vector<std::string>> genericChunks;
            for (size_t ci = 0; ci < program.size(); ++ci) {
                for (const auto& instruction : program[ci]) {
                    FunctionInfo function;
                    if (instruction.op == VMOperator::IR_ANNOTATION &&
                        readFunctionSignature(instruction, function)) {
                        function.chunkIndex = ci;
                        function.isStaticMethod = instruction.metadata.isStaticMethod;
                        function.isInline = instruction.metadata.isInline;
                        function.isNoSafepoint = instruction.metadata.isNoSafepoint;
                        names.insert(function.name);
                        continue;
                    }
                    std::vector<std::string> parameters;
                    if (readTypeParameters(instruction, parameters)) {
                        genericChunks[ci] = std::move(parameters);
                    }
                }
            }

            for (const auto& entry : genericChunks) {
                std::set<std::string> uniqueParameters(
                    entry.second.begin(), entry.second.end());
                if (entry.second.empty() ||
                    uniqueParameters.size() != entry.second.size()) {
                    error = "@type parameters must be a non-empty list of distinct identifiers";
                    outError = error;
                    outErrorLine = 0;
                    return false;
                }
                FunctionInfo function;
                bool foundSignature = false;
                for (const auto& instruction : program[entry.first]) {
                    if (instruction.op == VMOperator::IR_ANNOTATION &&
                        readFunctionSignature(instruction, function)) {
                        foundSignature = true;
                        function.chunkIndex = entry.first;
                        function.isStaticMethod = instruction.metadata.isStaticMethod;
                        function.isInline = instruction.metadata.isInline;
                        function.isNoSafepoint = instruction.metadata.isNoSafepoint;
                        break;
                    }
                }
                if (!foundSignature || function.name.empty()) {
                    error = "@type 函数缺少有效的函数签名";
                    outError = error;
                    outErrorLine = program[entry.first].empty()
                        ? 0 : program[entry.first].front().line;
                    return false;
                }
                function.typeParameters = entry.second;
                if (function.captureCount != 0) {
                    error = "@type 泛型静态函数暂不支持闭包捕获";
                    outError = error;
                    outErrorLine = program[entry.first].front().line;
                    return false;
                }
                templates[function.name] = std::move(function);
            }

            // Existing serialized specializations are reused after package merging or .zir reads.
            for (size_t ci = 0; ci < program.size(); ++ci) {
                bool hasSpecializationMarker = false;
                std::string origin;
                std::vector<std::string> concreteTypes;
                std::string functionName;
                for (const auto& instruction : program[ci]) {
                    std::string marker;
                    if (instruction.op != VMOperator::IR_ANNOTATION) continue;

                    FunctionInfo signature;
                    if (readFunctionSignature(instruction, signature)) {
                        functionName = signature.name;
                    }
                    if (readStringArg(instruction, 0, marker) &&
                        marker == "generic_static_specialization" &&
                        readStringArg(instruction, 1, origin) &&
                        instruction.args.size() >= 4 &&
                        (instruction.args.size() - 2) % 2 == 0) {
                        concreteTypes.clear();
                        for (size_t argument = 3;
                             argument < instruction.args.size(); argument += 2) {
                            std::string concreteType;
                            if (!readStringArg(instruction, argument, concreteType)) {
                                concreteTypes.clear();
                                break;
                            }
                            concreteTypes.push_back(std::move(concreteType));
                        }
                        hasSpecializationMarker = !concreteTypes.empty();
                    }
                }
                if (hasSpecializationMarker) {
                    if (functionName.empty()) {
                        functionName = specializationName(origin, concreteTypes);
                    }
                    specializations[specializationKey(origin, concreteTypes)] =
                        SpecializationEntry{ ci, std::move(functionName) };
                }
            }

            {
                bool progress = false;
                bool pending = false;
                do {
                    progress = false;
                    pending = false;
                    for (size_t ci = 0; ci < program.size(); ++ci) {
                        const auto templateForChunk = std::find_if(
                            templates.begin(), templates.end(),
                            [&](const auto& entry) { return entry.second.chunkIndex == ci; });
                        if (templateForChunk != templates.end()) continue;

                        const auto substitutions = substitutionsByChunk.find(ci);
                        const std::map<std::string, std::string> callerMapping =
                            substitutions == substitutionsByChunk.end()
                                ? std::map<std::string, std::string>{}
                                : substitutions->second;

                        for (size_t ii = 0; ii < program[ci].size(); ++ii) {
                            std::string targetName;
                            if (!readCallName(program[ci][ii], targetName)) {
                                const auto& unresolved = program[ci][ii];
                                std::string methodName;
                                if (unresolved.op == VMOperator::OBJ_CALL_METHOD &&
                                    readStringArg(unresolved, 0, methodName)) {
                                    const bool genericMethod = std::any_of(
                                        templates.begin(), templates.end(),
                                        [&](const auto& entry) {
                                            const auto& function = entry.second;
                                            return !function.ownerType.empty() &&
                                                function.name == function.ownerType + "::" + methodName;
                                        });
                                    if (genericMethod &&
                                        (requireStaticCalls ||
                                         !unresolved.metadata.explicitTypeArguments.empty())) {
                                        pending = true;
                                        errorLine = unresolved.line;
                                    }
                                }
                                continue;
                            }
                            const auto target = templates.find(targetName);
                            if (target == templates.end()) continue;

                            const bool hasExplicitTypeArguments =
                                !program[ci][ii].metadata.explicitTypeArguments.empty();
                            if (!hasExplicitTypeArguments && !requireStaticCalls) continue;

                            std::map<std::string, std::string> concreteTypes;
                            const bool resolvedType = hasExplicitTypeArguments
                                ? readExplicitTypeArguments(
                                      program[ci][ii], target->second, callerMapping,
                                      concreteTypes)
                                : inferTypeArguments(
                                      program[ci][ii], target->second, callerMapping,
                                      concreteTypes);
                            if (!resolvedType) {
                                if (hasExplicitTypeArguments) return false;
                                pending = true;
                                errorLine = program[ci][ii].line;
                                continue;
                            }
                            if (hasExplicitTypeArguments) {
                                bool argumentTypesPending = false;
                                if (!validateExplicitTypeAgainstArguments(
                                        program[ci][ii], target->second,
                                        callerMapping, concreteTypes,
                                        argumentTypesPending)) {
                                    return false;
                                }
                                if (argumentTypesPending) {
                                    pending = true;
                                    errorLine = program[ci][ii].line;
                                    continue;
                                }
                            }

                            size_t specializedChunk = 0;
                            std::string specializedName;
                            if (!createSpecialization(
                                    program, target->second, concreteTypes,
                                    specializedChunk, specializedName)) {
                                if (error.empty()) {
                                    return fail(program[ci][ii],
                                        "创建泛型静态函数实例失败");
                                }
                                return false;
                            }

                            retargetCall(program[ci], ii, targetName, target->second,
                                         concreteTypes, specializedName,
                                         !requireStaticCalls);
                            progress = true;
                        }
                    }
                    if (progress) IRStaticOperator::annotate(program);
                } while (progress && pending);
                if (pending) {
                    error = "泛型静态调用的接收者或类型参数无法静态确定；请传入静态类型明确且一致的值";
                    return false;
                }
            }

            // Preserve dynamic VM execution for the unspecialized template by resolving T
            // from its matching parameter/value when an address intrinsic executes.
            for (const auto& entry : templates) {
                if (!prepareRuntimeTemplate(program[entry.second.chunkIndex], entry.second)) {
                    outError = error;
                    outErrorLine = errorLine;
                    return false;
                }
            }

            outError.clear();
            outErrorLine = 0;
            return true;
        }
    };

    inline bool specialize(
        std::vector<std::vector<IRType::IRInstr>>& program,
        bool requireStaticCalls,
        std::string& error,
        int& errorLine
    ) {
        Specializer specializer;
        return specializer.specialize(program, requireStaticCalls, error, errorLine);
    }

} // namespace IRGenericStatic
