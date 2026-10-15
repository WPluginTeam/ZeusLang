#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusThreadABI.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZeusBuiltins {

// VM adapters for the mutex ABI declared in ZeusThreadABI.hpp. The stateful
// pool lives in ZeusRuntimeLib so VM code and AOT code share one pool; these
// adapters only convert arguments and results.

inline int64_t mutexIntegerArgument(
    const std::vector<ZeusType::UniversalVariant>& args,
    const char* operation
) {
    if (!args.empty()) {
        if (const auto* value = std::get_if<LexerType::Value>(&args[0])) {
            if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
                return integer->toInt64();
            }
        }
    }
    throw std::runtime_error(std::string(operation) + ": invalid mutexId");
}

inline ZeusType::UniversalVariant builtin_mutex_create(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    const int64_t mutexId = zeus_mutex_create();
    if (mutexId <= 0) {
        throw std::runtime_error("__mutex_create: mutex pool is unavailable");
    }
    return ZeusType::UniversalVariant(LexerType::Value(static_cast<int>(mutexId)));
}

inline ZeusType::UniversalVariant builtin_mutex_lock(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    const int64_t mutexId = mutexIntegerArgument(args, "__mutex_lock");
    return ZeusType::UniversalVariant(LexerType::Value(zeus_mutex_lock(mutexId)));
}

inline ZeusType::UniversalVariant builtin_mutex_trylock(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    const int64_t mutexId = mutexIntegerArgument(args, "__mutex_trylock");
    return ZeusType::UniversalVariant(LexerType::Value(zeus_mutex_trylock(mutexId)));
}

inline ZeusType::UniversalVariant builtin_mutex_unlock(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    const int64_t mutexId = mutexIntegerArgument(args, "__mutex_unlock");
    return ZeusType::UniversalVariant(LexerType::Value(zeus_mutex_unlock(mutexId)));
}

inline ZeusType::UniversalVariant builtin_mutex_destroy(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    const int64_t mutexId = mutexIntegerArgument(args, "__mutex_destroy");
    return ZeusType::UniversalVariant(LexerType::Value(zeus_mutex_destroy(mutexId)));
}

inline ZeusType::UniversalVariant builtin_thread_current_id(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    return ZeusType::UniversalVariant(
        LexerType::Value(zeus_thread_current_id())
    );
}

inline ZeusType::UniversalVariant builtin_thread_sleep(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.empty()) {
        throw std::runtime_error("__thread_sleep requires 1 argument (milliseconds)");
    }

    int milliseconds = 0;
    if (const auto* value = std::get_if<LexerType::Value>(&args[0])) {
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
            milliseconds = static_cast<int>(integer->toInt64());
        } else if (const auto* decimal = std::get_if<ZeusType::BigDecimal>(&value->data)) {
            milliseconds = static_cast<int>(decimal->toDouble());
        }
    }

    return ZeusType::UniversalVariant(
        LexerType::Value(zeus_thread_sleep(milliseconds))
    );
}

inline ZeusType::UniversalVariant builtin_thread_should_stop(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    return ZeusType::UniversalVariant(LexerType::Value(zeus_thread_should_stop()));
}

inline void registerStandaloneThreadIdBuiltin() {
    registerBuiltin("__thread_current_id", builtin_thread_current_id, 0, 0);
}

inline void registerStandaloneThreadSleepBuiltin() {
    registerBuiltin("__thread_sleep", builtin_thread_sleep, 1, 1);
}

inline void registerStandaloneThreadStopBuiltin() {
    registerBuiltin("__thread_should_stop", builtin_thread_should_stop, 0, 0);
}

inline void registerStandaloneThreadBuiltins() {
    registerStandaloneThreadSleepBuiltin();
    registerStandaloneThreadIdBuiltin();
    registerStandaloneThreadStopBuiltin();
}

} // namespace ZeusBuiltins
