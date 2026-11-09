// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE bool tryExecuteNativeDouble(const IRType::IRInstr& instr) {
        const unsigned bits = ZeusBuiltins::nativeFloatingPointBits(instr.metadata.typeName);
        if (bits == 0) return false;
        switch (instr.op) {
            case VMOperator::ADD:
            case VMOperator::SUBTRACT:
            case VMOperator::MULTIPLY:
            case VMOperator::DIVIDE:
            case VMOperator::MODULUS:
            case VMOperator::EQUL:
            case VMOperator::NOT_EQUAL:
            case VMOperator::GREATER_THAN:
            case VMOperator::LESS_THAN:
            case VMOperator::GREATER_EQUAL:
            case VMOperator::LESS_EQUAL:
                break;
            default:
                return false;
        }
        if (stack.size() < 2) {
            VMException::VMException error("Native floating-point operator stack underflow", __line, instr);
            error._throw(vmstat, currentChunk);
            return true;
        }

        StackItem right = popStackItem();
        StackItem left = popStackItem();
        StackItemPairGuard operands(*this, std::move(left), std::move(right));

        // A binary32 value has to be computed in binary32. Evaluating it in
        // binary64 and rounding later would make the VM disagree with the
        // LLVM backend on the last bits of every float32 expression.
        if (bits == 32) {
            const float lhs = ZeusNativeFloat::require(operands.left.value);
            const float rhs = ZeusNativeFloat::require(operands.right.value);
            switch (instr.op) {
                case VMOperator::ADD:
                    stack.push_back(ZeusNativeFloat::boxed(lhs + rhs));
                    break;
                case VMOperator::SUBTRACT:
                    stack.push_back(ZeusNativeFloat::boxed(lhs - rhs));
                    break;
                case VMOperator::MULTIPLY:
                    stack.push_back(ZeusNativeFloat::boxed(lhs * rhs));
                    break;
                case VMOperator::DIVIDE:
                    stack.push_back(ZeusNativeFloat::boxed(lhs / rhs));
                    break;
                case VMOperator::MODULUS:
                    stack.push_back(ZeusNativeFloat::boxed(std::fmod(lhs, rhs)));
                    break;
                case VMOperator::EQUL:
                    stack.push_back(LexerType::Value(lhs == rhs));
                    break;
                case VMOperator::NOT_EQUAL:
                    stack.push_back(LexerType::Value(lhs != rhs));
                    break;
                case VMOperator::GREATER_THAN:
                    stack.push_back(LexerType::Value(lhs > rhs));
                    break;
                case VMOperator::LESS_THAN:
                    stack.push_back(LexerType::Value(lhs < rhs));
                    break;
                case VMOperator::GREATER_EQUAL:
                    stack.push_back(LexerType::Value(lhs >= rhs));
                    break;
                case VMOperator::LESS_EQUAL:
                    stack.push_back(LexerType::Value(lhs <= rhs));
                    break;
                default:
                    break;
            }
            return true;
        }

        const double lhs = ZeusNativeDouble::require(operands.left.value);
        const double rhs = ZeusNativeDouble::require(operands.right.value);
        switch (instr.op) {
            case VMOperator::ADD:
                stack.push_back(ZeusNativeDouble::boxed(lhs + rhs));
                break;
            case VMOperator::SUBTRACT:
                stack.push_back(ZeusNativeDouble::boxed(lhs - rhs));
                break;
            case VMOperator::MULTIPLY:
                stack.push_back(ZeusNativeDouble::boxed(lhs * rhs));
                break;
            case VMOperator::DIVIDE:
                stack.push_back(ZeusNativeDouble::boxed(lhs / rhs));
                break;
            case VMOperator::MODULUS:
                stack.push_back(ZeusNativeDouble::boxed(std::fmod(lhs, rhs)));
                break;
            case VMOperator::EQUL:
                stack.push_back(LexerType::Value(lhs == rhs));
                break;
            case VMOperator::NOT_EQUAL:
                stack.push_back(LexerType::Value(lhs != rhs));
                break;
            case VMOperator::GREATER_THAN:
                stack.push_back(LexerType::Value(lhs > rhs));
                break;
            case VMOperator::LESS_THAN:
                stack.push_back(LexerType::Value(lhs < rhs));
                break;
            case VMOperator::GREATER_EQUAL:
                stack.push_back(LexerType::Value(lhs >= rhs));
                break;
            case VMOperator::LESS_EQUAL:
                stack.push_back(LexerType::Value(lhs <= rhs));
                break;
            default:
                break;
        }
        return true;
    }
