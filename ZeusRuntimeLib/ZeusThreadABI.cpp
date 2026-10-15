#include "ZeusThreadABI.hpp"
#include "ZeusRuntimeDebug.hpp"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

enum class RuntimeThreadState {
    Created,
    Starting,
    Running,
    Finished,
    Error
};

struct RuntimeThreadRecord {
    ~RuntimeThreadRecord() {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::ThreadRecord, this);
    }

    int64_t id = -1;
    RuntimeThreadState state = RuntimeThreadState::Created;
    std::unique_ptr<std::thread> thread;
    std::mutex joinMutex;
    std::thread::id nativeId;
    std::string name;
    ZeusThreadTask task = nullptr;
    ZeusThreadContextDestroy destroyContext = nullptr;
    void* context = nullptr;
    std::atomic<bool> stopRequested{false};
    std::string errorMessage;
};

thread_local RuntimeThreadRecord* currentThreadRecord = nullptr;

std::string copyNativeString(ZeusNativeString value) {
    if (!value) throw std::runtime_error("thread name is null");
    uint64_t length = 0;
    std::memcpy(&length, value - sizeof(uint64_t), sizeof(length));
    const uint64_t maximumLength = static_cast<uint64_t>(
        std::numeric_limits<size_t>::max() - sizeof(uint64_t) - 1);
    if (length > maximumLength ||
        length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw std::runtime_error("thread name length is invalid");
    }
    const char* characters = value;
    if (characters[static_cast<size_t>(length)] != '\0') {
        throw std::runtime_error("thread name is missing its terminator");
    }
    return std::string(characters, static_cast<size_t>(length));
}

class RuntimeThreadManager {
public:
    ~RuntimeThreadManager() {
        joinAll();
    }

    int64_t create() {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t id = nextId_++;
        auto record = std::make_shared<RuntimeThreadRecord>();
        ZeusRuntimeDebug::trackCreated(
            ZeusRuntimeDebug::ResourceKind::ThreadRecord,
            record.get(),
            "runtime-thread-record",
            __FILE__,
            __LINE__,
            sizeof(RuntimeThreadRecord)
        );
        record->id = id;
        record->name = "thread-" + std::to_string(id);
        records_.emplace(id, std::move(record));
        return id;
    }

