#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "IRType.hpp"
#include "../ZeusTyping/GenericTypeName.hpp"

namespace IRGenericClass {

    struct ClassTemplate {
        std::string name;
        std::vector<std::string> parameters;
        size_t chunkIndex = 0;
    };

    inline bool readInteger(
        const IRType::IRInstr& instruction,
        size_t argumentIndex,
        int64_t& value
    ) {
        if (argumentIndex >= instruction.args.size() ||
            !std::holds_alternative<LexerType::Value>(instruction.args[argumentIndex])) {
            return false;
        }
        const auto* integer = std::get_if<ZeusType::BigInteger>(
            &std::get<LexerType::Value>(instruction.args[argumentIndex]).data);
        return integer != nullptr && integer->tryToInt64(value);
    }

    inline std::string substituteType(
        const std::string& typeName,
        const std::map<std::string, std::string>& substitutions,
        const std::string& originalClass = {},
        const std::string& specializedClass = {}
    ) {
        if (typeName.empty()) return typeName;
        bool isReference = typeName.front() == '&';
        const std::string base = isReference ? typeName.substr(1) : typeName;
        if (!originalClass.empty() && base == originalClass) {
            return (isReference ? "&" : "") + specializedClass;
        }
        const auto replacement = substitutions.find(base);
        if (replacement != substitutions.end()) {
            return (isReference ? "&" : "") + replacement->second;
        }

        std::string genericBase;
        std::vector<std::string> typeArguments;
        if (!ZeusTypeInfer::decodeGenericSpecializationName(
                base, genericBase, typeArguments)) {
            return typeName;
        }
        bool changed = false;
        for (auto& argument : typeArguments) {
            const std::string substituted = substituteType(
                argument, substitutions, originalClass, specializedClass);
            changed = changed || substituted != argument;
            argument = substituted;
        }
        if (!changed) return typeName;
        const std::string specialized =
            ZeusTypeInfer::genericSpecializationName(genericBase, typeArguments);
        return (isReference ? "&" : "") + specialized;
    }

    inline std::string rewriteQualifiedClassFunctionName(
        const std::string& functionName,
        const std::string& originalClass,
        const std::string& specializedClass
    ) {
        if (originalClass.empty()) return functionName;
        const std::string classSeparator = originalClass + "::";
        const size_t classStart = functionName.rfind(classSeparator);
        if (classStart == std::string::npos ||
            (classStart != 0 && functionName[classStart - 1] != '.')) {
            return functionName;
        }
        return functionName.substr(0, classStart) + specializedClass +
            functionName.substr(classStart + originalClass.size());
    }

    inline size_t prototypeIdForChunk(
        const std::vector<std::vector<IRType::IRInstr>>& program,
        size_t chunkIndex
    ) {
        size_t prototypeId = 0;
        for (size_t i = 0; i < chunkIndex && i < program.size(); ++i) {
            if (!program[i].empty() && program[i].front().op == VMOperator::FUNC_BEGIN) {
                ++prototypeId;
            }
        }
        return prototypeId;
    }

    inline bool isGenericClassTemplateMarker(
        const IRType::IRInstr& instruction
    ) {
        return instruction.op == VMOperator::IR_ANNOTATION &&
            !instruction.args.empty() &&
            std::holds_alternative<std::string>(instruction.args.front()) &&
            std::get<std::string>(instruction.args.front()) ==
                "generic_class_template";
    }

    inline bool isGenericStaticTemplateMarker(
        const IRType::IRInstr& instruction
    ) {
        return instruction.op == VMOperator::IR_ANNOTATION &&
            !instruction.args.empty() &&
            std::holds_alternative<std::string>(instruction.args.front()) &&
            std::get<std::string>(instruction.args.front()) ==
                "generic_type_parameters";
    }

