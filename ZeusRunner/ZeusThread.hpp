#pragma once

// ZeusThread.hpp
// 线程管理器：支持多 VM 实例在不同线程中运行
// Created 2025/11/27

#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <functional>
#include <condition_variable>
#include <queue>
#include <optional>
#include <deque>
#include <limits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../ZeusRuntimeLib/ZeusVariant.hpp"
#include "../ZeusRuntimeLib/ZeusSharedHeap.hpp"
#include "../ZeusRuntimeLib/ZeusThreadABI.hpp"
// 包含全局函数指针声明
#include "ZeusBuiltins.hpp"
#include "../ZeusIR/IRType.hpp"

#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_VM
#include "../ZeusDebug/Utils.hpp"

namespace ZeusThread {

    // 线程入口函数类型
    struct ThreadInfo;  // 前向声明
    using ThreadEntryFunc = std::function<void(std::shared_ptr<ThreadInfo>)>;
    
    // 闘包解析函数类型：从 closureId 获取 protoId
    using ClosureResolverFunc = std::function<int(int closureId)>;
    
    // BoundThis 解析函数类型：从 closureId 获取 boundThis（如果有）
    using BoundThisResolverFunc = std::function<std::optional<ZeusType::ObjectHandle>(int closureId)>;
    
    // 共享对象系统 - 包含子线程需要的所有主 VM 数据（提前声明）
    struct SharedObjectSystem {
        // 类和对象（原有）
        std::vector<ZeusType::ClassInfo>* classes = nullptr;
        void* typeImplementations = nullptr;  // std::unordered_map<std::string, ClassInfo>*
        std::deque<ZeusType::ObjectInstance>* objects = nullptr;  // 使用 deque 指针
        std::shared_mutex* mutex = nullptr;  // 读写锁
        
        // GC: 共享的空闲槽位列表
        std::vector<int>* freeObjectSlots = nullptr;
        
        // 全局变量系统（使用 void* 避免循环依赖）
        void* global = nullptr;               // std::vector<Cell*>*
        std::vector<std::string>* globalNames = nullptr;
        
        // 闭包系统
        void* closures = nullptr;             // std::vector<VM::Closure>*
        std::vector<int>* freeClosureSlots = nullptr;  // GC: 共享的闭包空闲槽位
        std::shared_mutex* closuresMutex = nullptr;    // 闭包系统锁
        void* prototypes = nullptr;           // std::vector<VM::Prototype>*
        void* nameToClosure = nullptr;        // std::unordered_map<std::string,int>*
        void* nameToClass = nullptr;          // std::unordered_map<std::string,int>*
    };

    // 线程状态
    enum class ThreadState {
        Created,    // 已创建，未启动
        Running,    // 运行中
        Finished,   // 已完成
        Error       // 出错
    };

    // 线程信息
    struct ThreadInfo {
        int id;                             // 线程 ID（Zeus 层面）
        std::thread::id nativeId;           // 原生线程 ID
        std::string name;                   // 线程名称
        ThreadState state = ThreadState::Created;
        std::string errorMessage;           // 错误信息（如果有）
        
        // 停止控制标志（用于 eliminate）
        std::atomic<bool> shouldStop{false};

        // 用于线程内 VM 执行的数据（使用指针引用，不复制大型数据）
        const std::vector<std::vector<IRType::IRInstr>>* program = nullptr;  // 指向主程序
        std::vector<std::string> globalNames;  // 保留用于兼容性，但优先使用 sharedGlobalNames
        int protoId = -1;                   // 要执行的函数原型 ID
        std::vector<ZeusType::UniversalVariant> args;  // 闭包参数
        std::optional<ZeusType::ObjectHandle> boundThis;  // 绑定的 this（用于方法引用）
        
        // 共享对象系统（指向主 VM 的数据）
        std::vector<ZeusType::ClassInfo>* sharedClasses = nullptr;
        void* sharedTypeImplementations = nullptr;
        std::deque<ZeusType::ObjectInstance>* sharedObjects = nullptr;  // 使用 deque 指针
        std::shared_mutex* sharedObjectsMutex = nullptr;  // 读写锁，用于同步对象访问
        std::vector<int>* sharedFreeObjectSlots = nullptr;  // GC: 共享的空闲槽位
        
