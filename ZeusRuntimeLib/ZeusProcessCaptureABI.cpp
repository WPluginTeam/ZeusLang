#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ZeusProcessCaptureABI.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr DWORD kReadChunkBytes = 4096;
constexpr size_t kMaximumCaptureBytes = 16U * 1024U * 1024U;

struct CaptureState {
    int64_t id = 0;
    HANDLE pipeRead = nullptr;
    size_t capacity = 0;
    std::vector<uint8_t> ring;
    std::mutex mutex;
    uint64_t totalWritten = 0;
    DWORD errorCode = ERROR_SUCCESS;
    bool done = false;
    bool discard = false;
    bool releaseWhenDone = false;
};

struct CaptureManager {
    std::mutex mutex;
    int64_t nextId = 1;
    std::unordered_map<int64_t, std::shared_ptr<CaptureState>> captures;
};

CaptureManager& captureManager() {
    // Detached readers may still be draining child output during process exit.
    // Let the OS reclaim this process-wide table instead of destroying it while
    // a worker is still returning from ReadFile.
    static CaptureManager* manager = new CaptureManager();
    return *manager;
}

void eraseCapture(const std::shared_ptr<CaptureState>& state) {
    CaptureManager& manager = captureManager();
    std::lock_guard<std::mutex> lock(manager.mutex);
    const auto found = manager.captures.find(state->id);
    if (found != manager.captures.end() && found->second == state) {
        manager.captures.erase(found);
    }
}

void pumpOutput(const std::shared_ptr<CaptureState>& state) {
    std::array<uint8_t, kReadChunkBytes> chunk{};
    for (;;) {
        DWORD bytesRead = 0;
        const BOOL succeeded = ReadFile(
            state->pipeRead,
            chunk.data(),
            static_cast<DWORD>(chunk.size()),
            &bytesRead,
            nullptr);
        if (succeeded && bytesRead > 0) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->discard) {
                for (DWORD index = 0; index < bytesRead; ++index) {
                    const uint64_t absoluteOffset = state->totalWritten + index;
                    state->ring[static_cast<size_t>(
                        absoluteOffset % state->capacity)] = chunk[index];
                }
            }
            state->totalWritten += bytesRead;
            continue;
        }

        DWORD failure = ERROR_SUCCESS;
        if (!succeeded) {
            failure = GetLastError();
            if (failure == ERROR_BROKEN_PIPE || failure == ERROR_NO_DATA) {
                failure = ERROR_SUCCESS;
            }
        }
        CloseHandle(state->pipeRead);
        state->pipeRead = nullptr;

        bool releaseWhenDone = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->errorCode = failure;
            state->done = true;
            releaseWhenDone = state->releaseWhenDone;
        }
        if (releaseWhenDone) eraseCapture(state);
        return;
    }
}

std::shared_ptr<CaptureState> findCapture(int64_t captureId) {
    CaptureManager& manager = captureManager();
    std::lock_guard<std::mutex> lock(manager.mutex);
    const auto found = manager.captures.find(captureId);
    return found == manager.captures.end() ? nullptr : found->second;
}

} // namespace

extern "C" int64_t zeus_process_capture_start(
    int64_t pipe_read_handle,
    int64_t capacity_bytes,
    int32_t* out_error
) noexcept {
    if (out_error) *out_error = ERROR_SUCCESS;
    const HANDLE pipeRead = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(pipe_read_handle));
    if (!pipeRead || pipeRead == INVALID_HANDLE_VALUE) {
        if (out_error) *out_error = ERROR_INVALID_HANDLE;
        return 0;
    }

    if (capacity_bytes <= 0 ||
        static_cast<uint64_t>(capacity_bytes) > kMaximumCaptureBytes) {
        CloseHandle(pipeRead);
        if (out_error) *out_error = ERROR_INVALID_PARAMETER;
        return 0;
    }

    std::shared_ptr<CaptureState> state;
    try {
        state = std::make_shared<CaptureState>();
        state->pipeRead = pipeRead;
        state->capacity = static_cast<size_t>(capacity_bytes);
        state->ring.resize(state->capacity);

        CaptureManager& manager = captureManager();
        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            if (manager.nextId <= 0 ||
                manager.nextId == std::numeric_limits<int64_t>::max()) {
                CloseHandle(pipeRead);
                state->pipeRead = nullptr;
                if (out_error) *out_error = ERROR_TOO_MANY_OPEN_FILES;
                return 0;
            }
            state->id = manager.nextId++;
            manager.captures.emplace(state->id, state);
        }

        try {
            std::thread reader(pumpOutput, state);
            reader.detach();
        } catch (...) {
            eraseCapture(state);
            CloseHandle(pipeRead);
            state->pipeRead = nullptr;
            if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY;
            return 0;
        }
        return state->id;
    } catch (...) {
        if (state && state->pipeRead) {
            CloseHandle(state->pipeRead);
            state->pipeRead = nullptr;
        } else {
            CloseHandle(pipeRead);
        }
        if (state && state->id > 0) eraseCapture(state);
        if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }
}

