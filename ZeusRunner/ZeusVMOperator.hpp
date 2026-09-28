
/*
    ## ADD
        Stack [1, 2]
            -> ADD
        STACK [3]

    ## SUBTRACT
        Stack [1, 2]
            -> SUBTRACT
        STACK [-1]

    ## MULTIPLY
        Stack [3, 4]
            -> MULTIPLY
        STACK [12]

    ## DIVIDE
        Stack [12, 3]
            -> DIVIDE
        STACK [4]
    
    ## MODULUS
        Stack [10, 3]
            -> MODULUS
        STACK [1]
    
    ## PUSH (CONST VALUE)
        Stack [1]
            -> PUSH 2
        STACK [1, 2]

    ## POP
        Stack [1, 2]
            -> POP
            <- 2
        STACK [1]

    ## CLEAR
        Stack [1, 2]
            -> CLEAR
        STACK []

    ## COPY
        Stack [2, 3]
            -> COPY
        Stack [2, 3, 3]

    ## SWAP
        Stack [1, 2]
            -> SWAP
        STACK [2, 1]


    ## NEW_OBJ
*/

#ifndef VMOperator_hpp
#define VMOperator_hpp

#include <string>

enum class VMOperator {
    ADD, 
    SUBTRACT,
    MULTIPLY,
    DIVIDE,

    MODULUS,

    PUSH,
    PUSH_NAMED,  // 压入命名参数: PUSH_NAMED "paramName" value
    POP,
    CLEAR,
    COPY,
    SWAP,
    
    NEW_OBJ,
    
    CALL_FUNC,
    CALL_CLOSURE,
    MAKE_FUNCTION,
    FUNC_BEGIN,
    FUNC_END,
    CAPTURE_OUTSIDE_VAR,
    RETURN,
    LOAD_NATIVE,
    CALL_NATIVE,

    STORE_VALUE_GLOBAL,
    STORE_VALUE_LOCAL,
    ASSIGN_VALUE_GLOBAL,
    ASSIGN_VALUE_LOCAL,
    ASSIGN_VALUE_UPVALUE,

    LOAD_VALUE,
    LOAD_CELL_REFERENCE,

    JUMP,
    JUMP_IF_TRUE,
    JUMP_IF_FALSE,

    EQUL,
    NOT_EQUAL,
    GREATER_THAN,
    LESS_THAN,
    GREATER_EQUAL,
    LESS_EQUAL,

    LOGICAL_AND,
    LOGICAL_OR,

    // ===== For / Collection Support (占位,后续实现) =====
    LIST_BUILD,
    LIST_LENGTH,
    LIST_LOAD_ELEM,
    LIST_STORE_ELEM,
    DICT_BUILD,
    DICT_ITER_INIT,
    DICT_ITER_NEXT,
    DICT_KEYS_BUILD,
    DICT_LOAD_VALUE,
    DICT_STORE_VALUE,
    COUNT_CHECK_NEG,
    COUNT_CHECK_STEP_ZERO,

    // ===== 面向对象 =====
    CLASS_BUILD,
    CLASS_BUILD_SHARED,   // 创建共享类定义（存储到共享堆）
    CLASS_STATIC_METHOD_CALL,
    OBJ_BUILD,
    OBJ_BUILD_SHARED,     // 创建共享对象（实例存储到共享堆）
    OBJ_LOAD_ATTR,
    OBJ_STORE_ATTR,
    OBJ_CALL_METHOD,
    
    // ===== 模块系统 =====
    IMPORT,
    NAMESPACE_BUILD,   // 创建包命名空间对象

    // ===== 共享堆 (跨 VM 共享变量) =====
    STORE_VALUE_SHARED,   // 存储到共享堆：STORE_VALUE_SHARED "name" "mut/const"
    LOAD_VALUE_SHARED,    // 从共享堆加载：LOAD_VALUE_SHARED "name"
    ASSIGN_VALUE_SHARED,  // 赋值到共享堆变量：ASSIGN_VALUE_SHARED "name"
    SHARED_LOCK,          // 获取共享变量写锁：SHARED_LOCK "name"
    SHARED_UNLOCK,        // 释放共享变量写锁：SHARED_UNLOCK "name"

