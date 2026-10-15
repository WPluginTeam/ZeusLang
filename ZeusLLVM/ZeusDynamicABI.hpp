// ZeusLLVM/ZeusDynamicABI.hpp
//
// LLVM 侧只记录动态对象 ABI 的名字和声明文本；具体 lowering 不放进
// ZeusEmitter.hpp，后续由独立的 DynamicObjectLowering 负责。

#pragma once

#include <string>

#include "../ZeusRunner/ZeusVMOperator.hpp"

namespace ZeusLLVM::DynamicABI {

inline bool isDynamicOperator(VMOperator op) {
    switch (op) {
        case VMOperator::LIST_BUILD:
        case VMOperator::LIST_LENGTH:
        case VMOperator::LIST_LOAD_ELEM:
        case VMOperator::LIST_STORE_ELEM:
        case VMOperator::DICT_BUILD:
        case VMOperator::DICT_KEYS_BUILD:
        case VMOperator::DICT_LOAD_VALUE:
        case VMOperator::DICT_STORE_VALUE:
            return true;
        default:
            return false;
    }
}

inline void appendLLVMDeclarations(std::string& out) {
    out += "declare void @zeus_value_from_class_handle(ptr, i32)\n";
    out += "declare i32 @zeus_value_require_class_handle(ptr, i32)\n";
    out += "declare ptr @zeus_lvalue_register(ptr, ptr, i32, i64, i32)\n";
    out += "declare void @zeus_lvalue_register_raw(ptr, i64, i32)\n";
    out += "declare void @zeus_lvalue_register_raw_owned(ptr, ptr, i64, i32)\n";
    out += "declare void @zeus_lvalue_register_native_buffer(ptr, ptr, ptr)\n";
    out += "declare void @zeus_lvalue_unregister_revealed(ptr)\n";
    out += "declare void @zeus_lvalue_unregister(ptr)\n";
    out += "declare void @zeus_lvalue_require_raw(ptr, i64, i32)\n";
    out += "declare void @zeus_lvalue_load(ptr, ptr, ptr)\n";
    out += "declare void @zeus_lvalue_store(ptr, ptr, ptr)\n";
    out += "declare void @zeus_lvalue_register_revealed(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_shared_value_slot_address(ptr, ptr, i32)\n";
    out += "declare ptr @zeus_class_copy_owned(ptr, i32)\n";
    out += "declare void @zeus_value_copy_class_owned(ptr, ptr, i32)\n";
    out += "declare void @zeus_class_copy_remember_allocated(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_class_copy_begin()\n";
    out += "declare ptr @zeus_class_copy_find(ptr, ptr)\n";
    out += "declare void @zeus_class_copy_remember(ptr, ptr, ptr)\n";
    out += "declare void @zeus_class_copy_end(ptr)\n";
    out += "declare void @zeus_class_copy_register(ptr, ptr)\n";
    out += "declare void @zeus_class_copy_register_default(ptr, ptr)\n";
    out += "declare ptr @zeus_class_copy_default_object(ptr, ptr)\n";
    out += "declare void @zeus_value_copy_class_graph_default(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_copy_class_graph_internal(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_class_copy_object(ptr, ptr)\n";
    out += "declare void @zeus_value_copy_class_graph(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_dynamic_field_copy_state(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_array_build(i64, ptr)\n";
    out += "declare ptr @zeus_dict_build(i64, ptr)\n";
    out += "declare i64 @zeus_array_length(ptr)\n";
    out += "declare ptr @zeus_array_load(ptr, i64)\n";
    out += "declare ptr @zeus_array_store(ptr, i64, ptr)\n";
    out += "declare ptr @zeus_dict_load(ptr, ptr)\n";
    out += "declare ptr @zeus_dict_store(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_dict_keys(ptr)\n";
    out += "declare ptr @zeus_array_iter_begin(ptr)\n";
    out += "declare i32 @zeus_array_iter_next(ptr, ptr)\n";
    out += "declare ptr @zeus_dict_iter_begin(ptr)\n";
    out += "declare i32 @zeus_dict_iter_next(ptr, ptr, ptr)\n";
    out += "declare void @zeus_dynamic_iter_release(ptr)\n";
    out += "declare void @zeus_value_array_build(ptr, i64, ptr)\n";
    out += "declare void @zeus_value_dict_build(ptr, i64, ptr)\n";
    out += "declare void @zeus_value_array_length(ptr, ptr)\n";
    out += "declare void @zeus_value_array_load(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_array_load_checked(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_array_load_i64(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_array_load_i64_checked(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_array_store(ptr, ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_array_store_i64(ptr, ptr, i64, ptr)\n";
    out += "declare void @zeus_value_dict_keys(ptr, ptr)\n";
    out += "declare void @zeus_value_dict_load(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_dict_store(ptr, ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_subscript_load(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_subscript_store(ptr, ptr, ptr, ptr)\n";
}

} // namespace ZeusLLVM::DynamicABI