    bool start(
        int64_t id,
        ZeusThreadTask task,
        void* context,
        ZeusThreadContextDestroy destroyContext
    ) {
        if (!task) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(id);
        if (it == records_.end() || it->second->state != RuntimeThreadState::Created) {
            return false;
        }

        const auto record = it->second;
        record->state = RuntimeThreadState::Starting;
        record->task = task;
        record->context = context;
        record->destroyContext = destroyContext;
        try {
            record->thread = std::make_unique<std::thread>([this, record] {
                {
                    std::lock_guard<std::mutex> threadLock(mutex_);
                    record->nativeId = std::this_thread::get_id();
                    record->state = RuntimeThreadState::Running;
                }

                RuntimeThreadRecord* previousRecord = currentThreadRecord;
                currentThreadRecord = record.get();
                try {
                    record->task(record->context);
                } catch (const std::exception& error) {
                    {
                        std::lock_guard<std::mutex> threadLock(mutex_);
                        record->errorMessage = error.what();
                        record->state = RuntimeThreadState::Error;
                    }
                    std::fprintf(stderr,
                        "[ZeusThreadABI] task failed for thread %lld: %s\n",
                        static_cast<long long>(record->id), error.what());
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> threadLock(mutex_);
                        record->errorMessage = "thread task threw a non-standard exception";
                        record->state = RuntimeThreadState::Error;
                    }
                    std::fprintf(stderr,
                        "[ZeusThreadABI] task failed for thread %lld: non-standard exception\n",
                        static_cast<long long>(record->id));
                }

                const auto destroy = record->destroyContext;
                void* taskContext = record->context;
                record->destroyContext = nullptr;
                record->context = nullptr;
                if (destroy) {
                    try {
                        destroy(taskContext);
                    } catch (...) {
                        std::lock_guard<std::mutex> threadLock(mutex_);
                        record->errorMessage = "thread context cleanup threw an exception";
                        record->state = RuntimeThreadState::Error;
                    }
                }
                currentThreadRecord = previousRecord;

                std::lock_guard<std::mutex> threadLock(mutex_);
                if (record->state != RuntimeThreadState::Error) {
                    record->state = RuntimeThreadState::Finished;
                }
            });
        } catch (...) {
            record->state = RuntimeThreadState::Created;
            record->task = nullptr;
            record->context = nullptr;
            record->destroyContext = nullptr;
            return false;
        }
        return true;
    }

    bool join(int64_t id) {
        std::shared_ptr<RuntimeThreadRecord> record;
        bool selfJoin = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = records_.find(id);
            if (it == records_.end()) return false;
            record = it->second;
            selfJoin = record->nativeId == std::this_thread::get_id();
        }

        std::unique_lock<std::mutex> joinLock(record->joinMutex, std::defer_lock);
        if (selfJoin) {
            if (!joinLock.try_lock()) return false;
        } else {
            joinLock.lock();
        }
        if (!record->thread || !record->thread->joinable()) return false;

        if (selfJoin) {
            record->thread->detach();
        } else {
            record->thread->join();
        }

        eraseIfSame(id, record);
        return true;
    }

    bool isAlive(int64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(id);
        return it != records_.end() &&
            it->second->state == RuntimeThreadState::Running;
    }

    int64_t count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int64_t>(records_.size());
    }

    int64_t activeCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t count = 0;
        for (const auto& entry : records_) {
            if (entry.second->state == RuntimeThreadState::Running) {
                ++count;
            }
        }
        return count;
    }

    bool joinAll() {
        std::vector<int64_t> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ids.reserve(records_.size());
            for (const auto& entry : records_) ids.push_back(entry.first);
        }
        bool success = true;
        for (const int64_t id : ids) {
            std::shared_ptr<RuntimeThreadRecord> record;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = records_.find(id);
                if (it == records_.end()) continue;
                record = it->second;
                if (record->state == RuntimeThreadState::Created) {
                    records_.erase(it);
                    continue;
                }
            }
            if (!join(id)) success = false;
        }
        return success;
    }

    bool eliminate(int64_t id) {
        std::shared_ptr<RuntimeThreadRecord> record;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = records_.find(id);
            if (it == records_.end()) return false;
            record = it->second;
            record->stopRequested.store(true);
            if (record->state == RuntimeThreadState::Created) {
                records_.erase(it);
                return true;
            }
        }
        return join(id);
    }

    bool setName(int64_t id, ZeusNativeString name) {
        if (!name) return false;
        std::string copiedName = copyNativeString(name);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(id);
        if (it == records_.end()) return false;
        it->second->name = std::move(copiedName);
        return true;
    }

    ZeusNativeString getName(int64_t id) {
        thread_local std::string snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = records_.find(id);
            snapshot = it == records_.end() ? std::string() : it->second->name;
        }
        return zeus_string_from_data(
            snapshot.data(), static_cast<int64_t>(snapshot.size()));
    }

    bool reset() {
        joinAll();
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
        nextId_ = 1;
        return true;
    }

private:
    void eraseIfSame(
        int64_t id,
        const std::shared_ptr<RuntimeThreadRecord>& record
    ) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(id);
        if (it != records_.end() && it->second == record) records_.erase(it);
    }

    std::mutex mutex_;
    std::unordered_map<int64_t, std::shared_ptr<RuntimeThreadRecord>> records_;
    int64_t nextId_ = 1;
};

RuntimeThreadManager& threadManager() {
    static RuntimeThreadManager manager;
    return manager;
}