    // ===== 循环控制 =====
    LOOP_BREAK,           // 跳出循环：LOOP_BREAK offset

    // ===== 异常处理 =====
    TRY_BEGIN,            // 开始 try 块：TRY_BEGIN catchOffset
    TRY_END,              // 结束 try 块（正常执行完毕）：TRY_END
    CATCH_BEGIN,          // 开始 catch 块：CATCH_BEGIN "varName" (空字符串表示无变量)
    CATCH_END,            // 结束 catch 块：CATCH_END
    THROW,                // 抛出异常：THROW (栈顶是异常对象)
    WITH_BEGIN,           // 开始 with 块：WITH_BEGIN (栈顶是 with 对象)
    WITH_END,             // 结束 with 块：WITH_END

    // ===== 内存管理 =====
    DELETE_VAR,           // 删除变量：DELETE_VAR "varName"

    // ===== Hotspot 特别优化 =====

    HOTSPOT,

    // 仅供后端使用的语义标注，不改变动态 VM 栈语义。
    IR_ANNOTATION,

    // Conditional branch used by function default-argument prologues.
    JUMP_IF_PARAM_PROVIDED,

    // Appended to preserve the numeric values of existing serialized operators.
    LOGICAL_NOT,

    // Registers class-style methods for an existing runtime type.
    TYPE_IMPL_BUILD
};


