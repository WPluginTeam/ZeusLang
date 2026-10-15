#pragma once

#include <cstdint>

extern "C" std::int64_t __zeus_malloc(std::int64_t size) noexcept;
extern "C" std::int64_t __zeus_free(std::int64_t address);
extern "C" void* zeus_memory_find_allocation(
    void* address,
    std::uint64_t size
) noexcept;
