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
// Scalars are inline, box-backed values own a ZeusBox, and scalar reference tags
// carry a non-owning native address. Do not copy owned values bytewise.
#define ZEUS_DYNAMIC_VALUE_ABI 2

typedef enum ZeusValueTag {
    ZEUS_VALUE_TAG_NIL = 0,
    ZEUS_VALUE_TAG_BOOL = 1,
    ZEUS_VALUE_TAG_INT64 = 2,
    ZEUS_VALUE_TAG_BIGINT = 3,
    ZEUS_VALUE_TAG_BIGDECIMAL = 4,
    ZEUS_VALUE_TAG_STRING = 5,
    ZEUS_VALUE_TAG_REF_INT64 = 6,
    ZEUS_VALUE_TAG_REF_BOOL = 7,
    ZEUS_VALUE_TAG_BOXED = 255
} ZeusValueTag;

enum { ZEUS_VALUE_FLAG_OWNS_BOX = 1u };

typedef struct ZeusValue {
    uint32_t tag;
    uint32_t flags;
    uint64_t payload;
} ZeusValue;

// Native string ABI: pointer to immutable UTF-8 bytes plus an explicit length.
// The view and its data remain valid for the runtime process lifetime after
// passing through zeus_string_view_intern / zeus_string_view_from_data /
// zeus_value_intern_string_view.
typedef struct ZeusStringView {
    const char* data;
    uint64_t length;
} ZeusStringView;

// Native Bytes ABI: immutable byte span with an explicit length. Views
// returned by zeus_bytes_view_* and zeus_value_intern_bytes_view remain valid
// for the runtime process lifetime.
typedef struct ZeusBytesView {
    const uint8_t* data;
    uint64_t length;
} ZeusBytesView;

