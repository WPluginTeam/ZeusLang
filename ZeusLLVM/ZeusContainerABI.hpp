// ZeusLLVM/ZeusContainerABI.hpp
// LLVM IR helpers for lowering dynamic list and dictionary opcodes.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ZeusLLVM::ContainerABI {

using EmitLine = std::function<void(const std::string&)>;
using FreshTemporary = std::function<std::string()>;

inline std::string emitValuePointerArray(
    const std::vector<std::string>& values,
    const FreshTemporary& freshTemporary,
    const EmitLine& emitLine
) {
    if (values.empty()) return "null";

    const std::string array = freshTemporary();
    emitLine(array + " = call ptr @malloc(i64 " +
             std::to_string(values.size() * sizeof(void*)) + ")");
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string slot = freshTemporary();
        emitLine(slot + " = getelementptr inbounds ptr, ptr " + array +
                 ", i64 " + std::to_string(i));
        emitLine("store ptr " + values[i] + ", ptr " + slot + ", align 8");
    }
    return array;
}

inline std::string emitResultCall(
    const std::string& symbol,
    const std::vector<std::string>& arguments,
    const FreshTemporary& freshTemporary,
    const EmitLine& emitLine
) {
    const std::string result = freshTemporary();
    emitLine(result + " = alloca %ZeusValue, align 8");

    std::string call = "call void " + symbol + "(ptr " + result;
    for (const auto& argument : arguments) call += ", ptr " + argument;
    call += ")";
    emitLine(call);
    return result;
}

inline std::string emitArrayIndexCall(
    const std::string& symbol,
    const std::string& array,
    const std::string& index,
    bool isStore,
    const std::string& value,
    const FreshTemporary& freshTemporary,
    const EmitLine& emitLine
) {
    const std::string result = freshTemporary();
    emitLine(result + " = alloca %ZeusValue, align 8");

    std::string call = "call void " + symbol + "(ptr " + result +
        ", ptr " + array + ", i64 " + index;
    if (isStore) call += ", ptr " + value;
    call += ")";
    emitLine(call);
    return result;
}

inline std::string emitBuildCall(
    bool dictionary,
    std::int64_t count,
    const std::vector<std::string>& values,
    const FreshTemporary& freshTemporary,
    const EmitLine& emitLine
) {
    const std::string argumentArray =
        emitValuePointerArray(values, freshTemporary, emitLine);
    const std::string result = freshTemporary();
    emitLine(result + " = alloca %ZeusValue, align 8");

    const std::string symbol = dictionary
        ? "@zeus_value_dict_build"
        : "@zeus_value_array_build";
    emitLine("call void " + symbol + "(ptr " + result + ", i64 " +
             std::to_string(count) + ", ptr " + argumentArray + ")");
    if (argumentArray != "null") emitLine("call void @free(ptr " + argumentArray + ")");
    return result;
}

} // namespace ZeusLLVM::ContainerABI
