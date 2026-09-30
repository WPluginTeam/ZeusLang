#pragma once

#include "ZeusABI.hpp"

// Generic ABI used by LLVM-generated adapters for dynamically typed member calls.
// The receiver and arguments are borrowed for the duration of the callback.
typedef void (*ZeusDynamicMemberFunction)(
    ZeusValue* out,
    const ZeusValue* receiver,
    int32_t argc,
    const ZeusValue* const* arguments
);

typedef void (*ZeusDynamicFieldGetter)(
    ZeusValue* out,
    const ZeusValue* receiver,
    int32_t line
);

extern "C" {

void zeus_dynamic_member_register(
    const char* type_name,
    const char* method_name,
    ZeusDynamicMemberFunction function
);

void zeus_dynamic_field_register(
    const char* type_name,
    const char* field_name,
    ZeusDynamicFieldGetter getter
);

// Loads a field from a dynamically typed native class receiver. The generated
// class adapter writes an owned dynamic value to `out` or throws on invalid
// receivers, deleted objects, uninitialized fields, and missing fields.
void zeus_value_dynamic_field_load(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* field_name,
    int32_t line
);

// Reports whether a method adapter is registered for a runtime class.
int32_t zeus_dynamic_member_has_method(
    const char* type_name,
    const char* method_name
);

// Dynamic ZeusValue adapter for method-presence checks. Both arguments are
// borrowed and the result is an owned dynamic bool.
void zeus_value_has_dynamic_member(
    ZeusValue* out,
    const ZeusValue* receiver,
    const ZeusValue* method_name
);

// Returns 1 when a method was found and called; 0 when receiver has no
// registered method. The output is initialized to nil in the latter case.
int32_t zeus_value_try_dynamic_member_call(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* method_name,
    int32_t argc,
    const ZeusValue* const* arguments
);

void zeus_value_dynamic_member_call(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* method_name,
    int32_t argc,
    const ZeusValue* const* arguments
);

// DICT_LOAD_VALUE and DICT_STORE_VALUE can represent a runtime class
// subscript as well as a built-in array or dictionary operation.
void zeus_value_subscript_load(
    ZeusValue* out,
    const ZeusValue* container,
    const ZeusValue* key
);
void zeus_value_subscript_store(
    ZeusValue* out,
    const ZeusValue* container,
    const ZeusValue* key,
    const ZeusValue* value
);

[[noreturn]] void zeus_dynamic_member_arity_error(
    const char* method_name,
    int32_t expected,
    int32_t actual
);
[[noreturn]] void zeus_dynamic_member_arity_range_error(
    const char* method_name,
    int32_t minimum,
    int32_t maximum,
    int32_t actual
);

}