    inline bool containsGenericFunctionTypeParameter(
        const std::string& typeName,
        const std::set<std::string>& parameters
    ) {
        const std::string base = !typeName.empty() && typeName.front() == '&'
            ? typeName.substr(1) : typeName;
        if (parameters.find(base) != parameters.end()) return true;

        constexpr const char* runtimePrefix = "__zeus_typevar:";
        if (base.rfind(runtimePrefix, 0) == 0 &&
            parameters.find(base.substr(
                std::char_traits<char>::length(runtimePrefix))) != parameters.end()) {
            return true;
        }

        std::string genericBase;
        std::vector<std::string> typeArguments;
        if (!ZeusTypeInfer::decodeGenericSpecializationName(
                base, genericBase, typeArguments)) {
            return false;
        }
        return std::any_of(typeArguments.begin(), typeArguments.end(),
            [&](const std::string& argument) {
                return containsGenericFunctionTypeParameter(argument, parameters);
            });
    }

    inline void markGenericClassTemplateMethod(
        std::vector<IRType::IRInstr>& chunk,
        const std::vector<std::string>& typeParameters
    ) {
        if (std::any_of(chunk.begin(), chunk.end(), isGenericClassTemplateMarker)) {
            return;
        }
        IRType::IRInstr marker;
        marker.op = VMOperator::IR_ANNOTATION;
        marker.args.push_back(std::string("generic_class_template"));
        for (const auto& parameter : typeParameters) {
            marker.args.push_back(parameter);
        }
        if (!chunk.empty()) {
            marker.line = chunk.front().line;
            marker.column = chunk.front().column;
        }
        const size_t insertion = std::min<size_t>(2, chunk.size());
        chunk.insert(chunk.begin() + static_cast<std::ptrdiff_t>(insertion),
                     std::move(marker));
    }

    inline void substituteMethodChunk(
        std::vector<IRType::IRInstr>& chunk,
        const std::string& originalClass,
        const std::string& specializedClass,
        const std::map<std::string, std::string>& substitutions
    ) {
        chunk.erase(std::remove_if(
            chunk.begin(), chunk.end(), isGenericClassTemplateMarker), chunk.end());
        for (auto& instruction : chunk) {
            auto& metadata = instruction.metadata;
            metadata.typeName = substituteType(
                metadata.typeName, substitutions, originalClass, specializedClass);
            for (auto& type : metadata.typeNames) {
                type = substituteType(type, substitutions, originalClass, specializedClass);
            }
            for (auto& type : metadata.explicitTypeArguments) {
                type = substituteType(type, substitutions, originalClass, specializedClass);
            }
            if (metadata.ownerType == originalClass) {
                metadata.ownerType = specializedClass;
            }
            if (metadata.kind != IRType::IRMetadataKind::ClassInstantiation &&
                metadata.symbol.kind == IRType::SymbolRefKind::Class) {
                metadata.symbol.name = substituteType(
                    metadata.symbol.name, substitutions, originalClass,
                    specializedClass);
            }
            if ((instruction.op == VMOperator::LLVM_FADDR ||
                 instruction.op == VMOperator::LLVM_FSTORE) &&
                !instruction.args.empty() &&
                std::holds_alternative<std::string>(instruction.args.front())) {
                instruction.args.front() = substituteType(
                    std::get<std::string>(instruction.args.front()), substitutions,
                    originalClass, specializedClass);
            }
            if (instruction.op == VMOperator::PUSH && !instruction.args.empty() &&
                std::holds_alternative<LexerType::Value>(instruction.args.front())) {
                auto* value = std::get_if<std::string>(
                    &std::get<LexerType::Value>(instruction.args.front()).data);
                if (value != nullptr && *value == originalClass) *value = specializedClass;
            }

            if (metadata.kind == IRType::IRMetadataKind::FunctionSignature ||
                metadata.kind == IRType::IRMetadataKind::ExternalFunctionSignature) {
                std::string functionName;
                std::string returnType;
                int captureCount = 0;
                std::vector<std::string> parameterTypes;
                if (metadata.readFunctionSignature(
                        functionName, returnType, captureCount, parameterTypes)) {
                    functionName = rewriteQualifiedClassFunctionName(
                        functionName, originalClass, specializedClass);
                    metadata.symbol = IRType::SymbolRef::function(functionName);
                    metadata.typeName = substituteType(
                        returnType, substitutions, originalClass, specializedClass);
                    metadata.typeNames.clear();
                    metadata.typeNames.reserve(parameterTypes.size());
                    for (const auto& type : parameterTypes) {
                        metadata.typeNames.push_back(substituteType(
                            type, substitutions, originalClass, specializedClass));
                    }
                    metadata.ownerType = specializedClass;
                    if (instruction.args.size() > 1) instruction.args[1] = functionName;
                    if (instruction.args.size() > 2) instruction.args[2] = metadata.typeName;
                    for (size_t i = 0; i < metadata.typeNames.size() &&
                         5 + i < instruction.args.size(); ++i) {
                        instruction.args[5 + i] = metadata.typeNames[i];
                    }
                }
            }

            if (metadata.kind == IRType::IRMetadataKind::DirectCall ||
                metadata.kind == IRType::IRMetadataKind::CallTarget) {
                metadata.symbol.name = rewriteQualifiedClassFunctionName(
                    metadata.symbol.name, originalClass, specializedClass);
            }
        }
    }

