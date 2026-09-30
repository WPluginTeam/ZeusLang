#pragma once

#include "core.hpp"

#include <functional>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ZeusBuiltins {

// VM-independent mutex state shared by the VM builtin and LLVM runtime ABI.
inline std::unordered_map<int, std::unique_ptr<std::mutex>> g_mutexPool;
inline std::mutex g_mutexPoolMutex;
inline int g_nextMutexId = 1;

inline ZeusType::UniversalVariant builtin_mutex_create(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    std::lock_guard<std::mutex> lock(g_mutexPoolMutex);
    const int mutexId = g_nextMutexId++;
    g_mutexPool[mutexId] = std::make_unique<std::mutex>();
    return ZeusType::UniversalVariant(LexerType::Value(mutexId));
}

inline ZeusType::UniversalVariant builtin_mutex_lock(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.empty()) {
        throw std::runtime_error("__mutex_lock requires 1 argument (mutexId)");
    }

    int mutexId = -1;
    if (const auto* value = std::get_if<LexerType::Value>(&args[0])) {
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
            mutexId = static_cast<int>(integer->toInt64());
        }
    }
    if (mutexId < 0) {
        throw std::runtime_error("__mutex_lock: invalid mutexId");
    }

    std::mutex* mutex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutexPoolMutex);
        const auto it = g_mutexPool.find(mutexId);
        if (it == g_mutexPool.end()) {
            throw std::runtime_error("__mutex_lock: mutex not found: " + std::to_string(mutexId));
        }
        mutex = it->second.get();
    }

    mutex->lock();
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_mutex_trylock(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.empty()) {
        throw std::runtime_error("__mutex_trylock requires 1 argument (mutexId)");
    }

    int mutexId = -1;
    if (const auto* value = std::get_if<LexerType::Value>(&args[0])) {
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
            mutexId = static_cast<int>(integer->toInt64());
        }
    }
    if (mutexId < 0) {
        throw std::runtime_error("__mutex_trylock: invalid mutexId");
    }

    std::mutex* mutex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutexPoolMutex);
        const auto it = g_mutexPool.find(mutexId);
        if (it == g_mutexPool.end()) {
            throw std::runtime_error("__mutex_trylock: mutex not found: " + std::to_string(mutexId));
        }
        mutex = it->second.get();
    }

    return ZeusType::UniversalVariant(mutex->try_lock());
}

inline ZeusType::UniversalVariant builtin_mutex_unlock(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.empty()) {
        throw std::runtime_error("__mutex_unlock requires 1 argument (mutexId)");
    }

    int mutexId = -1;
    if (const auto* value = std::get_if<LexerType::Value>(&args[0])) {
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
            mutexId = static_cast<int>(integer->toInt64());
        }
    }
    if (mutexId < 0) {
        throw std::runtime_error("__mutex_unlock: invalid mutexId");
    }

    std::mutex* mutex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutexPoolMutex);
        const auto it = g_mutexPool.find(mutexId);
        if (it == g_mutexPool.end()) {
            throw std::runtime_error("__mutex_unlock: mutex not found: " + std::to_string(mutexId));
        }
        mutex = it->second.get();
    }

    mutex->unlock();
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_mutex_destroy(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.empty()) {
        throw std::runtime_error("__mutex_destroy requires 1 argument (mutexId)");
    }

    int mutexId = -1;
    if (const auto* value = std::get_if<LexerType::Value>(&args[0])) {
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value->data)) {
            mutexId = static_cast<int>(integer->toInt64());
        }
    }
    if (mutexId < 0) {
        throw std::runtime_error("__mutex_destroy: invalid mutexId");
    }

    std::lock_guard<std::mutex> lock(g_mutexPoolMutex);
    const auto it = g_mutexPool.find(mutexId);
    if (it == g_mutexPool.end()) {
        throw std::runtime_error("__mutex_destroy: mutex not found: " + std::to_string(mutexId));
    }
    g_mutexPool.erase(it);
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_thread_current_id(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    const auto id = std::this_thread::get_id();
    const auto hashValue = std::hash<std::thread::id>{}(id);
    return ZeusType::UniversalVariant(
        LexerType::Value(static_cast<int64_t>(hashValue))
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

    if (milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline void registerStandaloneMutexBuiltins() {
    registerBuiltin("__mutex_create", builtin_mutex_create, 0, 0);
    registerBuiltin("__mutex_lock", builtin_mutex_lock, 1, 1);
    registerBuiltin("__mutex_trylock", builtin_mutex_trylock, 1, 1);
    registerBuiltin("__mutex_unlock", builtin_mutex_unlock, 1, 1);
    registerBuiltin("__mutex_destroy", builtin_mutex_destroy, 1, 1);
}

inline void registerStandaloneThreadIdBuiltin() {
    registerBuiltin("__thread_current_id", builtin_thread_current_id, 0, 0);
}

inline void registerStandaloneThreadSleepBuiltin() {
    registerBuiltin("__thread_sleep", builtin_thread_sleep, 1, 1);
}

inline void registerStandaloneThreadBuiltins() {
    registerStandaloneMutexBuiltins();
    registerStandaloneThreadSleepBuiltin();
    registerStandaloneThreadIdBuiltin();
}

} // namespace ZeusBuiltins