std::string VMO_toString(VMOperator opr) {
    switch (opr) {
        case VMOperator::ADD: return "ADD";
        case VMOperator::SUBTRACT: return "SUBTRACT";
        case VMOperator::MULTIPLY: return "MULTIPLY";
        case VMOperator::DIVIDE: return "DIVIDE";
        case VMOperator::MODULUS: return "MODULUS";
        case VMOperator::PUSH: return "PUSH";
        case VMOperator::PUSH_NAMED: return "PUSH_NAMED";
        case VMOperator::POP: return "POP";
        case VMOperator::CLEAR: return "CLEAR";
        case VMOperator::COPY: return "COPY";
        case VMOperator::SWAP: return "SWAP";
        case VMOperator::NEW_OBJ: return "NEW_OBJ";
        case VMOperator::CALL_FUNC: return "CALL_FUNC";
        case VMOperator::CALL_CLOSURE: return "CALL_CLOSURE";
        case VMOperator::MAKE_FUNCTION: return "MAKE_FUNCTION";
        case VMOperator::FUNC_BEGIN: return "FUNC_BEGIN";
        case VMOperator::FUNC_END: return "FUNC_END";
        case VMOperator::CAPTURE_OUTSIDE_VAR: return "CAPTURE_OUTSIDE_VAR";
        case VMOperator::RETURN: return "RETURN";
        case VMOperator::STORE_VALUE_GLOBAL: return "STORE_VALUE_GLOBAL";
        case VMOperator::STORE_VALUE_LOCAL: return "STORE_VALUE_LOCAL";
        case VMOperator::ASSIGN_VALUE_GLOBAL: return "ASSIGN_VALUE_GLOBAL";
        case VMOperator::ASSIGN_VALUE_LOCAL: return "ASSIGN_VALUE_LOCAL";
        case VMOperator::ASSIGN_VALUE_UPVALUE: return "ASSIGN_VALUE_UPVALUE";
        case VMOperator::LOAD_VALUE: return "LOAD_VALUE";
        case VMOperator::LOAD_CELL_REFERENCE: return "LOAD_CELL_REFERENCE";
        case VMOperator::JUMP: return "JUMP";
        case VMOperator::JUMP_IF_TRUE: return "JUMP_IF_TRUE";
        case VMOperator::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case VMOperator::LOAD_NATIVE: return "LOAD_NATIVE";
        case VMOperator::CALL_NATIVE: return "CALL_NATIVE";
        case VMOperator::EQUL: return "EQUL";
        case VMOperator::NOT_EQUAL: return "NOT_EQUAL";
        case VMOperator::GREATER_THAN: return "GREATER_THAN";
        case VMOperator::LESS_THAN: return "LESS_THAN";
        case VMOperator::GREATER_EQUAL: return "GREATER_EQUAL";
        case VMOperator::LESS_EQUAL: return "LESS_EQUAL";
        case VMOperator::LOGICAL_AND: return "LOGICAL_AND";
        case VMOperator::LOGICAL_OR: return "LOGICAL_OR";
        case VMOperator::LIST_BUILD: return "LIST_BUILD";
        case VMOperator::LIST_LENGTH: return "LIST_LENGTH";
        case VMOperator::LIST_LOAD_ELEM: return "LIST_LOAD_ELEM";
        case VMOperator::LIST_STORE_ELEM: return "LIST_STORE_ELEM";
        case VMOperator::DICT_BUILD: return "DICT_BUILD";
        case VMOperator::DICT_ITER_INIT: return "DICT_ITER_INIT";
        case VMOperator::DICT_ITER_NEXT: return "DICT_ITER_NEXT";
        case VMOperator::DICT_KEYS_BUILD: return "DICT_KEYS_BUILD";
        case VMOperator::DICT_LOAD_VALUE: return "DICT_LOAD_VALUE";
        case VMOperator::DICT_STORE_VALUE: return "DICT_STORE_VALUE";
        case VMOperator::COUNT_CHECK_NEG: return "COUNT_CHECK_NEG";
        case VMOperator::COUNT_CHECK_STEP_ZERO: return "COUNT_CHECK_STEP_ZERO";
        // 面向对象
        case VMOperator::CLASS_BUILD: return "CLASS_BUILD";
        case VMOperator::CLASS_BUILD_SHARED: return "CLASS_BUILD_SHARED";
        case VMOperator::CLASS_STATIC_METHOD_CALL: return "CLASS_STATIC_METHOD_CALL";
        case VMOperator::OBJ_BUILD: return "OBJ_BUILD";
        case VMOperator::OBJ_BUILD_SHARED: return "OBJ_BUILD_SHARED";
        case VMOperator::OBJ_LOAD_ATTR: return "OBJ_LOAD_ATTR";
        case VMOperator::OBJ_STORE_ATTR: return "OBJ_STORE_ATTR";
        case VMOperator::OBJ_CALL_METHOD: return "OBJ_CALL_METHOD";
        // 模块系统
        case VMOperator::IMPORT: return "IMPORT";
        case VMOperator::NAMESPACE_BUILD: return "NAMESPACE_BUILD";
        // 共享堆
        case VMOperator::STORE_VALUE_SHARED: return "STORE_VALUE_SHARED";
        case VMOperator::LOAD_VALUE_SHARED: return "LOAD_VALUE_SHARED";
        case VMOperator::ASSIGN_VALUE_SHARED: return "ASSIGN_VALUE_SHARED";
        case VMOperator::SHARED_LOCK: return "SHARED_LOCK";
        case VMOperator::SHARED_UNLOCK: return "SHARED_UNLOCK";
        // 循环控制
        case VMOperator::LOOP_BREAK: return "LOOP_BREAK";
        // 异常处理
        case VMOperator::TRY_BEGIN: return "TRY_BEGIN";
        case VMOperator::TRY_END: return "TRY_END";
        case VMOperator::CATCH_BEGIN: return "CATCH_BEGIN";
        case VMOperator::CATCH_END: return "CATCH_END";
        case VMOperator::THROW: return "THROW";
        case VMOperator::WITH_BEGIN: return "WITH_BEGIN";
        case VMOperator::WITH_END: return "WITH_END";
        // 内存管理
        case VMOperator::DELETE_VAR: return "DELETE_VAR";
        // Hotspot
        case VMOperator::HOTSPOT: return "HOTSPOT";
        case VMOperator::IR_ANNOTATION: return "IR_ANNOTATION";
        case VMOperator::JUMP_IF_PARAM_PROVIDED: return "JUMP_IF_PARAM_PROVIDED";
        case VMOperator::LOGICAL_NOT: return "LOGICAL_NOT";
        case VMOperator::TYPE_IMPL_BUILD: return "TYPE_IMPL_BUILD";
        default: return "UNKNOWN";
    }
}

#endif
