#pragma once

// ZeusSharedHeap.hpp
// 进程级共享堆，用于多 VM 实例间的变量共享
// Created 2025/11/27

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <string>
#include <stdexcept>

#include "ZeusVariant.hpp"

namespace ZeusShared {

    // 共享 Cell：带递归互斥锁的值容器
    // 使用递归锁以支持同一线程内的嵌套访问
    // 例如：__heap_lock("x"); x = x + 1; __heap_unlock("x");
    struct SharedCell {

        ZeusType::UniversalVariant value;
        mutable std::recursive_mutex mutex;  // 递归互斥锁
        std::atomic<int> refCount{1};
        std::atomic<int> lockCount{0};       // 锁定计数（用于调试）

        SharedCell() = default;

        explicit SharedCell(const ZeusType::UniversalVariant& v) : value(v) {}

        // 禁止拷贝
        SharedCell(const SharedCell&) = delete;
        SharedCell& operator=(const SharedCell&) = delete;

        // 读取（加锁，返回拷贝）
        ZeusType::UniversalVariant read() const {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            return value;
        }

        // 写入（加锁）
        void write(const ZeusType::UniversalVariant& v) {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            value = v;
        }

        // 获取锁（用于复合操作）
        void lockWrite() {
            mutex.lock();
            ++lockCount;
        }

        // 释放锁
        void unlockWrite() {
            --lockCount;
            mutex.unlock();
        }

        // 获取锁（读写同一个锁）
        void lockRead() const {
            mutex.lock();
        }

        // 释放锁
        void unlockRead() const {
            mutex.unlock();
        }
    };

    // 全局共享堆
    class GlobalHeap {
    private:

        std::unordered_map<std::string, std::shared_ptr<SharedCell>> namedVars;
        std::recursive_mutex heapMutex;

    public:

        GlobalHeap() = default;

        // 禁止拷贝
        GlobalHeap(const GlobalHeap&) = delete;
        GlobalHeap& operator=(const GlobalHeap&) = delete;

        // 创建或获取命名共享变量
        std::shared_ptr<SharedCell> getOrCreate(
            const std::string& name,
            const ZeusType::UniversalVariant& initVal
        ) {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);

            auto it = namedVars.find(name);

            if (it != namedVars.end()) {
                return it->second;
            }

            auto cell = std::make_shared<SharedCell>(initVal);
            namedVars[name] = cell;
            return cell;
        }

        // 仅获取（不创建）
        std::shared_ptr<SharedCell> find(const std::string& name) {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);

            auto it = namedVars.find(name);

            if (it != namedVars.end()) {
                return it->second;
            }

            return nullptr;
        }

        // 检查变量是否存在
        bool exists(const std::string& name) {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            return namedVars.find(name) != namedVars.end();
        }

        // 获取所有共享变量名
        std::vector<std::string> getAllNames() {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);

            std::vector<std::string> names;
            names.reserve(namedVars.size());

            for (const auto& kv : namedVars) {
                names.push_back(kv.first);
            }

            return names;
        }

        // 清空堆（通常仅在进程退出时调用）
        void clear() {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            namedVars.clear();
        }

        // 获取共享变量数量
        size_t size() {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            return namedVars.size();
        }
    };

    // 全局单例访问
    inline GlobalHeap& getGlobalHeap() {
        static GlobalHeap heap;
        return heap;
    }

}  // namespace ZeusShared