    class Specializer {
        using Program = std::vector<std::vector<IRType::IRInstr>>;

        struct ConcreteClass {
            std::string baseName;
            std::string specializedName;
            std::vector<std::string> typeArguments;
            std::map<std::string, std::string> substitutions;
            size_t templateChunk = 0;
        };

        std::string error;
        int errorLine = 0;

        bool fail(
            const IRType::IRInstr& instruction,
            std::string message,
            std::string& outError,
            int& outErrorLine
        ) {
            error = std::move(message);
            errorLine = instruction.line;
            outError = error;
            outErrorLine = errorLine;
            return false;
        }

        static bool readSignature(
            const IRType::IRInstr& instruction,
            std::string& name,
            std::string& owner
        ) {
            int captures = 0;
            std::string returnType;
            std::vector<std::string> parameterTypes;
            return instruction.op == VMOperator::IR_ANNOTATION &&
                instruction.metadata.readFunctionSignature(
                    name, returnType, captures, parameterTypes, &owner);
        }

        static std::vector<size_t> methodChunks(
            const Program& program,
            const std::string& className
        ) {
            std::vector<size_t> result;
            for (size_t ci = 0; ci < program.size(); ++ci) {
                if (program[ci].empty() ||
                    program[ci].front().op != VMOperator::FUNC_BEGIN) continue;
                for (const auto& instruction : program[ci]) {
                    std::string name;
                    std::string owner;
                    if (readSignature(instruction, name, owner) && owner == className) {
                        result.push_back(ci);
                        break;
                    }
                }
            }
            return result;
        }

        static bool rewriteClassChunk(
            std::vector<IRType::IRInstr>& chunk,
            const ClassTemplate& classTemplate,
            const ConcreteClass& concrete,
            const std::map<size_t, size_t>& prototypeMap
        ) {
            bool renamedClassLiteral = false;
            size_t methodDictionaryCount = 0;
            for (auto& instruction : chunk) {
                if (instruction.op == VMOperator::PUSH && !renamedClassLiteral &&
                    !instruction.args.empty() &&
                    std::holds_alternative<LexerType::Value>(instruction.args.front())) {
                    auto* value = std::get_if<std::string>(
                        &std::get<LexerType::Value>(instruction.args.front()).data);
                    if (value != nullptr && *value == classTemplate.name) {
                        *value = concrete.specializedName;
                        renamedClassLiteral = true;
                    }
                }

                if (instruction.op == VMOperator::DICT_BUILD && methodDictionaryCount < 3) {
                    ++methodDictionaryCount;
                } else if (methodDictionaryCount < 3 &&
                           instruction.op == VMOperator::PUSH &&
                           !instruction.args.empty()) {
                    int64_t oldPrototype = -1;
                    if (readInteger(instruction, 0, oldPrototype)) {
                        const auto replacement = prototypeMap.find(
                            static_cast<size_t>(oldPrototype));
                        if (replacement != prototypeMap.end()) {
                            instruction.args[0] = LexerType::Value(
                                ZeusType::BigInteger(static_cast<int64_t>(replacement->second)));
                        }
                    }
                }

                if (instruction.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
                    std::string className;
                    std::vector<std::string> fieldNames;
                    std::vector<std::string> fieldTypes;
                    std::vector<std::string> typeParameters;
                    std::vector<std::string> initializerNames;
                    if (!instruction.metadata.readClassLayout(
                            className, fieldNames, fieldTypes, nullptr, nullptr,
                            &typeParameters, nullptr, &initializerNames) ||
                        className != classTemplate.name) {
                        continue;
                    }
                    for (auto& fieldType : fieldTypes) {
                        fieldType = substituteType(fieldType, concrete.substitutions);
                    }
                    for (auto& initializerName : initializerNames) {
                        initializerName = rewriteQualifiedClassFunctionName(
                            initializerName, classTemplate.name,
                            concrete.specializedName);
                    }
                    instruction.metadata.symbol = IRType::SymbolRef::classType(
                        concrete.specializedName);
                    instruction.metadata.typeName = concrete.specializedName;
                    instruction.metadata.typeNames = std::move(fieldTypes);
                    instruction.metadata.fieldInitializerNames =
                        std::move(initializerNames);
                    instruction.metadata.classTypeParameters.clear();
                }

                if (instruction.op == VMOperator::STORE_VALUE_GLOBAL ||
                    instruction.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                    instruction.op = VMOperator::POP;
                    instruction.args.clear();
                    instruction.metadata = {};
                }
            }
            return renamedClassLiteral;
        }