        // 扩展共享：全局变量、闘包系统等
        void* sharedGlobal = nullptr;           // std::vector<Cell*>*
        std::vector<std::string>* sharedGlobalNames = nullptr;
        void* sharedClosures = nullptr;         // std::vector<VM::Closure>*
        std::vector<int>* sharedFreeClosureSlots = nullptr;  // GC: 共享的闭包空闲槽位
        std::shared_mutex* sharedClosuresMutex = nullptr;    // 用于同步闭包访问
        void* sharedPrototypes = nullptr;       // std::vector<VM::Prototype>*
        void* sharedNameToClosure = nullptr;    // std::unordered_map<std::string,int>*
        void* sharedNameToClass = nullptr;      // std::unordered_map<std::string,int>*

        ThreadInfo() : id(-1), name("thread-unknown") {}
        
        explicit ThreadInfo(int threadId) : id(threadId), name("thread-" + std::to_string(threadId)) {}
    };

    // 线程管理器（单例）
    struct VMThreadTaskContext {
        ThreadEntryFunc entryFunc;
        std::shared_ptr<ThreadInfo> info;
    };

    inline void runVMThreadTask(void* rawContext) {
        auto* context = static_cast<VMThreadTaskContext*>(rawContext);
        try {
            context->entryFunc(context->info);
        } catch (const std::exception& error) {
            context->info->state = ThreadState::Error;
            context->info->errorMessage = error.what();
        } catch (...) {
            context->info->state = ThreadState::Error;
            context->info->errorMessage = "thread entry threw a non-standard exception";
            ZEUS_DEBUG_PRINT("[ThreadManager] thread entry threw a non-standard exception: id=%d",
                context->info->id);
        }
    }

    inline void destroyVMThreadTask(void* rawContext) {
        delete static_cast<VMThreadTaskContext*>(rawContext);
    }

    class ThreadManager {
    private:
        std::unordered_map<int, std::shared_ptr<ThreadInfo>> threads;
        std::mutex managerMutex;
        std::vector<std::vector<IRType::IRInstr>> mainProgram;
        std::vector<std::string> mainGlobalNames;

        ThreadManager() = default;

    public:
        ~ThreadManager() = default;

        ThreadManager(const ThreadManager&) = delete;
        ThreadManager& operator=(const ThreadManager&) = delete;

        static ThreadManager& instance() {
            static ThreadManager manager;
            return manager;
        }

        void setMainProgram(
            const std::vector<std::vector<IRType::IRInstr>>& program,
            const std::vector<std::string>& globalNames
        ) {
            std::lock_guard<std::mutex> lock(managerMutex);
            mainProgram = program;
            mainGlobalNames = globalNames;
            ZEUS_DEBUG_PRINT("[ThreadManager] setMainProgram: chunks=%zu, globals=%zu",
                program.size(), globalNames.size());
        }

        const std::vector<std::vector<IRType::IRInstr>>& getMainProgram() const {
            return mainProgram;
        }

        const std::vector<std::string>& getMainGlobalNames() const {
            return mainGlobalNames;
        }

        int createThread() {
            auto info = std::make_shared<ThreadInfo>(-1);
            const int64_t id = zeus_thread_create();
            if (id < 0 || id > std::numeric_limits<int>::max()) {
                ZEUS_DEBUG_PRINT("[ThreadManager] createThread failed: runtime id=%lld",
                    static_cast<long long>(id));
                return -1;
            }
            info->id = static_cast<int>(id);
            info->name = "thread-" + std::to_string(id);
            info->program = &mainProgram;
            info->sharedGlobalNames = &mainGlobalNames;
            try {
                std::lock_guard<std::mutex> lock(managerMutex);
                threads[static_cast<int>(id)] = std::move(info);
            } catch (...) {
                zeus_thread_eliminate(id);
                ZEUS_DEBUG_PRINT("[ThreadManager] createThread failed to record runtime id=%d",
                    static_cast<int>(id));
                return -1;
            }
            ZEUS_DEBUG_PRINT("[ThreadManager] createThread: id=%d", static_cast<int>(id));
            return static_cast<int>(id);
        }

        std::shared_ptr<ThreadInfo> getThread(int id) {
            std::lock_guard<std::mutex> lock(managerMutex);
            const auto it = threads.find(id);
            return it == threads.end() ? nullptr : it->second;
        }

