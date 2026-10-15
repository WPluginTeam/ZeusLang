// ZeusRuntimeLib/ZeusABI.hpp
//
// Zeus 运行时的对外契约锚点（当前以文档 + 版本宏为主，不引入新的依赖）。
//
// 背景：
// - 本目录（ZeusType / ZeusVariant / ZeusCell / ZeusGC / ZeusSharedHeap /
//   ZeusVMException / ZeusVMUtils）是“纯运行时语义”：值表示、内存管理、
//   错误框架、值运算工具。它们不依赖解释器调度循环（ZeusRunner/ZeusVM）。
// - 解释器（ZeusVM）和未来的 AOT 后端（LLVM Emitter）都只调用本目录的语义，
//   因此本目录就是以后 `libzeus_rt` 静态库的本体。
//
// 演进方向（给 LLVM Emitter 用的 C ABI 落点）：
// - Phase 1（当前）：C++ 头文件形态，解释器直接包含使用。
// - Phase 2：将 ZeusGC::retain/release、ZeusVMUtils 的 int 快路径等包一层
//   `extern "C"` 函数（zeus_int_add / zeus_gc_retain / zeus_throw …），
//   编成 libzeus_rt.lib 供原生代码链接。
// - Phase 3：Emitter 只生成控制流 + 快路径算术，其余语义全部 call 进 C ABI，
//   不支持的 chunk 回退解释器（partial VM 混合 exe）。
//
// 注意：本文件目前不声明任何未实现的函数，仅做版本标记，避免产生链接期悬空符号。

#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define ZEUS_RUNTIME_VERSION_MAJOR 0
#define ZEUS_RUNTIME_VERSION_MINOR 1
#define ZEUS_RUNTIME_VERSION_PATCH 0

// 运行库的值表示版本（UniversalVariant 布局变更时递增，用于 .zir 兼容性检查）。
#define ZEUS_RUNTIME_VALUE_ABI 2

