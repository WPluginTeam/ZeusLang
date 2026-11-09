#pragma once

#include <cstdint>

extern "C" {

// Ownership of pipe_read_handle transfers to the capture pump on every call.
// A zero result means startup failed and the handle has been closed.
int64_t zeus_process_capture_start(
    int64_t pipe_read_handle,
    int64_t capacity_bytes,
    int32_t* out_error
) noexcept;

// Copies a stable byte range from the bounded ring. All output pointers except
// destination are required. destination may be null only when max_bytes is 0.
int32_t zeus_process_capture_read(
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
) noexcept;

// Releases the consumer. If the child still holds the pipe writer, the pump
// keeps draining and discarding until EOF, then releases its native state.
int32_t zeus_process_capture_release(int64_t capture_id) noexcept;

}
