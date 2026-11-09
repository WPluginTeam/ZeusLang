#pragma once

#include "../../ZeusRuntimeLib/ZeusProcessCaptureABI.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ZeusBuiltins {

inline int64_t processCaptureIntegerArgument(
    const std::vector<ZeusType::UniversalVariant>& args,
    size_t index,
    const char* operation
) {
    if (index < args.size()) {
        if (const auto* value = std::get_if<LexerType::Value>(&args[index])) {
            if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
                return integer->toInt64();
            }
        }
    }
    throw std::runtime_error(std::string(operation) + ": expected an integer argument");
}

inline ZeusType::UniversalVariant builtin_process_capture_start(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 3U) {
        throw std::runtime_error("__process_capture_start expects 3 arguments");
    }
    const int64_t errorAddress = processCaptureIntegerArgument(
        args, 2, "__process_capture_start");
    if (errorAddress < 0) {
        throw std::runtime_error("__process_capture_start: error pointer is negative");
    }
    const int64_t captureId = zeus_process_capture_start(
        processCaptureIntegerArgument(args, 0, "__process_capture_start"),
        processCaptureIntegerArgument(args, 1, "__process_capture_start"),
        reinterpret_cast<int32_t*>(static_cast<uintptr_t>(errorAddress)));
    return ZeusType::UniversalVariant(LexerType::Value(captureId));
}

inline ZeusType::UniversalVariant builtin_process_capture_read(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 10U) {
        throw std::runtime_error("__process_capture_read expects 10 arguments");
    }
    const auto integer = [&](size_t index) {
        return processCaptureIntegerArgument(args, index, "__process_capture_read");
    };
    const auto pointer = [&](size_t index) -> void* {
        const int64_t address = integer(index);
        if (address < 0) {
            throw std::runtime_error("__process_capture_read: pointer address is negative");
        }
        return reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    };
    const int32_t succeeded = zeus_process_capture_read(
        integer(0), integer(1), integer(2), pointer(3),
        static_cast<int64_t*>(pointer(4)),
        static_cast<int64_t*>(pointer(5)),
        static_cast<int64_t*>(pointer(6)),
        static_cast<int32_t*>(pointer(7)),
        static_cast<int32_t*>(pointer(8)),
        static_cast<int32_t*>(pointer(9)));
    return ZeusType::UniversalVariant(
        LexerType::Value(static_cast<int64_t>(succeeded)));
}

inline ZeusType::UniversalVariant builtin_process_capture_release(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1U) {
        throw std::runtime_error("__process_capture_release expects 1 argument");
    }
    const int32_t released = zeus_process_capture_release(
        processCaptureIntegerArgument(args, 0, "__process_capture_release"));
    return ZeusType::UniversalVariant(
        LexerType::Value(static_cast<int64_t>(released)));
}

} // namespace ZeusBuiltins
