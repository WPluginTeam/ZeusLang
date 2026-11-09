// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_ADD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::ADD: {
                // 1. 类型/数量检查

                if (stack.size() < 2) {
                    VMException::VMException exp("ADD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右操作数和左操作数（右操作数在栈顶）
                // P1：move 替代拷贝（string/Array 等大 value 省一次深拷贝+引用计数）
                ZeusType::UniversalVariant right = std::move(stack.back());
                stack.pop_back();

                ZeusType::UniversalVariant left  = std::move(stack.back());
                stack.pop_back();

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[+]", operatorArguments, 2, instr, nextInstruction)) break;

                // P0 fast path: int + int（覆盖 x=x+1 与 for 计数器自增）
                // 跳过 operator重载 / Bytes / convertToValue / getStringType 全套慢路径
                {
                    int64_t li = 0, ri = 0;

                    if (ZeusVMUtils::tryGetInt64(left, li) && ZeusVMUtils::tryGetInt64(right, ri)) {
                        bool overflow = false;

                        if (ri > 0) overflow = (li > INT64_MAX - ri);
                        else if (ri < 0) overflow = (li < INT64_MIN - ri);

                        if (!overflow) {
                            stack.push_back(LexerType::Value(li + ri));
                            break;
                        }
                        // 溢出回退慢路径（大整数加法，极少发生）
                    }
                }

                // 2.5 检查左操作数是否为对象，且是否有operator[+]重载
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[+]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                // 找到operator[+]重载，调用它
                                int protoId = opIt->second;
                                ZEUS_DEBUG_PRINT("[VM] Calling operator[+] overload (protoId=%d) for object %d", protoId, objectHandle.id);

                                // 创建临时闭包
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));

                                // 栈布局: [closure, this(left), arg(right)]
                                // 对于 func operator[+] (other) [&this]:
                                //   - local 0 = this (来自捕获列表,但作为参数传递)
                                //   - local 1 = other (显式参数)
                                stack.push_back(ZeusType::ClosureHandle{cloId}); // callee
                                stack.push_back(left);  // this -> local 0
                                stack.push_back(right); // other -> local 1

                                // 调用: argc=2 (this + other)
                                // Dispatch through a real frame so the callee gets its own local
                                // slots instead of sharing this frame's local buffer.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                // 结果已在栈顶，直接break
                                vmMessage(">>> Operator[+] overload called.");
                                break;
                            }
                        }
                    }
                }

                // 3. 特殊处理: Bytes + Bytes
                // Bytes可能以两种形式存在：直接Bytes或Variant包装的Bytes
                const ZeusType::Bytes* leftBytes = nullptr;
                const ZeusType::Bytes* rightBytes = nullptr;

                // 尝试直接获取Bytes
                if (std::holds_alternative<ZeusType::Bytes>(left)) {
                    leftBytes = &std::get<ZeusType::Bytes>(left);
                } else if (std::holds_alternative<ZeusType::Variant>(left)) {
                    const auto& leftVar = std::get<ZeusType::Variant>(left);
                    if (std::holds_alternative<ZeusType::Bytes>(leftVar)) {
                        leftBytes = &std::get<ZeusType::Bytes>(leftVar);
                    }
                }

                if (std::holds_alternative<ZeusType::Bytes>(right)) {
                    rightBytes = &std::get<ZeusType::Bytes>(right);
                } else if (std::holds_alternative<ZeusType::Variant>(right)) {
                    const auto& rightVar = std::get<ZeusType::Variant>(right);
                    if (std::holds_alternative<ZeusType::Bytes>(rightVar)) {
                        rightBytes = &std::get<ZeusType::Bytes>(rightVar);
                    }
                }

                if (leftBytes && rightBytes) {
                    // 合并两个bytes
                    ZeusType::Bytes result(leftBytes->data);
                    result.data.insert(result.data.end(), rightBytes->data.begin(), rightBytes->data.end());

                    stack.push_back(result);
                    vmMessage(">>> Exec add (bytes).");
                    printStack();
                    break;
                }

                // 4. 没有重载，使用默认实现：转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);

                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("ADD operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 根据类型判断是数字加法还是字符串拼接
                bool numPair = ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv);
                bool strPair = (lv.getStringType() == "string" && rv.getStringType() == "string");

                if (!numPair && !strPair) {
                    VMException::VMException exp("ADD expects numeric+numeric or string+string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 推入计算结果
                stack.push_back(lv + rv);
                vmMessage(">>> Exec add.");
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_SUBTRACT(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::SUBTRACT: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("SUBTRACT stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[-]", operatorArguments, 2, instr, nextInstruction)) break;

                // 3. 检查左操作数是否为对象（支持operator[-]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[-]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                ZEUS_DEBUG_PRINT("[VM] Calling operator- overload (protoId=%d) for object %d", protoId, objectHandle.id);

                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));

                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);

                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator- overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("SUBTRACT operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 验证操作数为数值类型
                if (!(ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv))) {
                    VMException::VMException exp("SUBTRACT operands must be numeric", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 执行减法并推入结果
                stack.push_back(lv - rv);

                vmMessage(">>> Exec subtract.");
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_MULTIPLY(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::MULTIPLY: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("MULTIPLY stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[*]", operatorArguments, 2, instr, nextInstruction)) break;

                // 3. 检查左操作数是否为对象（支持operator[*]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[*]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                ZEUS_DEBUG_PRINT("[VM] Calling operator* overload (protoId=%d) for object %d", protoId, objectHandle.id);

                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));

                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);

                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator* overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("MULTIPLY operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 验证操作数为数值类型
                if (!(ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv))) {
                    VMException::VMException exp("MULTIPLY operands must be numeric", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 执行乘法并推入结果
                stack.push_back(lv * rv);

                vmMessage(">>> Exec multiply.");
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_DIVIDE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::DIVIDE: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("DIVIDE stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[/]", operatorArguments, 2, instr, nextInstruction)) break;

                // 3. 检查左操作数是否为对象（支持operator[/]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[/]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator/ overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("DIVIDE operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 验证操作数为数值类型
                if (!(ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv))) {
                    VMException::VMException exp("DIVIDE operands must be numeric", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 检查除数是否为零（避免除零错误）
                ZeusType::BigDecimal rnum(0);
                if (ZeusVMUtils::extractNumber(right, rnum) && rnum == ZeusType::BigDecimal(0)) {
                    VMException::VMException exp("DIVIDE by zero", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 7. 执行除法并推入结果
                stack.push_back(lv / rv);

                vmMessage(">>> Exec divide.");
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_MODULUS(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::MODULUS: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("MODULUS stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left = stack.back();
                stack.pop_back();

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[%]", operatorArguments, 2, instr, nextInstruction)) break;

                // 3. 检查左操作数是否为对象（支持operator[%]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[%]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator% overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("MODULUS operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 确认为整数类型（int % int）
                if (!(lv.getStringType() == "int" && rv.getStringType() == "int")) {
                    VMException::VMException exp("MODULUS expects int % int", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                const ZeusType::BigInteger& li = std::get<ZeusType::BigInteger>(lv.data);
                const ZeusType::BigInteger& ri = std::get<ZeusType::BigInteger>(rv.data);

                if (ri == ZeusType::BigInteger(0)) {
                    VMException::VMException exp("MODULUS by zero", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::BigInteger res = li % ri;
                stack.push_back(LexerType::Value(res));
                vmMessage(">>> Exec modulus.");
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_EQUL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::EQUL: {
                // 1. 检查栈中是否有足够的元素
                if (stack.size() < 2) {
                    VMException::VMException exp("EQUL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出并保留所有权标记，比较完成后由 guard 释放。
                StackItem rightItem = popStackItem();
                StackItem leftItem = popStackItem();
                StackItemPairGuard operands(*this, std::move(leftItem),
                                            std::move(rightItem));
                const ZeusType::UniversalVariant& left = operands.left.value;
                const ZeusType::UniversalVariant& right = operands.right.value;

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[==]", operatorArguments, 2, instr, nextInstruction)) break;

                // 2.5 处理 nil 比较: nil == nil 为 true，nil == 非nil 为 false
                bool leftIsNull = ZeusVMUtils::isNull(left);
                bool rightIsNull = ZeusVMUtils::isNull(right);

                if (leftIsNull || rightIsNull) {
                    bool result = (leftIsNull && rightIsNull);
                    stack.push_back(LexerType::Value(result));
                    printStack();
                    break;
                }

                // 3. 检查左操作数是否为对象（支持operator[==]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[==]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                operands.transferToStack();
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator== overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 处理闭包和原生函数句柄的相等性比较
                if (std::holds_alternative<ZeusType::ClosureHandle>(left) &&
                    std::holds_alternative<ZeusType::ClosureHandle>(right)) {
                    bool r = std::get<ZeusType::ClosureHandle>(left).id ==
                              std::get<ZeusType::ClosureHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }
                if (std::holds_alternative<ZeusType::NativeFuncHandle>(left) &&
                    std::holds_alternative<ZeusType::NativeFuncHandle>(right)) {
                    bool r = std::get<ZeusType::NativeFuncHandle>(left).id ==
                              std::get<ZeusType::NativeFuncHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }

                // 5. 将任意值转换为内部可比较的 LexerType::Value（如果可能）
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!(ZeusVMUtils::convertToValue(left, lv) && ZeusVMUtils::convertToValue(right, rv))) {
                    // 无法转换为可比较的值或类型不一致 -> 视为不相等
                    stack.push_back(LexerType::Value(false));
                    printStack();
                    break;
                }

                // 6. 根据类型选择比较策略（数值/字符串/布尔等）
                std::string lt = lv.getStringType();
                std::string rt = rv.getStringType();

                auto isNum = [](const std::string &s) {
                    return s == "int" || s == "float";
                };

                bool result = false;
                if (isNum(lt) && isNum(rt)) {
                    ZeusType::BigDecimal lf = (lt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(lv.data))
                                              : std::get<ZeusType::BigDecimal>(lv.data);
                    ZeusType::BigDecimal rf = (rt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(rv.data))
                                              : std::get<ZeusType::BigDecimal>(rv.data);
                    result = (lf == rf);
                } else if (lt == rt) {
                    if (lt == "string")       result = std::get<std::string>(lv.data) == std::get<std::string>(rv.data);
                    else if (lt == "bool")    result = std::get<bool>(lv.data)        == std::get<bool>(rv.data);
                    else if (lt == "int")     result = std::get<ZeusType::BigInteger>(lv.data) == std::get<ZeusType::BigInteger>(rv.data);
                    else if (lt == "float")   result = std::get<ZeusType::BigDecimal>(lv.data)       == std::get<ZeusType::BigDecimal>(rv.data);
                }

                // 7. 推入比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_NOT_EQUAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::NOT_EQUAL: {
                // 1. 前置检查：栈元素数量是否足够
                if (stack.size() < 2) {
                    VMException::VMException exp("NOT_EQUAL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出并保留所有权标记，比较完成后由 guard 释放。
                StackItem rightItem = popStackItem();
                StackItem leftItem = popStackItem();
                StackItemPairGuard operands(*this, std::move(leftItem),
                                            std::move(rightItem));
                const ZeusType::UniversalVariant& left = operands.left.value;
                const ZeusType::UniversalVariant& right = operands.right.value;

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[!=]", operatorArguments, 2, instr, nextInstruction)) break;

                // 2.5 处理 nil 比较: nil != nil 为 false，nil != 非nil 为 true
                bool leftIsNull = ZeusVMUtils::isNull(left);
                bool rightIsNull = ZeusVMUtils::isNull(right);

                if (leftIsNull || rightIsNull) {
                    bool result = !(leftIsNull && rightIsNull);
                    stack.push_back(LexerType::Value(result));
                    printStack();
                    break;
                }

                // 3. 检查左操作数是否为对象（支持operator[!=]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[!=]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                operands.transferToStack();
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator!= overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 处理闭包/原生函数句柄的特殊不等比较
                if (std::holds_alternative<ZeusType::ClosureHandle>(left) &&
                    std::holds_alternative<ZeusType::ClosureHandle>(right)) {
                    bool r = std::get<ZeusType::ClosureHandle>(left).id !=
                              std::get<ZeusType::ClosureHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }
                if (std::holds_alternative<ZeusType::NativeFuncHandle>(left) &&
                    std::holds_alternative<ZeusType::NativeFuncHandle>(right)) {
                    bool r = std::get<ZeusType::NativeFuncHandle>(left).id !=
                              std::get<ZeusType::NativeFuncHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }

                // 5. 尝试将值转换为内部可比较形式
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!(ZeusVMUtils::convertToValue(left, lv) && ZeusVMUtils::convertToValue(right, rv))) {
                    // 无法转换为可比较值 -> 视为不相等
                    stack.push_back(LexerType::Value(true));
                    printStack();
                    break;
                }

                // 6. 根据类型决定比较逻辑
                std::string lt = lv.getStringType();
                std::string rt = rv.getStringType();
                auto isNum = [](const std::string &s) { return s == "int" || s == "float"; };

                bool result = true; // 默认认为不相等
                if (isNum(lt) && isNum(rt)) {
                    ZeusType::BigDecimal lf = (lt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(lv.data))
                                              : std::get<ZeusType::BigDecimal>(lv.data);
                    ZeusType::BigDecimal rf = (rt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(rv.data))
                                              : std::get<ZeusType::BigDecimal>(rv.data);
                    result = (lf != rf);
                } else if (lt == rt) {
                    if (lt == "string")       result = std::get<std::string>(lv.data) != std::get<std::string>(rv.data);
                    else if (lt == "bool")    result = std::get<bool>(lv.data)        != std::get<bool>(rv.data);
                    else if (lt == "int")     result = std::get<ZeusType::BigInteger>(lv.data) != std::get<ZeusType::BigInteger>(rv.data);
                    else if (lt == "float")   result = std::get<ZeusType::BigDecimal>(lv.data)       != std::get<ZeusType::BigDecimal>(rv.data);
                    else                       result = true;
                } else {
                    result = true; // 不同类型 -> 视为不相等
                }

                // 7. 推入不等比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_GREATER_THAN(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::GREATER_THAN: {
                // 1. 前置检查：栈元素数量是否足够
                if (stack.size() < 2) {
                    VMException::VMException exp("GREATER_THAN stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem rightItem = popStackItem();
                StackItem leftItem = popStackItem();
                StackItemPairGuard operands(*this, std::move(leftItem),
                                            std::move(rightItem));
                const ZeusType::UniversalVariant& left = operands.left.value;
                const ZeusType::UniversalVariant& right = operands.right.value;

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[>]", operatorArguments, 2, instr, nextInstruction)) break;

                // 3. 检查左操作数是否为对象（支持operator[>]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[>]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                operands.transferToStack();
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator> overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 尝试提取数值（用于数值比较）
                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okLeft  = ZeusVMUtils::extractNumber(left,  a);
                bool okRight = ZeusVMUtils::extractNumber(right, b);

                // 5. 如果两侧均为数值，则进行数值比较
                bool result = false;
                if (okLeft && okRight) {
                    result = (a > b);
                }

                // 6. 推入比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LESS_THAN(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LESS_THAN: {
                // 1. 检查栈是否至少有两个值
                if (stack.size() < 2) {
                    VMException::VMException exp("LESS_THAN stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出并保留所有权标记，比较完成后由 guard 释放。
                StackItem rightItem = popStackItem();
                StackItem leftItem = popStackItem();
                StackItemPairGuard operands(*this, std::move(leftItem),
                                            std::move(rightItem));
                const ZeusType::UniversalVariant& left = operands.left.value;
                const ZeusType::UniversalVariant& right = operands.right.value;

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[<]", operatorArguments, 2, instr, nextInstruction)) break;

                // P0 fast path: int < int（覆盖 for 循环条件）
                // 跳过 BigDecimal(0) 构造 + extractNumber + BigDecimal 比较全套
                {
                    int64_t li = 0, ri = 0;

                    if (ZeusVMUtils::tryGetInt64(left, li) && ZeusVMUtils::tryGetInt64(right, ri)) {
                        stack.push_back(LexerType::Value(li < ri));
                        printStack();
                        break;
                    }
                }

                // 4. 检查左操作数是否为对象（支持operator[<]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[<]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                operands.transferToStack();
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator< overload called.");
                                break;
                            }
                        }
                    }
                }

                // 5. 尝试将两侧转换为数值并比较
                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okLeft  = ZeusVMUtils::extractNumber(left,  a);
                bool okRight = ZeusVMUtils::extractNumber(right, b);

                bool result = false;
                if (okLeft && okRight) {
                    result = (a < b);
                }

                // 6. 推入比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_GREATER_EQUAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::GREATER_EQUAL: {
                if (stack.size() < 2) {
                    VMException::VMException exp("GREATER_EQUAL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                StackItem rightItem = popStackItem();
                StackItem leftItem = popStackItem();
                StackItemPairGuard operands(*this, std::move(leftItem),
                                            std::move(rightItem));
                const ZeusType::UniversalVariant& left = operands.left.value;
                const ZeusType::UniversalVariant& right = operands.right.value;

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[>=]", operatorArguments, 2, instr, nextInstruction)) break;

                // 检查左操作数是否为对象（支持operator[>=]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[>=]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                operands.transferToStack();
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator>= overload called.");
                                break;
                            }
                        }
                    }
                }

                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okL = ZeusVMUtils::extractNumber(left,  a);
                bool okR = ZeusVMUtils::extractNumber(right, b);

                bool res = false;
                if (okL && okR) {
                    res = (a >= b);
                }
                stack.push_back(LexerType::Value(res));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LESS_EQUAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LESS_EQUAL: {
                if (stack.size() < 2) {
                    VMException::VMException exp("LESS_EQUAL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                StackItem rightItem = popStackItem();
                StackItem leftItem = popStackItem();
                StackItemPairGuard operands(*this, std::move(leftItem),
                                            std::move(rightItem));
                const ZeusType::UniversalVariant& left = operands.left.value;
                const ZeusType::UniversalVariant& right = operands.right.value;

                const ZeusType::UniversalVariant* operatorArguments[] = { &left, &right };
                if (tryDispatchTypeImplementationOperator(
                        "[<=]", operatorArguments, 2, instr, nextInstruction)) break;

                // 检查左操作数是否为对象（支持operator[<=]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(left);
                    auto* objPtr = getValidObject(objectHandle);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[<=]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                operands.transferToStack();
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator<= overload called.");
                                break;
                            }
                        }
                    }
                }

                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okL = ZeusVMUtils::extractNumber(left,  a);
                bool okR = ZeusVMUtils::extractNumber(right, b);

                bool res = false;
                if (okL && okR) {
                    res = (a <= b);
                }
                stack.push_back(LexerType::Value(res));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOGICAL_AND(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOGICAL_AND: {
                // 逻辑与运算: left and right
                if (stack.size() < 2) {
                    VMException::VMException exp("LOGICAL_AND stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto right = stack.back();
                stack.pop_back();
                auto left = stack.back();
                stack.pop_back();

                // 将左右操作数转换为布尔值
                bool leftBool = ZeusVMUtils::toBool(left);
                bool rightBool = ZeusVMUtils::toBool(right);

                bool result = leftBool && rightBool;
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOGICAL_OR(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOGICAL_OR: {
                // 逻辑或运算: left or right
                if (stack.size() < 2) {
                    VMException::VMException exp("LOGICAL_OR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto right = stack.back();
                stack.pop_back();
                auto left = stack.back();
                stack.pop_back();

                // 将左右操作数转换为布尔值
                bool leftBool = ZeusVMUtils::toBool(left);
                bool rightBool = ZeusVMUtils::toBool(right);

                bool result = leftBool || rightBool;
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOGICAL_NOT(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOGICAL_NOT: {
                if (stack.empty()) {
                    VMException::VMException exp("LOGICAL_NOT stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                const auto operand = stack.back();
                stack.pop_back();
                stack.push_back(LexerType::Value(!ZeusVMUtils::toBool(operand)));
                printStack();
                break;
            }
        }
    }
