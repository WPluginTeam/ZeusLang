#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "IRType.hpp"

namespace IRReferenceValidation {

struct Diagnostic {
    std::string message;
    int line = 0;
    int column = 1;
};

struct FunctionInfo {
    std::string name;
    std::vector<std::string> parameterNames;
    std::vector<std::string> parameterTypes;
    std::vector<bool> parameterIsReference;
};

inline FunctionInfo functionInfo(const std::vector<IRType::IRInstr>& chunk) {
    FunctionInfo result;
    for (const auto& instruction : chunk) {
        if (instruction.metadata.kind != IRType::IRMetadataKind::FunctionSignature) {
            continue;
        }
        result.name = instruction.metadata.symbol.name;
        result.parameterNames = instruction.metadata.parameterNames;
        result.parameterTypes = instruction.metadata.typeNames;
        result.parameterIsReference = instruction.metadata.parameterIsReference;
        break;
    }
    return result;
}

inline bool readIntegerArgument(
    const IRType::IRInstr& instruction,
    size_t index,
    int& result
) {
    if (index >= instruction.args.size()) return false;
    const auto* value = std::get_if<LexerType::Value>(&instruction.args[index]);
    if (!value) return false;
    const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data);
    if (!integer) return false;

    try {
        const std::string text = integer->toString();
        size_t parsed = 0;
        const long long number = std::stoll(text, &parsed);
        if (parsed != text.size() ||
            number < std::numeric_limits<int>::min() ||
            number > std::numeric_limits<int>::max()) {
            return false;
        }
        result = static_cast<int>(number);
        return true;
    } catch (...) {
        return false;
    }
}

inline const std::string* readStringArgument(
    const IRType::IRInstr& instruction,
    size_t index
) {
    if (index >= instruction.args.size()) return nullptr;
    return std::get_if<std::string>(&instruction.args[index]);
}

inline std::vector<Diagnostic> validateNoReferenceParameterCaptures(
    const std::vector<std::vector<IRType::IRInstr>>& program
) {
    std::vector<Diagnostic> diagnostics;

    for (size_t parentIndex = 0; parentIndex < program.size(); ++parentIndex) {
        const auto parent = functionInfo(program[parentIndex]);
        if (parent.parameterIsReference.empty()) continue;

        for (const auto& makeFunction : program[parentIndex]) {
            if (makeFunction.op != VMOperator::MAKE_FUNCTION) continue;

            int childIndex = -1;
            if (!readIntegerArgument(makeFunction, 0, childIndex) ||
                childIndex < 0 || static_cast<size_t>(childIndex) >= program.size()) {
                continue;
            }

            const auto& childChunk = program[static_cast<size_t>(childIndex)];
            const auto child = functionInfo(childChunk);
            for (const auto& capture : childChunk) {
                if (capture.op != VMOperator::CAPTURE_OUTSIDE_VAR) continue;
                const auto* sourceKind = readStringArgument(capture, 0);
                if (!sourceKind || *sourceKind != "local") continue;

                int sourceIndex = -1;
                if (!readIntegerArgument(capture, 1, sourceIndex) || sourceIndex < 0 ||
                    static_cast<size_t>(sourceIndex) >= parent.parameterIsReference.size() ||
                    !parent.parameterIsReference[static_cast<size_t>(sourceIndex)]) {
                    continue;
                }

                const size_t parameterIndex = static_cast<size_t>(sourceIndex);
                const std::string parameterName = parameterIndex < parent.parameterNames.size()
                    ? parent.parameterNames[parameterIndex]
                    : std::string("<unnamed>");
                const std::string parameterType = parameterIndex < parent.parameterTypes.size()
                    ? parent.parameterTypes[parameterIndex]
                    : std::string();
                const std::string parentName = parent.name.empty()
                    ? std::string("<anonymous>")
                    : parent.name;
                const std::string childName = child.name.empty()
                    ? std::string("<anonymous>")
                    : child.name;

                std::string parameterSignature = "&" + parameterName;
                if (!parameterType.empty()) parameterSignature += ": " + parameterType;

                int line = makeFunction.line;
                int column = makeFunction.column;
                if (line <= 0 && !childChunk.empty()) {
                    line = childChunk.front().line;
                    column = childChunk.front().column;
                }
                if (line <= 0) {
                    line = capture.line;
                    column = capture.column;
                }

                diagnostics.push_back({
                    "nested function '" + childName + "' captures by-reference parameter '" +
                        parameterSignature + "' of function '" + parentName +
                        "'; reference parameters are scoped to the active call and cannot be captured",
                    line,
                    column > 0 ? column : 1
                });
            }
        }
    }

    return diagnostics;
}

} // namespace IRReferenceValidation
