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

#include "../ZeusRuntimeLib/ZeusVariant.hpp"
#include "../ZeusRuntimeLib/ZeusSharedHeap.hpp"
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
        std::unique_ptr<std::thread> thread;
        
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
    class ThreadManager {
    private:

        std::unordered_map<int, std::shared_ptr<ThreadInfo>> threads;
        std::mutex managerMutex;
        std::atomic<int> nextThreadId{1};

        // 主程序的字节码（用于创建新 VM 时复制）
        std::vector<std::vector<IRType::IRInstr>> mainProgram;
        std::vector<std::string> mainGlobalNames;

        ThreadManager() = default;

    public:

        // 析构函数：确保所有线程被 join 或 detach
        ~ThreadManager() {
            // 遍历所有线程，join 可 joinable 的
            for (auto& kv : threads) {
                if (kv.second->thread && kv.second->thread->joinable()) {
                    kv.second->thread->join();
                }
            }
            threads.clear();
        }

        // 禁止拷贝
        ThreadManager(const ThreadManager&) = delete;
        ThreadManager& operator=(const ThreadManager&) = delete;

        // 单例访问
        static ThreadManager& instance() {
            static ThreadManager mgr;
            return mgr;
        }

        // 设置主程序字节码（由主 VM 在启动时调用）
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

        // 获取主程序
        const std::vector<std::vector<IRType::IRInstr>>& getMainProgram() const {
            return mainProgram;
        }

        const std::vector<std::string>& getMainGlobalNames() const {
            return mainGlobalNames;
        }

        // 创建新线程（但不启动）
        int createThread() {
            std::lock_guard<std::mutex> lock(managerMutex);

            int id = nextThreadId++;
            auto info = std::make_shared<ThreadInfo>(id);
            info->program = &mainProgram;  // 使用指针引用，不复制
            info->sharedGlobalNames = &mainGlobalNames;  // 使用指针引用
            threads[id] = info;

            ZEUS_DEBUG_PRINT("[ThreadManager] createThread: id=%d", id);

            return id;
        }

        // 获取线程信息
        std::shared_ptr<ThreadInfo> getThread(int id) {
            std::lock_guard<std::mutex> lock(managerMutex);

            auto it = threads.find(id);
            if (it != threads.end()) {
                return it->second;
            }

            return nullptr;
        }

        // 启动线程 - 需要提供线程入口函数和共享环境
        bool startThread(int id, int protoId, 
                         const std::vector<ZeusType::UniversalVariant>& args,
                         ThreadEntryFunc entryFunc,
                         std::optional<ZeusType::ObjectHandle> boundThis = std::nullopt,
                         const SharedObjectSystem& shared = SharedObjectSystem()) {
            std::shared_ptr<ThreadInfo> info;

            {
                std::lock_guard<std::mutex> lock(managerMutex);

                auto it = threads.find(id);
                if (it == threads.end()) {
                    return false;
                }

                info = it->second;

                if (info->state != ThreadState::Created) {
                    return false;  // 只能启动新创建的线程
                }

                info->protoId = protoId;
                info->args = args;
                info->boundThis = boundThis;
                
                // 原有共享
                info->sharedClasses = shared.classes;
                info->sharedObjects = shared.objects;
                info->sharedObjectsMutex = shared.mutex;
                info->sharedFreeObjectSlots = shared.freeObjectSlots;
                
                // 扩展共享
                info->sharedGlobal = shared.global;
                info->sharedGlobalNames = shared.globalNames;
                info->sharedClosures = shared.closures;
                info->sharedFreeClosureSlots = shared.freeClosureSlots;
                info->sharedClosuresMutex = shared.closuresMutex;
                info->sharedPrototypes = shared.prototypes;
                info->sharedNameToClosure = shared.nameToClosure;
                info->sharedNameToClass = shared.nameToClass;
            }

            // 创建并启动新线程
            info->thread = std::make_unique<std::thread>(entryFunc, info);

            ZEUS_DEBUG_PRINT("[ThreadManager] startThread: id=%d, protoId=%d, hasBoundThis=%d, hasSharedEnv=%d", 
                id, protoId, boundThis.has_value(), shared.global != nullptr);

            return true;
        }

        // 等待线程完成
        bool joinThread(int id) {
            std::shared_ptr<ThreadInfo> info;

            {
                std::lock_guard<std::mutex> lock(managerMutex);
                auto it = threads.find(id);

                if (it == threads.end()) {
                    return false;
                }

                info = it->second;
                
                // 检查是否是当前线程尝试 join 自己（避免死锁）
                auto currentId = std::this_thread::get_id();
                if (info->nativeId == currentId) {
                    // 不能 join 自己，直接标记为完成并清理
                    if (info->thread && info->thread->joinable()) {
                        info->thread->detach();
                    }
                    
                    // 清理资源（program 是指针，不需要清理）
                    info->program = nullptr;
                    info->globalNames.clear();
                    info->globalNames.shrink_to_fit();
                    info->args.clear();
                    info->args.shrink_to_fit();
                    info->state = ThreadState::Finished;
                    threads.erase(id);
                    
                    return true;
                }
            }

            if (info->thread && info->thread->joinable()) {
                info->thread->join();
                
                // 线程join后，立即清理资源
                {
                    std::lock_guard<std::mutex> lock(managerMutex);
                    
                    // 清理ThreadInfo中的数据（program 是指针，不需要清理）
                    info->program = nullptr;
                    info->globalNames.clear();
                    info->globalNames.shrink_to_fit();
                    info->args.clear();
                    info->args.shrink_to_fit();
                    
                    // 更新状态为已完成
                    info->state = ThreadState::Finished;
                    
                    // 从映射中移除
                    threads.erase(id);
                    
                    ZEUS_DEBUG_PRINT("[ThreadManager] joinThread: cleaned up thread %d", id);
                }
                
                return true;
            }

            return false;
        }

        // 检查线程是否存活
        bool isAlive(int id) {
            std::lock_guard<std::mutex> lock(managerMutex);

            auto it = threads.find(id);
            if (it == threads.end()) {
                return false;
            }

            return it->second->state == ThreadState::Running;
        }

        // 终止线程：设置 shouldStop 标志，等待线程自行退出
        bool eliminateThread(int id) {
            std::shared_ptr<ThreadInfo> info;
            bool isSelfEliminate = false;
            
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                
                auto it = threads.find(id);
                if (it == threads.end()) {
                    return false;
                }
                
                info = it->second;
                
                // 检查是否是当前线程尝试 eliminate 自己（避免死锁）
                if (info->nativeId == std::this_thread::get_id()) {
                    isSelfEliminate = true;
                    ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread: thread %d trying to eliminate itself, detaching", id);
                }
            }
            
            // 设置停止标志
            info->shouldStop.store(true);
            
            ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread: signaling stop for id=%d", id);
            
            if (isSelfEliminate) {
                // 不能 join 自己，直接 detach 并清理
                if (info->thread && info->thread->joinable()) {
                    info->thread->detach();
                }
                
                // 清理资源
                {
                    std::lock_guard<std::mutex> lock(managerMutex);
                    info->program = nullptr;
                    info->globalNames.clear();
                    info->globalNames.shrink_to_fit();
                    info->args.clear();
                    info->args.shrink_to_fit();
                    info->state = ThreadState::Finished;
                    threads.erase(id);
                    ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread: self-detached thread %d", id);
                }
                return true;
            }
            
            // 等待线程结束
            if (info->thread && info->thread->joinable()) {
                info->thread->join();
            }
            
            // 清理资源
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                
                // 清理ThreadInfo中的数据（program 是指针，不需要清理）
                info->program = nullptr;
                info->globalNames.clear();
                info->globalNames.shrink_to_fit();
                info->args.clear();
                info->args.shrink_to_fit();
                
                // 更新状态
                info->state = ThreadState::Finished;
                
                // 从映射中移除
                threads.erase(id);
                
                ZEUS_DEBUG_PRINT("[ThreadManager] eliminateThread: cleaned up thread %d", id);
            }
            
            return true;
        }

        // 获取线程状态
        ThreadState getState(int id) {
            std::lock_guard<std::mutex> lock(managerMutex);

            auto it = threads.find(id);
            if (it == threads.end()) {
                return ThreadState::Error;
            }

            return it->second->state;
        }

        // 设置线程名
        bool setThreadName(int id, const std::string& name) {
            std::lock_guard<std::mutex> lock(managerMutex);

            auto it = threads.find(id);
            if (it == threads.end()) {
                return false;
            }

            it->second->name = name;
            return true;
        }

        // 获取线程名
        std::string getThreadName(int id) {
            std::lock_guard<std::mutex> lock(managerMutex);

            auto it = threads.find(id);
            if (it == threads.end()) {
                return "";
            }

            return it->second->name;
        }

        // 根据原生线程 ID 获取线程名（用于错误报告）
        std::string getThreadNameByNativeId(std::thread::id nativeId) {
            std::lock_guard<std::mutex> lock(managerMutex);

            for (const auto& kv : threads) {
                if (kv.second->nativeId == nativeId) {
                    return kv.second->name;
                }
            }

            return "";
        }

        // 获取当前线程数量
        size_t threadCount() {
            std::lock_guard<std::mutex> lock(managerMutex);
            return threads.size();
        }

        // 获取活跃线程数量
        size_t activeThreadCount() {
            std::lock_guard<std::mutex> lock(managerMutex);

            size_t count = 0;
            for (const auto& kv : threads) {
                if (kv.second->state == ThreadState::Running) {
                    ++count;
                }
            }

            return count;
        }

        // 等待所有线程完成
        void joinAll() {
            std::vector<std::shared_ptr<ThreadInfo>> allThreads;
            std::vector<int> allThreadIds;

            {
                std::lock_guard<std::mutex> lock(managerMutex);
                for (auto& kv : threads) {
                    allThreadIds.push_back(kv.first);
                    allThreads.push_back(kv.second);
                }
            }

            for (auto& info : allThreads) {
                if (info->thread && info->thread->joinable()) {
                    info->thread->join();
                }
            }

            // joined entries are no longer useful; clear them to avoid stale
            // thread records accumulating in long-running processes.
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                for (int id : allThreadIds) {
                    auto it = threads.find(id);
                    if (it == threads.end()) continue;

                    it->second->program = nullptr;
                    it->second->globalNames.clear();
                    it->second->globalNames.shrink_to_fit();
                    it->second->args.clear();
                    it->second->args.shrink_to_fit();
                    it->second->state = ThreadState::Finished;
                    threads.erase(it);
                }
            }
        }

        // 清理已完成的线程
        void cleanup() {
            std::lock_guard<std::mutex> lock(managerMutex);

            for (auto it = threads.begin(); it != threads.end(); ) {
                if (it->second->state == ThreadState::Finished ||
                    it->second->state == ThreadState::Error) {

                    if (it->second->thread && it->second->thread->joinable()) {
                        it->second->thread->join();
                    }

                    it = threads.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 重置（清除所有线程）
        void reset() {
            joinAll();

            std::lock_guard<std::mutex> lock(managerMutex);
            threads.clear();
            nextThreadId = 1;
        }
    };

    // 全局线程管理器访问
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
        static ClosureResolverFunc func = nullptr;
        return func;
    }

    // 设置全局闭包解析器
    inline void setGlobalClosureResolver(ClosureResolverFunc func) {
        getGlobalClosureResolver() = func;
    }
    
    // 全局 BoundThis 解析器（由 VM 设置）
    inline BoundThisResolverFunc& getGlobalBoundThisResolver() {
        static BoundThisResolverFunc func = nullptr;
        return func;
    }
    
    // 设置全局 BoundThis 解析器
    inline void setGlobalBoundThisResolver(BoundThisResolverFunc func) {
        getGlobalBoundThisResolver() = func;
    }
    
    using SharedObjectResolverFunc = std::function<SharedObjectSystem()>;
    
    // 全局共享对象系统解析器（由 VM 设置）
    inline SharedObjectResolverFunc& getGlobalSharedObjectResolver() {
        static SharedObjectResolverFunc func = nullptr;
        return func;
    }
    
    // 设置全局共享对象系统解析器
    inline void setGlobalSharedObjectResolver(SharedObjectResolverFunc func) {
        getGlobalSharedObjectResolver() = func;
    }
    
    using RetainReleaseResolverFunc = std::function<std::tuple<
        std::function<void(int)>, // retainObject
        std::function<void(int)>, // releaseObject
        std::function<void(int)>, // retainClosure
        std::function<void(int)>  // releaseClosure
    >()>;
    
    // 全局 retain/release 函数解析器（由 VM 设置）
    inline RetainReleaseResolverFunc& getGlobalRetainReleaseResolver() {
        static RetainReleaseResolverFunc func = nullptr;
        return func;
    }
    
    // 设置全局 retain/release 函数解析器
    inline void setGlobalRetainReleaseResolver(RetainReleaseResolverFunc func) {
        getGlobalRetainReleaseResolver() = func;
    }

}  // namespace ZeusThread

