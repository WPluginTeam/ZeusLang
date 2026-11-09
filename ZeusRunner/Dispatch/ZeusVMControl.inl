// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_PUSH(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::PUSH: {
                    if (!instr.args.empty() && std::holds_alternative<IRType::NullLiteral>(instr.args[0])) {
                        stack.push_back(ZeusType::Variant(ZeusType::Null()));
                    } else if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                        const auto &val = std::get<LexerType::Value>(instr.args[0]);
                        stack.push_back(val);
                    } else if (!instr.args.empty() && std::holds_alternative<ZeusType::Bytes>(instr.args[0])) {
                        // Bytes 字面量：包装到 Variant 后推入栈
                        ZeusType::Variant bytesVar = std::get<ZeusType::Bytes>(instr.args[0]);
                        stack.push_back(bytesVar);
                    } else {
                        // 直接将原始 Value 推入栈
                        stack.push_back(std::get<LexerType::Value>(instr.args[0]));
                    }
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_PUSH_NAMED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::PUSH_NAMED: {
                // PUSH_NAMED: 将栈顶的值包装成 NamedArg(命名参数)
                // args[0] = 参数名(string)
                if (stack.empty()) {
                    VMException::VMException exp("PUSH_NAMED: stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string paramName;
                if (!instr.args.empty() && std::holds_alternative<std::string>(instr.args[0])) {
                    paramName = std::get<std::string>(instr.args[0]);
                } else {
                    VMException::VMException exp("PUSH_NAMED: missing parameter name", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 从栈顶取值
                auto value = stack.back();
                stack.pop_back();

                // 创建 NamedArg 包装
                auto namedArg = std::make_shared<ZeusType::NamedArg>();
                namedArg->name = paramName;
                namedArg->value = value;

                // 将包装后的命名参数推回栈(占用一个栈位置)
                stack.push_back(namedArg);

                ZEUS_DEBUG_PRINT("[VM] PUSH_NAMED: wrapped '%s'", paramName.c_str());
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_FUNC_BEGIN(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::FUNC_BEGIN: {
                // FUNC_BEGIN 占位（无实际操作）
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_IR_ANNOTATION(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::IR_ANNOTATION: {
                // 语义标注只供 LLVM 等后端读取，动态 VM 不产生任何栈副作用。
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_FUNC_END(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::FUNC_END: {

                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_POP(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::POP: {

                if (!stack.empty()) {
                    dropStackBack();
                }

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_SWAP(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::SWAP: {
                // 交换栈顶两个元素
                if (stack.size() < 2) {
                    VMException::VMException exp("SWAP requires at least 2 elements on stack", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto top = stack.back();
                stack.pop_back();
                auto second = stack.back();
                stack.pop_back();

                stack.push_back(top);    // 原栈顶到第二位
                stack.push_back(second); // 原第二位到栈顶

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_JUMP(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::JUMP: {
                // 单个 execute 中不处理 PC，PC 的整体控制由 runChunk 负责
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_JUMP_IF_TRUE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::JUMP_IF_TRUE: {
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_JUMP_IF_FALSE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::JUMP_IF_FALSE: {
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_JUMP_IF_PARAM_PROVIDED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::JUMP_IF_PARAM_PROVIDED: {
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_IMPORT(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::IMPORT: {
                // IMPORT: 导入模块
                // 参数: [packageName(Value string)]

                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("IMPORT requires package name", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                const auto& val = std::get<LexerType::Value>(instr.args[0]);

                if (!std::holds_alternative<std::string>(val.data)) {
                    VMException::VMException exp("IMPORT package name must be string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string packageName = std::get<std::string>(val.data);

                vmMessage("[IMPORT] Loading package: " + packageName);

                // TODO: 实现实际的包加载逻辑
                // 1. 使用IRImport::loadPackage获取包文件路径
                // 2. 编译包文件为IR
                // 3. 执行包的IR代码
                // 4. 注册包的导出符号到命名空间

                vmMessage("[IMPORT] Package loaded (placeholder): " + packageName);

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_TRY_BEGIN(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::TRY_BEGIN: {
                // TRY_BEGIN catchOffset: 注册异常处理器
                // catchOffset 是从当前位置到 catch 块的偏移量
                if (instr.args.empty()) {
                    VMException::VMException exp("TRY_BEGIN missing catchOffset", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int64_t catchOffset = 0;
                if (std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    catchOffset = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }

                // 获取当前 PC（需要从调用栈帧获取）
                size_t currentPc = 0;
                const std::vector<IRType::IRInstr>* currentCode = nullptr;
                if (!callFrames.empty()) {
                    currentPc = callFrames.back().pc;
                    currentCode = callFrames.back().code;
                }

                // 注册异常处理器
                ExceptionHandler handler;
                handler.catchPc = currentPc + catchOffset;  // catch 块的起始位置
                handler.callFrameDepth = callFrames.size();
                handler.code = currentCode;
                handler.stackBase = stack.size();
                handler.localSize = local.size();
                handler.withDepth = withContextStack.size();
                exceptionHandlers.push_back(handler);

                ZEUS_DEBUG_PRINT("[VM] TRY_BEGIN: registered handler, catchPc=%zu, depth=%zu",
                    handler.catchPc, handler.callFrameDepth);
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_TRY_END(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::TRY_END: {
                // TRY_END: try 块正常完成，移除异常处理器
                if (!exceptionHandlers.empty()) {
                    exceptionHandlers.pop_back();
                    ZEUS_DEBUG_PRINT("[VM] TRY_END: removed handler, remaining=%zu", exceptionHandlers.size());
                }
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CATCH_BEGIN(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CATCH_BEGIN: {
                // CATCH_BEGIN "varName" slotIndex: catch 块开始
                // 如果有变量名和槽索引，将当前异常存储到该槽位
                std::string varName;
                int slotIndex = -1;

                if (!instr.args.empty() && std::holds_alternative<std::string>(instr.args[0])) {
                    varName = std::get<std::string>(instr.args[0]);
                }
                if (instr.args.size() > 1 && std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    slotIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));
                }

                if (!varName.empty() && slotIndex >= 0 && hasUnhandledException) {
                    // 确保 local 足够大
                    while (local.size() <= (size_t)slotIndex) {
                        local.push_back(nullptr);
                    }

                    // 如果该槽已有值，先释放
                    if (local[slotIndex]) {
                        releaseCellAndContent(local[slotIndex]);
                    }

                    // 在指定槽位存储异常
                    Cell* excCell = newCell(currentException, 0, "catch_var");
                    local[slotIndex] = excCell;
                    ZEUS_DEBUG_PRINT("[VM] CATCH_BEGIN: stored exception to '%s', local[%d]",
                        varName.c_str(), slotIndex);
                }

                hasUnhandledException = false;
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CATCH_END(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CATCH_END: {
                // CATCH_END: catch 块结束
                ZEUS_DEBUG_PRINT("[VM] CATCH_END");
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_THROW(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::THROW: {
                // THROW: 抛出异常（栈顶是异常对象）
                if (stack.empty()) {
                    VMException::VMException exp("THROW: stack is empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                currentException = stack.back();
                stack.pop_back();
                hasUnhandledException = true;

                ZEUS_DEBUG_PRINT("[VM] THROW: exception raised");

                vmstat = ZeusType::VMStatus::Error;
                if (exceptionHandlers.empty()) {
                    std::string errorMsg = "Unhandled exception";
                    if (std::holds_alternative<ZeusType::ObjectHandle>(currentException)) {
                        auto& oh = std::get<ZeusType::ObjectHandle>(currentException);
                        auto* obj = getValidObject(oh);
                        if (obj) {
                            // 尝试获取 _message 字段
                            if (hasObjectField(*obj, "_message")) {
                                auto message = readObjectField(*obj, "_message");
                                if (std::holds_alternative<std::string>(message)) {
                                    errorMsg = std::get<std::string>(message);
                                }
                            }
                            // 尝试获取 _type 字段
                            if (hasObjectField(*obj, "_type")) {
                                auto type = readObjectField(*obj, "_type");
                                if (std::holds_alternative<std::string>(type)) {
                                    errorMsg = std::get<std::string>(type) + ": " + errorMsg;
                                }
                            }
                        }
                    } else if (std::holds_alternative<std::string>(currentException)) {
                        errorMsg = std::get<std::string>(currentException);
                    } else if (std::holds_alternative<LexerType::Value>(currentException)) {
                        auto& val = std::get<LexerType::Value>(currentException);
                        if (std::holds_alternative<std::string>(val.data)) {
                            errorMsg = std::get<std::string>(val.data);
                        }
                    }

                    VMException::VMException exp("Uncaught exception: " + errorMsg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                }
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_HOTSPOT(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::HOTSPOT: {
                // HOTSPOT: 热点触发指令，根据第一个参数决定要执行的热热点 ID
                // 支持两种参数布局：
                //  - HotspotCompiler 生成的参数序列: [id(Value int), pushValue(Value), idx(Value int), mutOrConst(string), name?]
                //  - 返回/写入形式: [idx(Value int), mutOrConst(string), name?]（用于读/写操作）
                int hotId = -1;
                if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0]) &&
                    std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[0]).data)) {
                    hotId = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }

                // 构造 HotspotContext（封装 VM 状态以供热点处理器使用）
                HotspotContext hctx{
                    /*stack*/   this->stack.data,
                    /*global*/  this->global,
                    /*local*/   this->local,
                    /*upvalue*/ this->upvalue,
                    /*vmstat*/  this->vmstat,
                    /*__line*/  this->__line,
                    /*globalNames*/ this->globalNames,
                    /*nameToClosure*/ this->nameToClosure,
                    /*enableLateResolve*/ this->enableLateResolve,
                    /*newCellCb*/ [this](const ZeusType::UniversalVariant& v, uint8_t flags, const char* createdBy){ return this->newCell(v, flags, createdBy); },
                    /*assignCellValueCb*/ [this](Cell* cell, const ZeusType::UniversalVariant& v){ this->assignCellValue(cell, v); },
                    /*tryRegisterGlobalClosureCb*/ [this](int slot, const ZeusType::UniversalVariant& v){ this->tryRegisterGlobalClosure(slot, v); },
                    /*printGlobalsCb*/ [this](){ this->printGlobals(); },
                    /*printStackCb*/   [this](){ this->printStack(); }
                };
                // 处理已知的 hotspot id（hotId==0 为示例处理器）
                if (hotId == 0) {
                    // instr 可能直接携带 hotspot1 的内部参数布局，调用 hotspot1 处理
                    hotspot1(instr, hctx);
                } else {
                    // 未知的 hotspot id：仅打印调试信息并跳过
                    ZEUS_DEBUG_PRINT("[VM][HOTSPOT] unknown id=%d, noop", hotId);
                }
                break;
            }
        }
    }
