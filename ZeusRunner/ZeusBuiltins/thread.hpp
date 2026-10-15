// ZeusBuiltins/thread.hpp - 线程和同步操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"
#include "ThreadNativeBuiltins.hpp"
#include "../ZeusThread.hpp"
#include "../../ZeusRuntimeLib/ZeusSharedHeap.hpp"

namespace ZeusBuiltins {

    // ===== 共享堆锁操作函数 =====

    // __heap_lock(varName): 获取共享变量的写锁
    inline ZeusType::UniversalVariant builtin_heap_lock(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__heap_lock requires 1 argument (varName)");
        }

        std::string varName;

        if (std::holds_alternative<std::string>(args[0])) {
            varName = std::get<std::string>(args[0]);
        }
        else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);

            if (std::holds_alternative<std::string>(v.data)) {
                varName = std::get<std::string>(v.data);
            }
            else {
                throw std::runtime_error("__heap_lock: varName must be string");
            }
        }
        else {
            throw std::runtime_error("__heap_lock: varName must be string");
        }

        auto cell = ZeusShared::getGlobalHeap().find(varName);

        if (!cell) {
            throw std::runtime_error("__heap_lock: undefined shared variable '" + varName + "'");
        }

        cell->lockWrite();
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __heap_unlock(varName): 释放共享变量的写锁
    inline ZeusType::UniversalVariant builtin_heap_unlock(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__heap_unlock requires 1 argument (varName)");
        }

        std::string varName;

        if (std::holds_alternative<std::string>(args[0])) {
            varName = std::get<std::string>(args[0]);
        }
        else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);

            if (std::holds_alternative<std::string>(v.data)) {
                varName = std::get<std::string>(v.data);
            }
            else {
                throw std::runtime_error("__heap_unlock: varName must be string");
            }
        }
        else {
            throw std::runtime_error("__heap_unlock: varName must be string");
        }

        auto cell = ZeusShared::getGlobalHeap().find(varName);

        if (!cell) {
            throw std::runtime_error("__heap_unlock: undefined shared variable '" + varName + "'");
        }

        cell->unlockWrite();
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __heap_exists(varName): 检查共享变量是否存在
    inline ZeusType::UniversalVariant builtin_heap_exists(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__heap_exists requires 1 argument (varName)");
        }

        std::string varName;

        if (std::holds_alternative<std::string>(args[0])) {
            varName = std::get<std::string>(args[0]);
        }
        else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);

            if (std::holds_alternative<std::string>(v.data)) {
                varName = std::get<std::string>(v.data);
            }
            else {
                throw std::runtime_error("__heap_exists: varName must be string");
            }
        }
        else {
            throw std::runtime_error("__heap_exists: varName must be string");
        }

        bool exists = ZeusShared::getGlobalHeap().exists(varName);
        return ZeusType::UniversalVariant(exists);
    }

    // ===== 线程操作函数 =====

    // __thread_create(): 创建新线程，返回线程 ID
    inline ZeusType::UniversalVariant builtin_thread_create(const std::vector<ZeusType::UniversalVariant>& args) {
        int threadId = ZeusThread::getThreadManager().createThread();
        return ZeusType::UniversalVariant(LexerType::Value(threadId));
    }

    // __thread_start(threadId, target, args): args is nil or an argument array.
    // The VM receives the same copied argument sequence as the LLVM runtime.
    inline ZeusType::UniversalVariant builtin_thread_start(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("__thread_start requires threadId, target, and an argument array");
        }

        int threadId = -1;
        int protoId = -1;
        int closureIdToRetain = -1;  // 需要 retain 的闭包 ID
        std::optional<ZeusType::ObjectHandle> boundThis;
        std::vector<ZeusType::UniversalVariant> threadArgs;

        if (!ZeusVMUtils::isNull(args[2])) {
            const ZeusType::Array* argumentArray = ZeusVMUtils::getArrayConst(args[2]);
            if (!argumentArray) return ZeusType::UniversalVariant(false);
            if (argumentArray->impl) threadArgs = argumentArray->impl->data;
        }

        // 获取线程 ID
        if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                threadId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        }

        // 先获取 retain/release 函数（在解析闭包之前）
        auto retainReleaseResolver = ZeusThread::getGlobalRetainReleaseResolver();
        if (retainReleaseResolver) {
            auto [retainObj, releaseObj, retainClo, releaseClo] = retainReleaseResolver();
            g_vmRetainObject = retainObj;
            g_vmReleaseObject = releaseObj;
            g_vmRetainClosure = retainClo;
            g_vmReleaseClosure = releaseClo;
        }

        // 获取 protoId（可以是整数或闭包句柄）
        if (std::holds_alternative<LexerType::Value>(args[1])) {
            const auto& v = std::get<LexerType::Value>(args[1]);
            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                protoId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        } else if (std::holds_alternative<ZeusType::ClosureHandle>(args[1])) {
            // 如果是闭包句柄，先 retain 再获取 protoId
            int closureId = std::get<ZeusType::ClosureHandle>(args[1]).id;
            
            // 在解析之前先 retain 闭包，防止被释放
            if (g_vmRetainClosure) {
                g_vmRetainClosure(closureId);
                closureIdToRetain = closureId;
            }
            
            // 使用全局解析器获取 protoId
            auto resolver = ZeusThread::getGlobalClosureResolver();
            if (resolver) {
                protoId = resolver(closureId);
                
                // 如果 protoId 为 -1，说明闭包已被释放（可能是竞争条件）
                if (protoId < 0) {
                    if (g_vmReleaseClosure && closureIdToRetain >= 0) {
                        g_vmReleaseClosure(closureIdToRetain);
                    }
                    throw std::runtime_error("__thread_start: closure #" + std::to_string(closureId) + 
                        " has been freed (protoId=-1). This may be a race condition - ensure the closure is retained before starting the thread.");
                }
                
                // 同时获取 boundThis（如果有）
                auto boundThisResolver = ZeusThread::getGlobalBoundThisResolver();
                if (boundThisResolver) {
                    boundThis = boundThisResolver(closureId);
                }
            } else {
                // 解析失败，release 闭包
                if (g_vmReleaseClosure && closureIdToRetain >= 0) {
                    g_vmReleaseClosure(closureIdToRetain);
                }
                throw std::runtime_error("__thread_start: closure resolver not registered");
            }
        }

        if (threadId < 0 || protoId < 0) {
            // 参数无效，release 闭包
            if (g_vmReleaseClosure && closureIdToRetain >= 0) {
                g_vmReleaseClosure(closureIdToRetain);
            }
            throw std::runtime_error("__thread_start: invalid threadId or protoId");
        }

        // 获取全局线程入口函数
        auto entryFunc = ZeusThread::getGlobalThreadEntry();
        if (!entryFunc) {
            if (g_vmReleaseClosure && closureIdToRetain >= 0) {
                g_vmReleaseClosure(closureIdToRetain);
            }
            throw std::runtime_error("__thread_start: thread entry function not registered");
        }
        
        // 获取共享对象系统
        ZeusThread::SharedObjectSystem sharedObjects;
        auto sharedObjectResolver = ZeusThread::getGlobalSharedObjectResolver();
        if (sharedObjectResolver) {
            sharedObjects = sharedObjectResolver();
        }

        auto retainThreadArguments = [&threadArgs]() {
            for (const auto& argument : threadArgs) {
                if (std::holds_alternative<ZeusType::ObjectHandle>(argument)) {
                    if (g_vmRetainObject) {
                        g_vmRetainObject(std::get<ZeusType::ObjectHandle>(argument));
                    }
                } else if (std::holds_alternative<ZeusType::ClosureHandle>(argument)) {
                    if (g_vmRetainClosure) {
                        g_vmRetainClosure(std::get<ZeusType::ClosureHandle>(argument).id);
                    }
                }
            }
        };
        auto releaseThreadArguments = [&threadArgs]() {
            for (const auto& argument : threadArgs) {
                if (std::holds_alternative<ZeusType::ObjectHandle>(argument)) {
                    if (g_vmReleaseObject) {
                        g_vmReleaseObject(std::get<ZeusType::ObjectHandle>(argument));
                    }
                } else if (std::holds_alternative<ZeusType::ClosureHandle>(argument)) {
                    if (g_vmReleaseClosure) {
                        g_vmReleaseClosure(std::get<ZeusType::ClosureHandle>(argument).id);
                    }
                }
            }
        };

        bool retainedBoundThis = false;
        if (boundThis.has_value()) {
            if (!g_vmRetainObject || !g_vmReleaseObject) {
                if (g_vmReleaseClosure && closureIdToRetain >= 0) {
                    g_vmReleaseClosure(closureIdToRetain);
                }
                throw std::runtime_error(
                    "__thread_start: bound method requires VM object retain/release callbacks");
            }
            g_vmRetainObject(*boundThis);
            retainedBoundThis = true;
        }

        retainThreadArguments();
        bool started = false;
        try {
            started = ZeusThread::getThreadManager().startThread(
                threadId, protoId, threadArgs, entryFunc, boundThis, sharedObjects);
        } catch (...) {
            releaseThreadArguments();
            if (retainedBoundThis && g_vmReleaseObject) {
                g_vmReleaseObject(*boundThis);
            }
            if (g_vmReleaseClosure && closureIdToRetain >= 0) {
                g_vmReleaseClosure(closureIdToRetain);
            }
            throw;
        }
        if (!started) {
            releaseThreadArguments();
            if (retainedBoundThis && g_vmReleaseObject) {
                g_vmReleaseObject(*boundThis);
            }
        }

        // 线程启动后，release 闭包
        // 闭包只是用来传递 protoId，线程使用的是 protoId（函数原型），不是闭包本身
        if (g_vmReleaseClosure && closureIdToRetain >= 0) {
            g_vmReleaseClosure(closureIdToRetain);
        }

        return ZeusType::UniversalVariant(started);
    }

    // __thread_join(threadId): 等待线程完成
    inline ZeusType::UniversalVariant builtin_thread_join(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__thread_join requires 1 argument (threadId)");
        }

        int threadId = -1;

        if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                threadId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        }

        if (threadId < 0) {
            throw std::runtime_error("__thread_join: invalid threadId");
        }

        bool joined = ZeusThread::getThreadManager().joinThread(threadId);
        return ZeusType::UniversalVariant(joined);
    }

    // __thread_is_alive(threadId): 检查线程是否存活
    inline ZeusType::UniversalVariant builtin_thread_is_alive(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__thread_is_alive requires 1 argument (threadId)");
        }

        int threadId = -1;

        if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                threadId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        }

        if (threadId < 0) {
            return ZeusType::UniversalVariant(false);
        }

        bool alive = ZeusThread::getThreadManager().isAlive(threadId);
        return ZeusType::UniversalVariant(alive);
    }

    // __thread_count(): 获取当前线程数量
    inline ZeusType::UniversalVariant builtin_thread_count(const std::vector<ZeusType::UniversalVariant>& args) {
        size_t count = ZeusThread::getThreadManager().threadCount();
        return ZeusType::UniversalVariant(LexerType::Value(static_cast<int>(count)));
    }

    // __thread_active_count(): 获取活跃线程数量
    inline ZeusType::UniversalVariant builtin_thread_active_count(const std::vector<ZeusType::UniversalVariant>& args) {
        size_t count = ZeusThread::getThreadManager().activeThreadCount();
        return ZeusType::UniversalVariant(LexerType::Value(static_cast<int>(count)));
    }

    // __thread_join_all(): 等待所有线程完成
    inline ZeusType::UniversalVariant builtin_thread_join_all(const std::vector<ZeusType::UniversalVariant>& args) {
        ZeusThread::getThreadManager().joinAll();
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __thread_set_name(threadId, name): 设置线程名
    inline ZeusType::UniversalVariant builtin_thread_set_name(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) {
            throw std::runtime_error("__thread_set_name requires 2 arguments (threadId, name)");
        }

        int threadId = -1;
        std::string name;

        // 获取线程 ID
        if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);

            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                threadId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        }

        // 获取名称
        if (std::holds_alternative<std::string>(args[1])) {
            name = std::get<std::string>(args[1]);
        } else if (std::holds_alternative<LexerType::Value>(args[1])) {
            const auto& v = std::get<LexerType::Value>(args[1]);

            if (std::holds_alternative<std::string>(v.data)) {
                name = std::get<std::string>(v.data);
            }
        }

        if (threadId < 0) {
            throw std::runtime_error("__thread_set_name: invalid threadId");
        }

        bool success = ZeusThread::getThreadManager().setThreadName(threadId, name);
        return ZeusType::UniversalVariant(LexerType::Value(success));
    }

    // __thread_get_name(threadId): 获取线程名
    inline ZeusType::UniversalVariant builtin_thread_get_name(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__thread_get_name requires 1 argument (threadId)");
        }

        int threadId = -1;

        if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);

            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                threadId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        }

        if (threadId < 0) {
            throw std::runtime_error("__thread_get_name: invalid threadId");
        }

        std::string name = ZeusThread::getThreadManager().getThreadName(threadId);
        return ZeusType::UniversalVariant(name);
    }

    inline ZeusType::UniversalVariant builtin_thread_eliminate(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            throw std::runtime_error("__thread_eliminate requires 1 argument (threadId)");
        }

        int threadId = -1;

        if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& v = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
                threadId = static_cast<int>(std::get<ZeusType::BigInteger>(v.data).toInt64());
            }
        }

        if (threadId < 0) {
            throw std::runtime_error("__thread_eliminate: invalid threadId");
        }

        bool eliminated = ZeusThread::getThreadManager().eliminateThread(threadId);
        return ZeusType::UniversalVariant(eliminated);
    }

    // 注册线程和同步相关内置函数
    inline void registerThreadBuiltins() {
        // 共享堆操作（用于多线程共享变量）
        registerBuiltin("__heap_lock", builtin_heap_lock, 1, 1);
        registerBuiltin("__heap_unlock", builtin_heap_unlock, 1, 1);
        registerBuiltin("__heap_exists", builtin_heap_exists, 1, 1);

        // 线程操作（用于多线程支持）
        registerBuiltin("__thread_create", builtin_thread_create, 0, 0);
        registerBuiltin("__thread_start", builtin_thread_start, 3, 3);
        registerBuiltin("__thread_join", builtin_thread_join, 1, 1);
        registerBuiltin("__thread_is_alive", builtin_thread_is_alive, 1, 1);
        registerBuiltin("__thread_count", builtin_thread_count, 0, 0);
        registerBuiltin("__thread_active_count", builtin_thread_active_count, 0, 0);
        registerBuiltin("__thread_join_all", builtin_thread_join_all, 0, 0);
        registerStandaloneThreadSleepBuiltin();
        registerStandaloneThreadIdBuiltin();
        registerStandaloneThreadStopBuiltin();
        registerBuiltin("__thread_eliminate", builtin_thread_eliminate, 1, 1);
        registerBuiltin("__thread_set_name", builtin_thread_set_name, 2, 2);
        registerBuiltin("__thread_get_name", builtin_thread_get_name, 1, 1);
    }

} // namespace ZeusBuiltins
