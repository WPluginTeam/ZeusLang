#pragma once

#include "ZeusABI.hpp"

#include <stdint.h>

enum ZeusRangeCheckKind : int32_t {
    ZEUS_RANGE_CHECK_NEGATIVE_END = 0,
    ZEUS_RANGE_CHECK_ZERO_STEP = 1
};

#ifdef __cplusplus
extern "C" {
#endif

void zeus_range_check_i64(int64_t value, int32_t check_kind, int32_t source_line);
void zeus_range_check_value(
    const ZeusValue* value,
    int32_t check_kind,
    int32_t source_line
);

#ifdef __cplusplus
}
#endif