// Native closure ABI used by LLVM-generated functions. Captured values are
// held in reference-counted cells; copy captures allocate a new cell at closure
// creation while reference captures retain the parent's cell.
typedef void (*ZeusClosureInvoke)(
    ZeusValue* out,
    void* environment,
    int32_t argc,
    const ZeusValue* const* values,
    const char* const* argument_names
);
void* zeus_capture_cell_create(const ZeusValue* initial);
void* zeus_capture_cell_clone(void* cell);
void* zeus_capture_cell_retain(void* cell);
void zeus_capture_cell_release(void* cell);
void zeus_capture_cell_load(ZeusValue* out, void* cell);
void zeus_capture_cell_store(void* cell, const ZeusValue* value);
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
    const ZeusValue* const* values
);
void zeus_closure_call_named(
    ZeusValue* out,
    const ZeusValue* closure,
    int32_t argc,
    const ZeusValue* const* values,
    const char* const* argument_names
);
void zeus_closure_bind_arguments(
    int32_t parameter_count,
    const char* const* parameter_names,
    uint64_t default_parameter_mask,
    int32_t has_variadic_args,
    int32_t has_variadic_kwargs,
    int32_t argc,
    const ZeusValue* const* values,
    const char* const* argument_names,
    const ZeusValue** ordered_values,
    uint64_t* provided_mask,
    ZeusValue* variadic_args,
    ZeusValue* variadic_kwargs
);
void zeus_closure_env_load(ZeusValue* out, void* environment, int32_t index);
void zeus_closure_env_store(void* environment, int32_t index, const ZeusValue* value);
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
// Managed LLVM class handles share one lifetime lease. Explicit delete still
// invalidates every copied handle; the lease only defers freeing its tombstone
// allocation until the last boxed handle is released.
void zeus_value_from_class_ptr(ZeusValue* out, void* object, const char* type_name);
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
void zeus_exception_capture(ZeusValue* out, ZeusRuntimeErrorFactory factory) noexcept(false);
void zeus_exception_report_current() noexcept;
void zeus_i64_check_division(int64_t left, int64_t right, int32_t line) noexcept(false);
int64_t zeus_value_require_i64(const ZeusValue* value);
int32_t zeus_value_require_bool(const ZeusValue* value);
const ZeusStringView* zeus_value_intern_string_view(const ZeusValue* value);
const ZeusStringView* zeus_string_view_intern(const ZeusStringView* view);
const ZeusStringView* zeus_string_view_from_data(const char* data, int64_t length);
const ZeusBytesView* zeus_value_intern_bytes_view(const ZeusValue* value);
const ZeusBytesView* zeus_bytes_view_intern(const ZeusBytesView* view);
const ZeusBytesView* zeus_bytes_view_from_data(const uint8_t* data, int64_t length);
void* zeus_value_require_i64_ptr(const ZeusValue* value);
void* zeus_value_require_bool_ptr(const ZeusValue* value);
ZeusBox* zeus_value_to_box(const ZeusValue* value);
void zeus_value_release(ZeusValue* value);
void zeus_value_from_cstr(ZeusValue* out, const char* text);
void zeus_value_from_string_data(ZeusValue* out, const char* data, int64_t length);
void zeus_value_from_string_view(ZeusValue* out, const ZeusStringView* view);
void zeus_value_from_bytes_view(ZeusValue* out, const ZeusBytesView* view);
void zeus_value_from_bytes(ZeusValue* out, const uint8_t* data, int64_t length);
int64_t zeus_bytes_get(const ZeusBytesView* bytes, int64_t index);
int64_t zeus_bytes_len(const ZeusBytesView* bytes);
const ZeusBytesView* zeus_bytes_slice(
    const ZeusBytesView* bytes,
    int64_t start,
    int64_t end
);
const ZeusStringView* zeus_bytes_decode(
    const ZeusBytesView* bytes,
    const ZeusStringView* encoding
);
void zeus_bytes_to_list(ZeusValue* out, const ZeusBytesView* bytes);
int64_t zeus_string_length_view(const ZeusStringView* value);
const ZeusStringView* zeus_string_get_view(const ZeusStringView* value, int64_t index);
const ZeusStringView* zeus_string_slice_view(
    const ZeusStringView* value,
    int64_t start,
    int64_t end
);
int64_t zeus_string_index_view(
    const ZeusStringView* value,
    const ZeusStringView* substring
);
int64_t zeus_string_rindex_view(
    const ZeusStringView* value,
    const ZeusStringView* substring
);
const ZeusStringView* zeus_string_replace_view(
    const ZeusStringView* value,
    const ZeusStringView* old_value,
    const ZeusStringView* replacement
);
bool zeus_string_startswith_view(
    const ZeusStringView* value,
    const ZeusStringView* prefix
);
bool zeus_string_endswith_view(
    const ZeusStringView* value,
    const ZeusStringView* suffix
);
const ZeusStringView* zeus_string_lower_view(const ZeusStringView* value);
const ZeusStringView* zeus_string_upper_view(const ZeusStringView* value);
int64_t zeus_string_count_view(
    const ZeusStringView* value,
    const ZeusStringView* substring
);
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
void zeus_global_clear(int idx);
void zeus_value_global_delete(int idx);
void zeus_value_local_store(int idx, ZeusValue* value);
void zeus_value_local_load(ZeusValue* out, int idx);
void zeus_value_local_delete(int idx);
void zeus_value_delete_object(ZeusValue* value);
void zeus_shared_value_store(const char* name, const ZeusValue* value);
void zeus_shared_value_assign(const char* name, const ZeusValue* value);
void zeus_shared_value_load(ZeusValue* out, const char* name);
int32_t zeus_shared_value_delete(const char* name);
// Scalar fast path for the same global slots. A top-level loop that bumps a
// scalar global uses these instead of boxing a ZeusValue per access; a type
// mismatch aborts rather than reinterpreting the stored value.
void zeus_global_store_i64(int idx, int64_t value);
int64_t zeus_global_load_i64(int idx);
void zeus_global_store_bool(int idx, int32_t value);
int32_t zeus_global_load_bool(int idx);
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
