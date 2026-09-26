// ZeusLLVM/ZeusDynamicBinaryABI.hpp

#pragma once

#include <string>

#include "../ZeusRunner/ZeusVMOperator.hpp"

namespace ZeusLLVM::DynamicBinaryABI {

inline int opcodeFor(VMOperator op) {
    switch (op) {
        case VMOperator::ADD: return 0;
        case VMOperator::SUBTRACT: return 1;
        case VMOperator::MULTIPLY: return 2;
        case VMOperator::DIVIDE: return 3;
        case VMOperator::MODULUS: return 4;
        case VMOperator::LESS_THAN: return 5;
        case VMOperator::EQUL: return 6;
        case VMOperator::NOT_EQUAL: return 7;
        case VMOperator::GREATER_THAN: return 8;
        case VMOperator::GREATER_EQUAL: return 9;
        case VMOperator::LESS_EQUAL: return 10;
        default: return -1;
    }
}

inline void appendLLVMDeclarations(std::string& out) {
    out += "declare void @zeus_value_dynamic_binary(ptr, ptr, i32, ptr, ptr)\n";
}

} // namespace ZeusLLVM::DynamicBinaryABI