extern "C" int32_t zeus_process_capture_read(
    int64_t capture_id,
    int64_t requested_offset,
    int64_t max_bytes,
    void* destination,
    int64_t* out_read_start,
    int64_t* out_base_offset,
    int64_t* out_total_written,
    int32_t* out_copied,
    int32_t* out_done,
    int32_t* out_error
) noexcept {
    if (out_error) *out_error = ERROR_INVALID_PARAMETER;
    if (!out_read_start || !out_base_offset || !out_total_written ||
        !out_copied || !out_done || !out_error || max_bytes < 0 ||
        max_bytes > static_cast<int64_t>(kMaximumCaptureBytes) ||
        (max_bytes > 0 && !destination)) {
        return 0;
    }
    const std::shared_ptr<CaptureState> state = findCapture(capture_id);
    if (!state) {
        if (out_error) *out_error = ERROR_INVALID_HANDLE;
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    const uint64_t total = state->totalWritten;
    uint64_t base = total > state->capacity
        ? total - state->capacity
        : 0;
    if (state->discard) base = total;

    uint64_t requested = requested_offset < 0
        ? 0
        : static_cast<uint64_t>(requested_offset);
    uint64_t start = std::max(requested, base);
    if (start > total) start = total;

    uint64_t copied = 0;
    if (!state->discard && max_bytes > 0 && start < total) {
        copied = std::min(
            static_cast<uint64_t>(max_bytes),
            total - start);
        const size_t ringIndex = static_cast<size_t>(start % state->capacity);
        const size_t firstPart = std::min(
            static_cast<size_t>(copied),
            state->capacity - ringIndex);
        auto* output = static_cast<uint8_t*>(destination);
        std::memcpy(output, state->ring.data() + ringIndex, firstPart);
        const size_t secondPart = static_cast<size_t>(copied) - firstPart;
        if (secondPart > 0) {
            std::memcpy(output + firstPart, state->ring.data(), secondPart);
        }
    }

    if (start > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        base > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        total > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return 0;
    }
    *out_read_start = static_cast<int64_t>(start);
    *out_base_offset = static_cast<int64_t>(base);
    *out_total_written = static_cast<int64_t>(total);
    *out_copied = static_cast<int32_t>(copied);
    *out_done = state->done ? 1 : 0;
    *out_error = static_cast<int32_t>(state->errorCode);
    return 1;
}

extern "C" int32_t zeus_process_capture_release(int64_t capture_id) noexcept {
    const std::shared_ptr<CaptureState> state = findCapture(capture_id);
    if (!state) return 0;

    bool done = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->discard = true;
        state->releaseWhenDone = true;
        std::vector<uint8_t>().swap(state->ring);
        done = state->done;
    }
    if (done) eraseCapture(state);
    return 1;
}

#else
#include "ZeusProcessCaptureABI.hpp"

extern "C" {
int64_t zeus_process_capture_start(int64_t, int64_t, int32_t* out_error) noexcept {
    if (out_error) *out_error = -1;
    return 0;
}
int32_t zeus_process_capture_read(int64_t, int64_t, int64_t, void*, int64_t* out_read_start, int64_t* out_base_offset, int64_t* out_total_written, int32_t* out_copied, int32_t* out_done, int32_t* out_error) noexcept {
    if (out_read_start) *out_read_start = 0;
    if (out_base_offset) *out_base_offset = 0;
    if (out_total_written) *out_total_written = 0;
    if (out_copied) *out_copied = 0;
    if (out_done) *out_done = 1;
    if (out_error) *out_error = 0;
    return 0;
}
int32_t zeus_process_capture_release(int64_t) noexcept {
    return 1;
}
}
#endif
