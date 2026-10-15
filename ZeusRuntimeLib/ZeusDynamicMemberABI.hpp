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

typedef void (*ZeusDynamicFieldSetter)(
    void* object,
    const ZeusValue* value,
    int32_t line
);

typedef void (*ZeusDynamicValueTraceCallback)(void* context, const ZeusValue* value);
typedef void (*ZeusDynamicFieldPreparer)(void* object);

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

// Registers a field reader for GC graph tracing without exposing that field to
// dynamic attribute lookup.
void zeus_dynamic_field_trace_register(
    const char* type_name,
    const char* field_name,
    ZeusDynamicFieldGetter getter,
    ZeusDynamicFieldPreparer preparer
);

// Registers dynamic-write policy for a known field. Explicitly typed fields may
// provide a native setter; other writes use the dynamic sidecar.
void zeus_dynamic_field_setter_register(
    const char* type_name,
    const char* field_name,
    int32_t writable,
    const char* expected_type,
    ZeusDynamicFieldSetter setter
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

// Loads a dynamic attribute using VM member precedence: an existing field wins,
// otherwise a registered instance method is returned as a bound closure.
void zeus_value_dynamic_attribute_load(
    ZeusValue* out,
    const ZeusValue* receiver,
    const char* member_name,
    int32_t line,
    ZeusClosureInvoke method_invoke
);

void zeus_value_dynamic_field_store(
    const ZeusValue* receiver,
    const char* field_name,
    const ZeusValue* value,
    int32_t line
);

// Drops dynamic fields when the owning native object is destroyed.
void zeus_value_dynamic_field_object_destroy(void* object);
// Replaces the destination object's dynamic field sidecar with a cloned copy
// of the source object's sidecar. Both objects must already have matching types.
void zeus_value_dynamic_field_replace_state(void* destination, void* source);
// Traces dynamic sidecar values as edges owned by their LLVM class instance.
void zeus_value_dynamic_field_trace(
    void* object,
    const char* type_name,
    ZeusDynamicValueTraceCallback callback,
    void* context
);
// Converts inline ZeusValue fields into graph edges when a class first enters
// the managed value graph. Unboxed native class instances keep ordinary roots.
void zeus_value_dynamic_field_prepare(void* object, const char* type_name);

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

extern "C" {
void zeus_value_copy_class_graph(ZeusValue* out, const ZeusValue* source, void* context) noexcept(false);
void zeus_value_dynamic_field_copy_state(void* destination, void* source, void* context) noexcept(false);
}

extern "C" {
void zeus_value_copy_class_graph_default(ZeusValue* out, const ZeusValue* source, void* context) noexcept(false);
void zeus_value_copy_class_graph_internal(ZeusValue* out, const ZeusValue* source, void* context) noexcept(false);
}

extern "C" void zeus_value_copy_class_owned(ZeusValue* out, const ZeusValue* source, int32_t skip_hook) noexcept(false);
