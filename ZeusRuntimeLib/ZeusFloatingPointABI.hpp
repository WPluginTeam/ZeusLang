#pragma once

#include "ZeusABI.hpp"

extern "C" {
double zeus_value_require_double(const ZeusValue* value);
int64_t zeus_double_require_signed_integer(double value, int32_t bits);
void zeus_value_init_double(ZeusValue* out, double value);
double zeus_global_load_double(int index);
void zeus_global_store_double(int index, double value);
}