    public:
        bool specialize(
            Program& program,
            std::string& outError,
            int& outErrorLine,
            bool deferUnknownTemplates
        ) {
            std::map<std::string, ClassTemplate> templates;
            std::map<std::string, bool> registeredClassNames;
            for (size_t ci = 0; ci < program.size(); ++ci) {
                for (const auto& instruction : program[ci]) {
                    if (instruction.op != VMOperator::CLASS_BUILD &&
                        instruction.op != VMOperator::CLASS_BUILD_SHARED) continue;
                    std::string className;
                    std::vector<std::string> fields;
                    std::vector<std::string> fieldTypes;
                    std::vector<std::string> typeParameters;
                    if (instruction.metadata.readClassLayout(
                            className, fields, fieldTypes, nullptr, nullptr,
                            &typeParameters)) {
                        if (!typeParameters.empty()) {
                            templates[className] = ClassTemplate{
                                className, std::move(typeParameters), ci};
                        } else {
                            registeredClassNames[className] = true;
                        }
                    }
                }
            }

            std::map<std::string, std::set<std::string>> genericFunctionTypeParameters;
            for (const auto& chunk : program) {
                std::set<std::string> typeParameters;
                for (const auto& instruction : chunk) {
                    if (!isGenericStaticTemplateMarker(instruction) && !isGenericClassTemplateMarker(instruction)) continue;
                    for (size_t i = 1; i < instruction.args.size(); ++i) {
                        if (!std::holds_alternative<std::string>(instruction.args[i])) {
                            typeParameters.clear();
                            break;
                        }
                        typeParameters.insert(
                            std::get<std::string>(instruction.args[i]));
                    }
                    break;
                }
                if (typeParameters.empty()) continue;

                for (const auto& instruction : chunk) {
                    std::string functionName;
                    std::string ownerType;
                    if (!readSignature(instruction, functionName, ownerType)) continue;
                    auto& collected = genericFunctionTypeParameters[functionName];
                    collected.insert(typeParameters.begin(), typeParameters.end());
                    break;
                }
            }

            for (const auto& entry : templates) {
                const std::vector<size_t> methods = methodChunks(program, entry.first);
                for (const size_t methodChunk : methods) {
                    markGenericClassTemplateMethod(
                        program[methodChunk], entry.second.parameters);
                }
            }

            // Calls may carry a template method's unresolved signature before
            // the receiver has been specialized. Treat its class parameters as
            // deferred call signature parameters as well.
            for (const auto& chunk : program) {
                std::string name, owner;
                for (const auto& instruction : chunk) {
                    if (!readSignature(instruction, name, owner)) continue;
                    const auto type = templates.find(owner);
                    if (type != templates.end()) {
                        auto& parameters = genericFunctionTypeParameters[name];
                        parameters.insert(type->second.parameters.begin(), type->second.parameters.end());
                    }
                    break;
                }
            }

            std::map<std::string, ConcreteClass> concreteClasses;
            std::map<size_t, std::vector<std::vector<IRType::IRInstr>>> classClones;
            std::set<std::string> clonedClasses;
            std::function<bool(
                const std::string&, const std::vector<std::string>&,
                const IRType::IRInstr&, std::string&)> requestConcreteClass;
            std::function<bool(const std::string&, const IRType::IRInstr&)>
                requestTypeName;

            requestTypeName = [&](const std::string& typeName,
                                  const IRType::IRInstr& source) {
                const std::string base =
                    !typeName.empty() && typeName.front() == '&'
                        ? typeName.substr(1) : typeName;
                std::string genericBase;
                std::vector<std::string> typeArguments;
                if (!ZeusTypeInfer::decodeGenericSpecializationName(
                        base, genericBase, typeArguments) ||
                    templates.find(genericBase) == templates.end()) {
                    return true;
                }
                std::string ignored;
                return requestConcreteClass(
                    genericBase, typeArguments, source, ignored);
            };

            requestConcreteClass = [&](const std::string& className,
                                       const std::vector<std::string>& typeArguments,
                                       const IRType::IRInstr& source,
                                       std::string& specializedName) {
                const auto found = templates.find(className);
                if (found == templates.end()) {
                    specializedName = className;
                    return true;
                }
                if (typeArguments.empty()) {
                    return fail(source,
                        "new " + className + " requires explicit @type arguments",
                        outError, outErrorLine);
                }
                if (typeArguments.size() != found->second.parameters.size()) {
                    return fail(source,
                        "new " + className + " expects " +
                        std::to_string(found->second.parameters.size()) +
                        " type argument(s), got " +
                        std::to_string(typeArguments.size()),
                        outError, outErrorLine);
                }
                for (const auto& type : typeArguments) {
                    if (type.empty() || type == "unknown" ||
                        type == "type parameter" ||
                        type.rfind("__zeus_typevar:", 0) == 0) {
                        return fail(source,
                            "generic class type arguments must be concrete",
                            outError, outErrorLine);
                    }
                }

                specializedName = ZeusTypeInfer::genericSpecializationName(
                    className, typeArguments);
                const std::string key = specializedName;
                if (registeredClassNames.find(key) == registeredClassNames.end() &&
                    concreteClasses.find(key) == concreteClasses.end()) {
                    ConcreteClass value;
                    value.baseName = className;
                    value.specializedName = key;
                    value.typeArguments = typeArguments;
                    value.templateChunk = found->second.chunkIndex;
                    for (size_t i = 0; i < typeArguments.size(); ++i) {
                        value.substitutions[found->second.parameters[i]] = typeArguments[i];
                    }
                    concreteClasses.emplace(key, std::move(value));
                }
                for (const auto& argument : typeArguments) {
                    if (!requestTypeName(argument, source)) return false;
                }
                return true;
            };

            auto requestInstructionTypes = [&](IRType::IRInstr& instruction) {
                const auto genericCallTypeParameters =
                    instruction.metadata.kind == IRType::IRMetadataKind::CallTarget
                        ? genericFunctionTypeParameters.find(
                              instruction.metadata.symbol.name)
                        : genericFunctionTypeParameters.end();
                const auto hasUnresolvedGenericCallType =
                    [&](const std::string& typeName) {
                        return genericCallTypeParameters !=
                                genericFunctionTypeParameters.end() &&
                            containsGenericFunctionTypeParameter(
                                typeName, genericCallTypeParameters->second);
                    };
                bool deferGenericClassLayout = false;
                if (instruction.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
                    std::string layoutName;
                    std::vector<std::string> fieldNames;
                    std::vector<std::string> fieldTypes;
                    std::vector<std::string> typeParameters;
                    if (instruction.metadata.readClassLayout(
                            layoutName, fieldNames, fieldTypes, nullptr, nullptr,
                            &typeParameters)) {
                        deferGenericClassLayout = !typeParameters.empty();
                    }
                }
                if (instruction.op == VMOperator::OBJ_BUILD &&
                    instruction.metadata.kind ==
                        IRType::IRMetadataKind::ClassInstantiation) {
                    std::string className;
                    std::vector<std::string> typeArguments;
                    if (instruction.metadata.readClassInstantiation(
                            className, typeArguments)) {
                        const auto found = templates.find(className);
                        if (found == templates.end()) {
                            // Keep unresolved package constructions through the
                            // deferred pass, even with no type arguments, so the
                            // post-merge pass can reject missing generic arguments.
                            if (!typeArguments.empty()) {
                                if (!deferUnknownTemplates) {
                                    std::string message = "new " + className +
                                        " has type arguments, but the class is not generic";
                                    if (!templates.empty()) {
                                        message += "; generic classes available: ";
                                        bool first = true;
                                        for (const auto& entry : templates) {
                                            if (!first) message += ", ";
                                            first = false;
                                            message += entry.first;
                                        }
                                    } else {
                                        message += "; no generic class templates are available in this program";
                                        if (!registeredClassNames.empty()) {
                                            message += "; registered classes: ";
                                            bool first = true;
                                            for (const auto& entry : registeredClassNames) {
                                                if (!first) message += ", ";
                                                first = false;
                                                message += entry.first;
                                            }
                                        }
                                    }
                                    return fail(instruction, std::move(message),
                                                outError, outErrorLine);
                                }
                            } else if (!deferUnknownTemplates) {
                                instruction.metadata = {};
                            }
                        } else {
                            std::string specializedName;
                            if (!requestConcreteClass(
                                    className, typeArguments, instruction,
                                    specializedName)) {
                                return false;
                            }
                            if (!specializedName.empty()) {
                                instruction.metadata.typeName = specializedName;
                            }
                        }
                    }
                }

                if (!hasUnresolvedGenericCallType(instruction.metadata.typeName) &&
                    !requestTypeName(instruction.metadata.typeName, instruction)) {
                    return false;
                }
                if (!deferGenericClassLayout) {
                    for (const auto& type : instruction.metadata.typeNames) {
                        if (hasUnresolvedGenericCallType(type)) continue;
                        if (!requestTypeName(type, instruction)) return false;
                    }
                }
                for (const auto& type : instruction.metadata.explicitTypeArguments) {
                    if (!requestTypeName(type, instruction)) return false;
                }
                if (instruction.metadata.symbol.kind ==
                    IRType::SymbolRefKind::Class &&
                    !requestTypeName(instruction.metadata.symbol.name, instruction)) {
                    return false;
                }
                for (const auto& argument : instruction.args) {
                    if (const auto* type = std::get_if<std::string>(&argument)) {
                        if (hasUnresolvedGenericCallType(*type)) continue;
                        if (!requestTypeName(*type, instruction)) return false;
                    }
                }
                return true;
            };

            for (auto& chunk : program) {
                const bool isTemplateMethod = std::any_of(
                    chunk.begin(), chunk.end(), isGenericClassTemplateMarker);
                const bool isGenericFunctionTemplate = std::any_of(
                    chunk.begin(), chunk.end(), isGenericStaticTemplateMarker);
                if (isTemplateMethod || isGenericFunctionTemplate) continue;
                for (auto& instruction : chunk) {
                    if (!requestInstructionTypes(instruction)) return false;
                }
            }

            while (true) {
                auto pending = std::find_if(
                    concreteClasses.begin(), concreteClasses.end(),
                    [&](const auto& entry) {
                        return clonedClasses.find(entry.first) == clonedClasses.end();
                    });
                if (pending == concreteClasses.end()) break;

                const ConcreteClass concrete = pending->second;
                clonedClasses.insert(pending->first);
                const auto templateIt = templates.find(concrete.baseName);
                if (templateIt == templates.end()) {
                    error = "unable to locate generic class template for " +
                        concrete.specializedName;
                    outError = error;
                    outErrorLine = errorLine;
                    return false;
                }
                const ClassTemplate& classTemplate = templateIt->second;
                const std::vector<size_t> methods = methodChunks(program, classTemplate.name);
                std::map<size_t, size_t> prototypeMap;
                std::vector<std::vector<IRType::IRInstr>> methodClones;
                methodClones.reserve(methods.size());
                for (const size_t sourceChunk : methods) {
                    const size_t oldPrototype = prototypeIdForChunk(program, sourceChunk);
                    auto clone = program[sourceChunk];
                    substituteMethodChunk(
                        clone, classTemplate.name, concrete.specializedName,
                        concrete.substitutions);
                    for (auto& instruction : clone) {
                        if (!requestInstructionTypes(instruction)) return false;
                    }
                    const size_t firstNewPrototype = prototypeIdForChunk(
                        program, program.size());
                    prototypeMap[oldPrototype] =
                        firstNewPrototype + methodClones.size();
                    methodClones.push_back(std::move(clone));
                }
                program.insert(program.end(),
                    std::make_move_iterator(methodClones.begin()),
                    std::make_move_iterator(methodClones.end()));

                auto classClone = program[classTemplate.chunkIndex];
                if (!rewriteClassChunk(
                        classClone, classTemplate, concrete, prototypeMap)) {
                    error = "generic class registration chunk is missing its class name";
                    outError = error;
                    outErrorLine = 0;
                    return false;
                }
                for (auto& instruction : classClone) {
                    if (!requestInstructionTypes(instruction)) return false;
                }
                classClones[classTemplate.chunkIndex].push_back(std::move(classClone));
            }

            for (auto it = classClones.rbegin(); it != classClones.rend(); ++it) {
                const size_t templateChunk = it->first;
                auto& clones = it->second;
                size_t insertion = std::min(templateChunk + 1, program.size());
                for (auto clone = clones.rbegin(); clone != clones.rend(); ++clone) {
                    program.insert(program.begin() + static_cast<std::ptrdiff_t>(insertion),
                                   std::move(*clone));
                }
            }

            // MAKE_FUNCTION stores a chunk index, not a prototype id. Inserting
            // specialized class-registration chunks before the main body shifts
            // later function chunks, so relocate every function reference,
            // including references copied into specialized method chunks.
            const size_t originalChunkCount = program.size();
            size_t insertedClassChunks = 0;
            for (const auto& entry : classClones) {
                insertedClassChunks += entry.second.size();
            }
            if (insertedClassChunks > 0) {
                for (auto& chunk : program) {
                    for (auto& instruction : chunk) {
                        if (instruction.op != VMOperator::MAKE_FUNCTION ||
                            instruction.args.empty() ||
                            !std::holds_alternative<LexerType::Value>(instruction.args[0])) {
                            continue;
                        }
                        auto* value = std::get_if<ZeusType::BigInteger>(
                            &std::get<LexerType::Value>(instruction.args[0]).data);
                        int64_t oldChunkIndex = -1;
                        if (value == nullptr || !value->tryToInt64(oldChunkIndex) ||
                            oldChunkIndex < 0 ||
                            static_cast<uint64_t>(oldChunkIndex) >= originalChunkCount - insertedClassChunks) {
                            continue;
                        }

                        size_t relocatedChunkIndex = static_cast<size_t>(oldChunkIndex);
                        for (const auto& insertion : classClones) {
                            if (insertion.first < static_cast<size_t>(oldChunkIndex)) {
                                relocatedChunkIndex += insertion.second.size();
                            }
                        }
                        instruction.args[0] = LexerType::Value(
                            ZeusType::BigInteger(static_cast<int64_t>(relocatedChunkIndex)));
                    }
                }
            }

            outError.clear();
            outErrorLine = 0;
            return true;
        }
    };

    inline bool specialize(
        std::vector<std::vector<IRType::IRInstr>>& program,
        std::string& error,
        int& errorLine,
        bool deferUnknownTemplates = false
    ) {
        Specializer specializer;
        return specializer.specialize(
            program, error, errorLine, deferUnknownTemplates);
    }

}
