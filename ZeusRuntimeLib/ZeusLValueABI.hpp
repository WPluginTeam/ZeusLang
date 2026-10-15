#pragma once

#include "ZeusABI.hpp"
extern "C" {
void* zeus_lvalue_register(void*,const char*,int32_t,uint64_t,int32_t) noexcept(false);
void zeus_lvalue_register_raw(void*,uint64_t,int32_t) noexcept(false);
void zeus_lvalue_register_raw_owned(void*,void*,uint64_t,int32_t) noexcept(false);
void zeus_lvalue_register_native_buffer(void*,void*,const char*) noexcept(false);
void zeus_lvalue_unregister_revealed(void*) noexcept;
void zeus_lvalue_unregister(void*) noexcept;
void zeus_lvalue_require_raw(void*,uint64_t,int32_t) noexcept(false);
void zeus_lvalue_load(ZeusValue*,void*,const char*) noexcept(false);
void zeus_lvalue_store(void*,const char*,const ZeusValue*) noexcept(false);
void zeus_lvalue_register_revealed(void*,const ZeusValue*,void*) noexcept(false);
void* zeus_shared_value_slot_address(const char*,const char*,int32_t) noexcept(false);
}

extern "C" void zeus_lvalue_unregister_span(void*,uint64_t) noexcept;