#ifdef __cplusplus
extern "C" {
#endif

// ZeusBox：穿过 C ABI 边界的不透明装箱值（IR 里一切值都是 ZeusBox*）。
// 堆分配、调用方拥有所有权（zeus_box_release 归还；存入全局量后由全局量接管）。
typedef struct ZeusBox ZeusBox;

// Stable, C-compatible dynamic value exchanged at the LLVM/runtime boundary.
// Scalars are inline, strings/bytes own a [u64 length][data] allocation,
// BigInteger/BigDecimal own typed native payloads, other managed values own a
// ZeusBox, and scalar references carry a borrowed address. Do not copy owned
// values bytewise.
#define ZEUS_DYNAMIC_VALUE_ABI 4

typedef enum ZeusValueTag {
    ZEUS_VALUE_TAG_NIL = 0,
    ZEUS_VALUE_TAG_BOOL = 1,
    ZEUS_VALUE_TAG_INT64 = 2,
    ZEUS_VALUE_TAG_BIGINT = 3,
    ZEUS_VALUE_TAG_BIGDECIMAL = 4,
    ZEUS_VALUE_TAG_STRING = 5,
    ZEUS_VALUE_TAG_REF_INT64 = 6,
    ZEUS_VALUE_TAG_REF_BOOL = 7,
    ZEUS_VALUE_TAG_DELETED = 8,
    ZEUS_VALUE_TAG_BYTES = 9,
    ZEUS_VALUE_TAG_CHAR = 10,
    ZEUS_VALUE_TAG_BOXED = 255
} ZeusValueTag;

enum {
    ZEUS_VALUE_FLAG_OWNS_BOX = 1u,
    ZEUS_VALUE_FLAG_OWNS_BUFFER = 2u,
    ZEUS_VALUE_FLAG_OWNS_NATIVE_PAYLOAD = 4u,
    // Preserve whether a native BigInteger/BigDecimal came from the VM's
    // LexerType::Value wrapper when crossing the dynamic ABI.
    ZEUS_VALUE_FLAG_LEXER_VALUE = 8u
};

typedef struct ZeusValue {
    uint32_t tag;
    uint32_t flags;
    union {
        uint64_t payload;
        int64_t int64_payload;
        bool bool_payload;
        uint8_t char_payload;
    };
} ZeusValue;

// Native strings point at UTF-8 data in [u64 byte length][UTF-8 bytes][NUL].
// The pointer addresses the first character; the byte length is stored at data - 8.
typedef const char* ZeusNativeString;

// Native Bytes values point at the first byte in [u64 byte length][bytes].
// The AOT runtime owns each allocation and releases it with zeus_bytes_release.
typedef const uint8_t* ZeusNativeBytes;

// C-compatible value representation for a non-owning raw byte range.
typedef struct ZeusRawBytesRange {
    uint64_t address;
    uint64_t length;
} ZeusRawBytesRange;

// Native closure ABI used by LLVM-generated functions. Captured values are
// held in reference-counted cells; copy captures allocate a new cell at closure
// creation while reference captures retain the parent's cell. Argument values
// and optional variable references are borrowed for the synchronous call.
typedef void (*ZeusClosureInvoke)(
    ZeusValue* out,
    void* environment,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* reference_slots,
    const char* const* argument_names
);
void* zeus_capture_cell_create(const ZeusValue* initial);
void* zeus_capture_cell_clone(void* cell);
void* zeus_capture_cell_retain(void* cell);
void zeus_capture_cell_release(void* cell);
void zeus_capture_cell_load(ZeusValue* out, void* cell);
void zeus_capture_cell_store(void* cell, const ZeusValue* value);
ZeusValue* zeus_capture_cell_value_slot(void* cell);
void zeus_value_slot_load(ZeusValue* out, const ZeusValue* slot);
int64_t zeus_value_slot_load_i64(const ZeusValue* slot);
int32_t zeus_value_slot_load_bool(const ZeusValue* slot);
// Borrowed primitive addresses for synchronous @native reference calls. They
// point into the existing slot (or its referent when the slot is itself a ref).
int64_t* zeus_value_slot_i64_pointer(ZeusValue* slot);
bool* zeus_value_slot_bool_pointer(ZeusValue* slot);
void zeus_value_slot_store(ZeusValue* slot, const ZeusValue* value);
// Generated LLVM class layouts mark embedded ZeusValue fields as graph edges.
// These slots are internal edges, not independent collector roots.
void zeus_value_slot_register_gc_edge(ZeusValue* slot);
void zeus_value_slot_unregister_gc_edge(ZeusValue* slot);
void zeus_value_slot_store_i64(ZeusValue* slot, int64_t value);
void zeus_value_slot_store_bool(ZeusValue* slot, int32_t value);
void zeus_capture_cell_delete(void* cell);
void* zeus_closure_capture_ref(void* environment, int32_t index);
void* zeus_closure_capture_copy(void* environment, int32_t index);
void zeus_closure_create(
    ZeusValue* out,
    ZeusClosureInvoke invoke,
    int32_t arity,
    int32_t capture_count,
    void* const* cells
);
void zeus_closure_create_range(
    ZeusValue* out,
    ZeusClosureInvoke invoke,
    int32_t min_arity,
    int32_t max_arity,
    int32_t capture_count,
    void* const* cells
);
void zeus_closure_call(
    ZeusValue* out,
    const ZeusValue* closure,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* reference_slots
);
void zeus_closure_call_named(
    ZeusValue* out,
    const ZeusValue* closure,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* reference_slots,
    const char* const* argument_names
);
// Reference entries are null for rvalues and point directly to stable mutable
// ZeusValue slots for assignable variables. They are borrowed for this call.
void zeus_closure_bind_arguments(
    int32_t parameter_count,
    const char* const* parameter_names,
    uint64_t default_parameter_mask,
    int32_t has_variadic_args,
    int32_t has_variadic_kwargs,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* reference_slots,
    const char* const* argument_names,
    const ZeusValue** ordered_values,
    ZeusValue** ordered_reference_slots,
    uint64_t* provided_mask,
    ZeusValue* variadic_args,
    ZeusValue* variadic_kwargs
);
void zeus_closure_env_load(ZeusValue* out, void* environment, int32_t index);
void zeus_closure_env_store(void* environment, int32_t index, const ZeusValue* value);
ZeusValue* zeus_closure_env_value_slot(void* environment, int32_t index);
void zeus_closure_env_delete(void* environment, int32_t index);

// Dynamic variadic call ABI. Arguments are borrowed ZeusValue pointers for the
// duration of the call; the callback initializes an owned result in `out`.
typedef void (*ZeusValueVariadicFunction)(
    ZeusValue* out,
    int32_t argc,
    const ZeusValue* const* values
);

// Init/clone/from functions write into an empty destination. Owned references
// must be copied with zeus_value_clone, never with a raw struct assignment.
// push/store consume the source and reset it to nil; pop/load initialize output.
// Arithmetic wrappers borrow inputs and write an independently owned result.
void zeus_value_init_nil(ZeusValue* out);
void zeus_value_init_i64(ZeusValue* out, int64_t value);
void zeus_value_init_bool(ZeusValue* out, int32_t value);
void zeus_value_from_i64_ptr(ZeusValue* out, void* value);
void zeus_value_from_bool_ptr(ZeusValue* out, void* value);
void zeus_value_clone(ZeusValue* out, const ZeusValue* value);
void zeus_value_from_box(ZeusValue* out, const ZeusBox* box);
void zeus_value_from_class_handle(ZeusValue* out, int32_t class_id);
int32_t zeus_value_require_class_handle(
    const ZeusValue* value, int32_t class_count);
// Managed LLVM class handles share one lifetime lease. Explicit delete still
// invalidates every copied handle; the lease only defers freeing its tombstone
// allocation until the last boxed handle is released.
void zeus_value_from_class_ptr(ZeusValue* out, void* object, const char* type_name);
void zeus_class_gc_register(void* object, const char* type_name);
void* zeus_value_require_class_ptr(const ZeusValue* value, const char* type_name);
// Like zeus_value_require_class_ptr, but a nil value returns nullptr instead of
// aborting. A non-nil value of the wrong class still aborts.
void* zeus_value_try_class_ptr(const ZeusValue* value, const char* type_name);
void zeus_class_pointer_invalidate(void* object);
void zeus_class_field_require_initialized(
    int32_t initialized, const char* class_name, const char* field_name, int32_t line) noexcept(false);
// These functions are allowed to throw a C++ exception across generated LLVM
// frames. A catchpad calls capture while its native exception is active.
void zeus_exception_raise(const ZeusValue* value) noexcept(false);
void zeus_exception_raise_owned(ZeusValue* value) noexcept(false);
using ZeusRuntimeErrorFactory = void (*)(ZeusValue*, const char*, int64_t);
void zeus_exception_capture(
    ZeusValue* out,
    ZeusRuntimeErrorFactory runtime_factory,
    ZeusRuntimeErrorFactory type_error_factory
) noexcept(false);
/* Labels must be valid NUL-terminated strings or null; output is capped at 128 bytes. */
#if defined(__cplusplus)
[[noreturn]] void zeus_oom_abort(
    const char* operation, const char* type_name
) noexcept;
#else
void zeus_oom_abort(const char* operation, const char* type_name);
#endif
void zeus_exception_report_current() noexcept;
void zeus_i64_check_division(int64_t left, int64_t right, int32_t line) noexcept(false);
void zeus_value_validate_argument_type(
    const ZeusValue* value,
    const char* function_name,
    int32_t argument_index,
    const char* expected_type
) noexcept(false);
void zeus_value_slot_validate_argument_type(
    const ZeusValue* slot,
    const ZeusValue* value,
    const char* function_name,
    int32_t argument_index,
    const char* expected_type
) noexcept(false);
int64_t zeus_value_require_i64(const ZeusValue* value);
int32_t zeus_value_require_bool(const ZeusValue* value);
int64_t zeus_value_require_argument_i64(
    const ZeusValue* value,
    const char* function_name,
    int32_t argument_index,
    const char* expected_type
) noexcept(false);
int32_t zeus_value_require_argument_bool(
    const ZeusValue* value,
    const char* function_name,
    int32_t argument_index,
    const char* expected_type
) noexcept(false);
int32_t zeus_value_require_char(const ZeusValue* value);
void zeus_value_from_char(ZeusValue* out, int32_t value);
int64_t zeus_value_array_length_i64(const ZeusValue* array);
int64_t zeus_value_dict_length_i64(const ZeusValue* dict);
int64_t zeus_value_dict_contains_key_i64(
    const ZeusValue* dict,
    const ZeusValue* normalized_key
);
void zeus_value_dict_get_normalized(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* normalized_key
);
void zeus_value_dict_store_normalized(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* normalized_key,
    const ZeusValue* value
);
void zeus_value_dict_delete_normalized(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* normalized_key
);
ZeusNativeString zeus_value_intern_string(const ZeusValue* value);
void* zeus_value_native_address(const ZeusValue* value, int32_t reveal);
ZeusNativeString zeus_string_intern(ZeusNativeString view);
ZeusNativeString zeus_string_from_data(const char* data, int64_t length);
ZeusNativeString zeus_string_from_address(int64_t data_address, int64_t length);
ZeusNativeString zeus_string_copy_from_data(const char* data, int64_t length);
void zeus_native_string_validate(ZeusNativeString value);
ZeusNativeBytes zeus_value_copy_bytes(const ZeusValue* value);
ZeusNativeBytes zeus_bytes_from_data(const uint8_t* data, int64_t length);
ZeusNativeBytes zeus_bytes_copy_from_address(int64_t data_address, int64_t length);
ZeusNativeBytes zeus_bytes_clone(ZeusNativeBytes bytes);
void zeus_bytes_release(ZeusNativeBytes bytes);
void zeus_bytes_store_slot(ZeusNativeBytes* slot, ZeusNativeBytes value, int32_t value_owned);
void zeus_bytes_copy_to_address(ZeusNativeBytes bytes, int64_t destination_address, int64_t length);
void zeus_bytes_store_raw_range(ZeusNativeBytes bytes, int64_t destination_address, int64_t length);
void zeus_raw_bytes_range_copy(int64_t destination_address, int64_t source_address, int64_t length);
void zeus_raw_bytes_range_validate(int64_t address, int64_t length);
void zeus_raw_bytes_range_store(
    int64_t destination_address, int64_t destination_length,
    int64_t source_address, int64_t source_length
);
int64_t zeus_raw_bytes_range_get(int64_t address, int64_t length, int64_t index);
void zeus_raw_bytes_range_set(int64_t address, int64_t length, int64_t index, int64_t value);
int64_t zeus_bytes_data_address(ZeusNativeBytes bytes) noexcept(false);
void* zeus_value_require_i64_ptr(const ZeusValue* value);
void* zeus_value_require_bool_ptr(const ZeusValue* value);
ZeusBox* zeus_value_to_box(const ZeusValue* value);
void zeus_value_release(ZeusValue* value);
void zeus_value_from_cstr(ZeusValue* out, const char* text);
void zeus_value_from_string_data(ZeusValue* out, const char* data, int64_t length);
void zeus_value_from_native_string(ZeusValue* out, ZeusNativeString view);
void zeus_value_from_native_bytes(ZeusValue* out, ZeusNativeBytes bytes);
void zeus_value_from_raw_bytes_range(
    ZeusValue* out, int64_t address, int64_t length
);
void zeus_value_require_raw_bytes_range(
    ZeusRawBytesRange* out, const ZeusValue* value
);
void zeus_value_from_bytes(ZeusValue* out, const uint8_t* data, int64_t length);
int64_t zeus_bytes_get(ZeusNativeBytes bytes, int64_t index);
int64_t zeus_bytes_len(ZeusNativeBytes bytes);
ZeusNativeBytes zeus_bytes_concat(
    ZeusNativeBytes left,
    ZeusNativeBytes right
);
ZeusNativeBytes zeus_bytes_slice(
    ZeusNativeBytes bytes,
    int64_t start,
    int64_t end
);
ZeusNativeString zeus_bytes_decode(
    ZeusNativeBytes bytes,
    ZeusNativeString encoding
);
void zeus_bytes_to_list(ZeusValue* out, ZeusNativeBytes bytes);
int64_t zeus_string_length(ZeusNativeString value);
ZeusNativeString zeus_native_string_concat(
    ZeusNativeString left,
    ZeusNativeString right
);
bool zeus_native_string_equal(
    ZeusNativeString left,
    ZeusNativeString right
);
ZeusNativeString zeus_native_string_get(ZeusNativeString value, int64_t index);
ZeusNativeString zeus_native_string_slice(
    ZeusNativeString value,
    int64_t start,
    int64_t end
);
int64_t zeus_native_string_index(
    ZeusNativeString value,
    ZeusNativeString substring
);
int64_t zeus_native_string_rindex(
    ZeusNativeString value,
    ZeusNativeString substring
);
ZeusNativeString zeus_native_string_replace(
    ZeusNativeString value,
    ZeusNativeString old_value,
    ZeusNativeString replacement
);
bool zeus_native_string_startswith(
    ZeusNativeString value,
    ZeusNativeString prefix
);
bool zeus_native_string_endswith(
    ZeusNativeString value,
    ZeusNativeString suffix
);
ZeusNativeString zeus_native_string_lower(ZeusNativeString value);
ZeusNativeString zeus_native_string_upper(ZeusNativeString value);
int64_t zeus_native_string_count(
    ZeusNativeString value,
    ZeusNativeString substring
);
int64_t zeus_string_data_address(ZeusNativeString value) noexcept(false);
void zeus_value_from_bigint_decimal(ZeusValue* out, const char* decimalText);
void zeus_value_from_bigdecimal_decimal(ZeusValue* out, const char* decimalText);
void zeus_value_push(ZeusValue* value);
void zeus_value_pop(ZeusValue* out);
void zeus_value_print(const ZeusValue* value);
void zeus_value_print_many(int32_t argc, const ZeusValue* const* values);
void zeus_value_print_call(
    ZeusValue* out,
    int32_t argc,
    const ZeusValue* const* values
);
// Invoke a VM-independent registered Zeus native callback by stable name.
// Arguments are borrowed and the callback initializes an owned result in out.
void zeus_value_native_call(
    ZeusValue* out,
    const char* builtin_name,
    int32_t argc,
    const ZeusValue* const* values
);
void zeus_value_global_store(int idx, ZeusValue* value);
void zeus_value_global_load(ZeusValue* out, int idx);
ZeusValue* zeus_value_global_slot(int idx);
void zeus_global_clear(int idx);
void zeus_value_global_delete(int idx);
void zeus_value_local_store(int idx, ZeusValue* value);
void zeus_value_local_load(ZeusValue* out, int idx);
void zeus_value_local_delete(int idx);
void zeus_value_delete_object(ZeusValue* value);
void zeus_shared_value_store(const char* name, const ZeusValue* value);
void zeus_shared_value_assign(const char* name, const ZeusValue* value);
void zeus_shared_value_load(ZeusValue* out, const char* name);
void zeus_shared_value_store_i64(const char* name, int64_t value);
void zeus_shared_value_assign_i64(const char* name, int64_t value);
int64_t zeus_shared_value_load_i64(const char* name);
void zeus_shared_value_store_bool(const char* name, int32_t value);
void zeus_shared_value_assign_bool(const char* name, int32_t value);
int32_t zeus_shared_value_load_bool(const char* name);
void* zeus_shared_value_native_address(const char* name, int32_t reveal);
int32_t zeus_shared_value_delete(const char* name);
// Scalar fast path for the same global slots. A top-level loop that bumps a
// scalar global uses these instead of boxing a ZeusValue per access; a type
// mismatch aborts rather than reinterpreting the stored value.
void zeus_global_store_i64(int idx, int64_t value);
int64_t zeus_global_load_i64(int idx);
void zeus_global_store_bool(int idx, int32_t value);
int32_t zeus_global_load_bool(int idx);
void zeus_global_store_string(int idx, ZeusNativeString value);
ZeusNativeString zeus_global_load_string(int idx);
ZeusNativeString* zeus_global_string_slot(int idx);
void zeus_global_sync_native_string_slot(void* slot);
void zeus_global_store_bytes(int idx, ZeusNativeBytes value);
ZeusNativeBytes zeus_global_load_bytes(int idx);
ZeusNativeBytes* zeus_global_bytes_slot(int idx);
void zeus_global_sync_native_bytes_slot(void* slot);
void zeus_value_int_add(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs);
void zeus_value_less_than(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs);
int32_t zeus_value_truthy(const ZeusValue* value);
void zeus_value_dynamic_binary(
    ZeusValue* out,
    void* context,
    int32_t opcode,
    const ZeusValue* lhs,
    const ZeusValue* rhs
);

ZeusBox* zeus_box_int(int64_t v);
ZeusBox* zeus_box_bool(int32_t v);
ZeusBox* zeus_box_cstr(const char* s);
ZeusBox* zeus_null_box(void);
void zeus_box_release(ZeusBox* box);
void zeus_gc_collect(void);
void zeus_gc_safepoint(void);
uint64_t zeus_gc_live_node_count(void);

int64_t zeus_unbox_int(const ZeusBox* box);
int32_t zeus_unbox_bool(const ZeusBox* box);

// 数值/字符串加法（int 快路径 + 大整数/浮点/字符串拼接回退）。
// 返回新装箱；入参为 NULL 或类型不支持时返回 null 装箱。
ZeusBox* zeus_int_add(const ZeusBox* a, const ZeusBox* b);

// 打印：字符串输出原文，其他类型输出调试表示（与解释器 print 一致）。
void zeus_print(const ZeusBox* box);

// 全局量槽（Phase A 极简版：进程内表；Phase B 搬进 ZeusContext）。
// store 拷贝值入槽并吃掉 box；load 返回新装箱（调用方拥有）。
void zeus_global_store(int idx, ZeusBox* box);
ZeusBox* zeus_global_load(int idx);

// ---- Emitter v0 栈机 ----
//
// 生成代码是栈式 IR 的直译：每个 IR 操作对应一次以下调用，
// 因此任意跳转（JUMP/JUMP_IF）在 LLVM 侧都是普通 br，无需 SSA 栈分析。
// 所有权：push 接管，pop 移交。

void zeus_push(ZeusBox* box);
ZeusBox* zeus_pop(void);

// 局部量槽（v0 单帧版；函数支持随 Phase C 到来）。
void zeus_local_store(int idx, ZeusBox* box);
ZeusBox* zeus_local_load(int idx);

// 数值小于比较（与 VM LESS_THAN 同语义：不可比则为 false），返回 bool 装箱。
ZeusBox* zeus_less_than(const ZeusBox* a, const ZeusBox* b);

// 真值测试（与 VM truthy 同语义）：0/1。
int32_t zeus_truthy(const ZeusBox* box);

#ifdef __cplusplus
}
#endif