        bool startThread(
            int id,
            int protoId,
            const std::vector<ZeusType::UniversalVariant>& args,
            ThreadEntryFunc entryFunc,
            std::optional<ZeusType::ObjectHandle> boundThis = std::nullopt,
            const SharedObjectSystem& shared = SharedObjectSystem()
        ) {
            std::shared_ptr<ThreadInfo> info;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                const auto it = threads.find(id);
                if (it == threads.end() || it->second->state != ThreadState::Created) {
                    return false;
                }
                info = it->second;
                info->protoId = protoId;
                info->args = args;
                info->boundThis = boundThis;
                info->sharedClasses = shared.classes;
                info->sharedTypeImplementations = shared.typeImplementations;
                info->sharedObjects = shared.objects;
                info->sharedObjectsMutex = shared.mutex;
                info->sharedFreeObjectSlots = shared.freeObjectSlots;
                info->sharedGlobal = shared.global;
                info->sharedGlobalNames = shared.globalNames;
                info->sharedClosures = shared.closures;
                info->sharedFreeClosureSlots = shared.freeClosureSlots;
                info->sharedClosuresMutex = shared.closuresMutex;
                info->sharedPrototypes = shared.prototypes;
                info->sharedNameToClosure = shared.nameToClosure;
                info->sharedNameToClass = shared.nameToClass;
            }

            auto* context = new VMThreadTaskContext{std::move(entryFunc), info};
            if (!zeus_thread_start_task(id, runVMThreadTask, context, destroyVMThreadTask)) {
                delete context;
                ZEUS_DEBUG_PRINT("[ThreadManager] startThread failed: id=%d protoId=%d",
                    id, protoId);
                return false;
            }
            ZEUS_DEBUG_PRINT("[ThreadManager] startThread: id=%d protoId=%d hasBoundThis=%d hasSharedEnv=%d",
                id, protoId, boundThis.has_value(), shared.global != nullptr);
            return true;
        }

        bool joinThread(int id) {
            std::shared_ptr<ThreadInfo> info;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                const auto it = threads.find(id);
                if (it == threads.end()) return false;
                info = it->second;
            }

