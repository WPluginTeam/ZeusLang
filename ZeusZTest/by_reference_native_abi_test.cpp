#include "../ZeusRuntimeLib/ZeusABI.hpp"

extern "C" int64_t zeus_test_native_reference_increment(ZeusValue* slot) {
    const int64_t next = zeus_value_slot_load_i64(slot) + 1;
    zeus_value_slot_store_i64(slot, next);
    return next;
}