// Mutex records stay alive while an in-flight lock operation still holds a
// reference, so a concurrent destroy never invalidates borrowed pointers.
class RuntimeMutexPool {
public:
    int64_t create() {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t id = nextId_++;
        mutexes_.emplace(id, std::make_shared<std::mutex>());
        return id;
    }

    bool lock(int64_t id) {
        const auto entry = find(id);
        if (!entry) return false;
        entry->lock();
        return true;
    }

    bool tryLock(int64_t id) {
        const auto entry = find(id);
        return entry != nullptr && entry->try_lock();
    }

    bool unlock(int64_t id) {
        const auto entry = find(id);
        if (!entry) return false;
        entry->unlock();
        return true;
    }

    bool destroy(int64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return mutexes_.erase(id) != 0;
    }

private:
    std::shared_ptr<std::mutex> find(int64_t id) {
        if (id <= 0) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = mutexes_.find(id);
        return it == mutexes_.end() ? nullptr : it->second;
    }

    std::mutex mutex_;
    std::unordered_map<int64_t, std::shared_ptr<std::mutex>> mutexes_;
    int64_t nextId_ = 1;
};

RuntimeMutexPool& mutexPool() {
    static RuntimeMutexPool pool;
    return pool;
}

} // namespace

extern "C" int64_t zeus_thread_create(void) {
    try {
        return threadManager().create();
    } catch (...) {
        return -1;
    }
}

extern "C" bool zeus_thread_start_task(
    int64_t thread_id,
    ZeusThreadTask task,
    void* context,
    ZeusThreadContextDestroy destroy_context
) {
    try {
        return threadManager().start(thread_id, task, context, destroy_context);
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_thread_join(int64_t thread_id) {
    try {
        return threadManager().join(thread_id);
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_thread_is_alive(int64_t thread_id) {
    try {
        return threadManager().isAlive(thread_id);
    } catch (...) {
        return false;
    }
}

extern "C" int64_t zeus_thread_count(void) {
    try {
        return threadManager().count();
    } catch (...) {
        return 0;
    }
}

extern "C" int64_t zeus_thread_active_count(void) {
    try {
        return threadManager().activeCount();
    } catch (...) {
        return 0;
    }
}

extern "C" bool zeus_thread_join_all(void) {
    try {
        return threadManager().joinAll();
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_thread_eliminate(int64_t thread_id) {
    try {
        return threadManager().eliminate(thread_id);
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_thread_set_name(
    int64_t thread_id,
    ZeusNativeString name
) {
    try {
        return threadManager().setName(thread_id, name);
    } catch (...) {
        return false;
    }
}

extern "C" ZeusNativeString zeus_thread_get_name(int64_t thread_id) {
    try {
        return threadManager().getName(thread_id);
    } catch (...) {
        return nullptr;
    }
}

extern "C" int64_t zeus_thread_current_id(void) {
    return static_cast<int64_t>(std::hash<std::thread::id>{}(
        std::this_thread::get_id()));
}

extern "C" bool zeus_thread_should_stop(void) {
    return currentThreadRecord && currentThreadRecord->stopRequested.load();
}

extern "C" bool zeus_thread_sleep(int64_t milliseconds) {
    if (milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    return true;
}

extern "C" bool zeus_thread_reset(void) {
    try {
        return threadManager().reset();
    } catch (...) {
        return false;
    }
}

extern "C" int64_t zeus_mutex_create(void) {
    try {
        return mutexPool().create();
    } catch (...) {
        return -1;
    }
}

extern "C" bool zeus_mutex_lock(int64_t mutex_id) {
    try {
        return mutexPool().lock(mutex_id);
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_mutex_trylock(int64_t mutex_id) {
    try {
        return mutexPool().tryLock(mutex_id);
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_mutex_unlock(int64_t mutex_id) {
    try {
        return mutexPool().unlock(mutex_id);
    } catch (...) {
        return false;
    }
}

extern "C" bool zeus_mutex_destroy(int64_t mutex_id) {
    try {
        return mutexPool().destroy(mutex_id);
    } catch (...) {
        return false;
    }
}