            if (!zeus_thread_join(id)) return false;
            std::lock_guard<std::mutex> lock(managerMutex);
            const auto it = threads.find(id);
            if (it == threads.end() || it->second != info) return true;
            info->program = nullptr;
            info->globalNames.clear();
            info->args.clear();
            info->state = ThreadState::Finished;
            threads.erase(it);
            ZEUS_DEBUG_PRINT("[ThreadManager] joinThread: cleaned up thread %d", id);
            return true;
        }

        bool isAlive(int id) {
            return zeus_thread_is_alive(id);
        }

        bool eliminateThread(int id) {
            auto info = getThread(id);
            if (!info) return false;
            info->shouldStop.store(true);
            ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread: signaling stop for id=%d", id);
            if (!zeus_thread_eliminate(id)) {
                ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread failed: id=%d", id);
                return false;
            }

            std::lock_guard<std::mutex> lock(managerMutex);
            const auto it = threads.find(id);
            if (it != threads.end() && it->second == info) {
                info->program = nullptr;
                info->globalNames.clear();
                info->args.clear();
                info->state = ThreadState::Finished;
                threads.erase(it);
            }
            ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread: cleaned up thread %d", id);
            return true;
        }

        ThreadState getState(int id) {
            std::lock_guard<std::mutex> lock(managerMutex);
            const auto it = threads.find(id);
            return it == threads.end() ? ThreadState::Error : it->second->state;
        }

        bool setThreadName(int id, const std::string& name) {
            if (name.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
                name.size() > std::numeric_limits<size_t>::max() - sizeof(uint64_t) - 1) {
                return false;
            }
            try {
                const size_t byteCount = sizeof(uint64_t) + name.size() + 1;
                const size_t wordCount =
                    byteCount / sizeof(uint64_t) + (byteCount % sizeof(uint64_t) != 0);
                std::vector<uint64_t> nativeName(wordCount, 0);
                nativeName[0] = static_cast<uint64_t>(name.size());
                char* characters =
                    reinterpret_cast<char*>(nativeName.data()) + sizeof(uint64_t);
                if (!name.empty()) std::memcpy(characters, name.data(), name.size());
                characters[name.size()] = '\0';
                if (!zeus_thread_set_name(id, characters)) return false;
            } catch (...) {
                return false;
            }
            std::lock_guard<std::mutex> lock(managerMutex);
            const auto it = threads.find(id);
            if (it == threads.end()) return false;
            it->second->name = name;
            return true;
        }

        std::string getThreadName(int id) {
            const ZeusNativeString value = zeus_thread_get_name(id);
            if (!value) return {};
            uint64_t length = 0;
            std::memcpy(&length, value - sizeof(uint64_t), sizeof(length));
            if (length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
                length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return {};
            }
            const char* characters = value;
            if (characters[static_cast<size_t>(length)] != '\0') return {};
            return std::string(characters, static_cast<size_t>(length));
        }

        std::string getThreadNameByNativeId(std::thread::id nativeId) {
            std::lock_guard<std::mutex> lock(managerMutex);
            for (const auto& entry : threads) {
                if (entry.second->nativeId == nativeId) return entry.second->name;
            }
            return {};
        }

        size_t threadCount() {
            return static_cast<size_t>(zeus_thread_count());
        }

        size_t activeThreadCount() {
            return static_cast<size_t>(zeus_thread_active_count());
        }

        void joinAll() {
            zeus_thread_join_all();
            std::lock_guard<std::mutex> lock(managerMutex);
            for (auto& entry : threads) {
                entry.second->program = nullptr;
                entry.second->globalNames.clear();
                entry.second->args.clear();
                entry.second->state = ThreadState::Finished;
            }
            threads.clear();
        }

        void cleanup() {
            std::vector<int> completed;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                for (const auto& entry : threads) {
                    if (entry.second->state == ThreadState::Finished ||
                        entry.second->state == ThreadState::Error) {
                        completed.push_back(entry.first);
                    }
                }
            }
            for (const int id : completed) joinThread(id);
        }

        void reset() {
            joinAll();
            zeus_thread_reset();
        }
    };

    inline ThreadManager& getThreadManager() {
        return ThreadManager::instance();
    }

    // 全局线程入口函数指针（由 ZeusVM.hpp 设置）
    inline ThreadEntryFunc& getGlobalThreadEntry() {
        static ThreadEntryFunc func = nullptr;
        return func;
    }

    // 设置全局线程入口函数
    inline void setGlobalThreadEntry(ThreadEntryFunc func) {
        getGlobalThreadEntry() = func;
    }

    // 全局闭包解析器（由 VM 设置）
    inline ClosureResolverFunc& getGlobalClosureResolver() {
        static thread_local ClosureResolverFunc func = nullptr;
        return func;
    }

    // 设置全局闭包解析器
    inline void setGlobalClosureResolver(ClosureResolverFunc func) {
        getGlobalClosureResolver() = func;
    }
    
    // 全局 BoundThis 解析器（由 VM 设置）
    inline BoundThisResolverFunc& getGlobalBoundThisResolver() {
        static thread_local BoundThisResolverFunc func = nullptr;
        return func;
    }
    
    // 设置全局 BoundThis 解析器
    inline void setGlobalBoundThisResolver(BoundThisResolverFunc func) {
        getGlobalBoundThisResolver() = func;
    }
    
    using SharedObjectResolverFunc = std::function<SharedObjectSystem()>;
    
    // 全局共享对象系统解析器（由 VM 设置）
    inline SharedObjectResolverFunc& getGlobalSharedObjectResolver() {
        static thread_local SharedObjectResolverFunc func = nullptr;
        return func;
    }
    
    // 设置全局共享对象系统解析器
    inline void setGlobalSharedObjectResolver(SharedObjectResolverFunc func) {
        getGlobalSharedObjectResolver() = func;
    }
    
    using RetainReleaseResolverFunc = std::function<std::tuple<
        std::function<void(ZeusType::ObjectHandle)>, // retainObject
        std::function<void(ZeusType::ObjectHandle)>, // releaseObject
        std::function<void(int)>, // retainClosure
        std::function<void(int)>  // releaseClosure
    >()>;
    
    // 全局 retain/release 函数解析器（由 VM 设置）
    inline RetainReleaseResolverFunc& getGlobalRetainReleaseResolver() {
        static thread_local RetainReleaseResolverFunc func = nullptr;
        return func;
    }
    
    // 设置全局 retain/release 函数解析器
    inline void setGlobalRetainReleaseResolver(RetainReleaseResolverFunc func) {
        getGlobalRetainReleaseResolver() = func;
    }

}  // namespace ZeusThread

