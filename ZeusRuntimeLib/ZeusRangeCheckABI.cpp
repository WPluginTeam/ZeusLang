#include "ZeusRangeCheckABI.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusVariant.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

using OwnedZeusBox = std::unique_ptr<ZeusBox, decltype(&zeus_box_release)>;

[[noreturn]] void rangeCheckPanic(
    int32_t checkKind,
    int32_t sourceLine
) {
    const char* message = nullptr;
    const char* details = nullptr;
    const char* suggestion = nullptr;

    switch (checkKind) {
        case ZEUS_RANGE_CHECK_NEGATIVE_END:
            message = "For loop end value cannot be negative";
            details = "For loop range must have a non-negative end value";
            suggestion =
                "Check the loop range: for (start..end..step) requires end >= 0";
            break;
        case ZEUS_RANGE_CHECK_ZERO_STEP:
            message = "For loop step cannot be zero";
            details = "A zero step would cause an infinite loop";
            suggestion =
                "Use a non-zero step value in for (start..end..step)";
            break;
        default:
            message = "Invalid range check kind";
            details = "The compiled program passed an unknown range check";
            suggestion = "Rebuild the program with a matching Zeus runtime";
            break;
    }

    std::fprintf(stderr, "[Zeus range runtime] %s", message);
    if (sourceLine > 0) std::fprintf(stderr, " at line %d", sourceLine);
    std::fprintf(stderr, "\nDetails: %s\nSuggestion: %s\n", details, suggestion);
    std::abort();
}

bool isInvalidInteger(
    const ZeusType::BigInteger& value,
    int32_t checkKind
) {
    switch (checkKind) {
        case ZEUS_RANGE_CHECK_NEGATIVE_END:
            return value < ZeusType::BigInteger(0);
        case ZEUS_RANGE_CHECK_ZERO_STEP:
            return value == ZeusType::BigInteger(0);
        default:
            rangeCheckPanic(checkKind, 0);
    }
}

const ZeusType::BigInteger* getBigInteger(
    const ZeusType::UniversalVariant& value
) {
    if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value)) {
        return integer;
    }

    const auto* literal = std::get_if<LexerType::Value>(&value);
    if (!literal) return nullptr;
    return std::get_if<ZeusType::BigInteger>(&literal->data);
}

} // namespace

extern "C" void zeus_range_check_i64(
    int64_t value,
    int32_t checkKind,
    int32_t sourceLine
) {
    const bool invalid = checkKind == ZEUS_RANGE_CHECK_NEGATIVE_END
        ? value < 0
        : checkKind == ZEUS_RANGE_CHECK_ZERO_STEP
            ? value == 0
            : true;
    if (invalid) rangeCheckPanic(checkKind, sourceLine);
}

extern "C" void zeus_range_check_value(
    const ZeusValue* value,
    int32_t checkKind,
    int32_t sourceLine
) {
    if (checkKind != ZEUS_RANGE_CHECK_NEGATIVE_END &&
        checkKind != ZEUS_RANGE_CHECK_ZERO_STEP) {
        rangeCheckPanic(checkKind, sourceLine);
    }
    if (!value) rangeCheckPanic(-1, sourceLine);

    OwnedZeusBox box(zeus_value_to_box(value), &zeus_box_release);
    if (!box) rangeCheckPanic(-1, sourceLine);

    const ZeusType::BigInteger* integer = getBigInteger(box->v);
    if (integer && isInvalidInteger(*integer, checkKind)) {
        rangeCheckPanic(checkKind, sourceLine);
    }
}
