#pragma once

// ZeusSharedHeap.hpp
// 进程级共享堆，用于多 VM 实例间的变量共享
// Created 2025/11/27

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>

#include "ZeusVariant.hpp"
#include "ZeusValueSlot.hpp"

namespace ZeusShared {

    // 共享 Cell：带递归互斥锁的值容器
    // 使用递归锁以支持同一线程内的嵌套访问
    // 例如：__heap_lock("x"); x = x + 1; __heap_unlock("x");
    struct SharedCell {

        ZeusValueSlot value;
        mutable std::recursive_mutex mutex;  // 递归互斥锁
        std::atomic<int> refCount{1};
        std::atomic<int> lockCount{0};       // 锁定计数（用于调试）
        uint64_t generation = 0;

        SharedCell() = default;

        explicit SharedCell(const ZeusType::UniversalVariant& v) : value(v) {}

        // 禁止拷贝
        SharedCell(const SharedCell&) = delete;
        SharedCell& operator=(const SharedCell&) = delete;

        // 读取（加锁，返回拷贝）
        ZeusType::UniversalVariant read() const {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            return value.read();
        }

        // 写入（加锁）
        void write(const ZeusType::UniversalVariant& v) {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            value.write(v);
        }

        template <typename Function>
        void withValue(Function&& function) {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            value.withVariant(std::forward<Function>(function));
        }

        template <typename Function>
        void withValue(Function&& function) const {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            value.withVariant(std::forward<Function>(function));
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
        uint64_t nextGeneration = 1;

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
            {
                std::lock_guard<std::recursive_mutex> lock(heapMutex);
                const auto it = namedVars.find(name);
                if (it != namedVars.end()) {
                    return it->second;
                }
            }

            // ZeusValueSlot construction enters the GC graph lock. Build the
            // candidate without holding heapMutex so the heap lock never nests
            // outside the graph lock.
            auto cell = std::make_shared<SharedCell>(initVal);
            {
                std::lock_guard<std::recursive_mutex> lock(heapMutex);
                const auto it = namedVars.find(name);
                if (it != namedVars.end()) {
                    return it->second;
                }
                cell->generation = nextGeneration++;
                if (nextGeneration == 0) nextGeneration = 1;
                namedVars.emplace(name, cell);
            }
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

        // Name-based shared-object handles include the cell generation so a
        // deleted handle cannot bind to a later value stored under the same name.
        std::shared_ptr<SharedCell> find(const std::string& name, uint64_t generation) {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            const auto it = namedVars.find(name);
            if (it == namedVars.end() || it->second->generation != generation) return nullptr;
            return it->second;
        }

        std::shared_ptr<SharedCell> findByAddress(
            const SharedCell* address,
            uint64_t generation = 0,
            std::string* name = nullptr
        ) {
            if (!address) return nullptr;
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            for (const auto& entry : namedVars) {
                if (entry.second.get() != address ||
                    (generation != 0 && entry.second->generation != generation)) {
                    continue;
                }
                if (name) *name = entry.first;
                return entry.second;
            }
            return nullptr;
        }

        // Remove the named source slot. Existing in-flight users may still
        // hold the returned cell, but subsequent name-based lookups fail.
        std::shared_ptr<SharedCell> erase(const std::string& name) {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            const auto it = namedVars.find(name);
            if (it == namedVars.end()) return nullptr;
            auto cell = std::move(it->second);
            namedVars.erase(it);
            return cell;
        }

        std::shared_ptr<SharedCell> erase(const std::string& name, uint64_t generation) {
            std::lock_guard<std::recursive_mutex> lock(heapMutex);
            const auto it = namedVars.find(name);
            if (it == namedVars.end() || it->second->generation != generation) return nullptr;
            auto cell = std::move(it->second);
            namedVars.erase(it);
            return cell;
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
        // Initialize the collector first so this heap (and its rooted slots)
        // is destroyed before the collector at process shutdown.
        (void)ZeusGC::graphMutex();
        static GlobalHeap heap;
        return heap;
    }

}  // namespace ZeusShared
