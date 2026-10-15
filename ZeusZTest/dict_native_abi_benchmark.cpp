#include <cstdint>

extern "C" std::int64_t zeus_bench_identity_i64(
    std::int64_t value
) noexcept {
    return value;
}
