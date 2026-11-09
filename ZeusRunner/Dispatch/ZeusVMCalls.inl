// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_CALL_FUNC(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CALL_FUNC: {

                auto const& funcName = std::get<std::string>(instr.args[0]);
                auto const& argc = std::get<LexerType::Value>(instr.args[1]);

                // printf(">>> Call function: %s %d\r\n", funcName.c_str(), std::get<int>(argc.data));

                // now its function is replaced by CALL_CLOSURE
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_MAKE_FUNCTION(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::MAKE_FUNCTION: {
                // 参数：chunkIndex (Value int)。需要通过 chunkToProtoId 映射到实际的 protoId
                auto const& pidVal = std::get<LexerType::Value>(instr.args[0]);
                int chunkIndex = VMHelpers::getIntFromValue(pidVal);

                int protoId = -1;
                if (chunkIndex >= 0 && (size_t)chunkIndex < chunkToProtoId.size()) {
                    protoId = chunkToProtoId[chunkIndex];
                }

                const Prototype* protoPtr = getPrototype(protoId);
                if (!protoPtr) {
                    ZEUS_DEBUG_PRINT("[VM] MAKE_FUNCTION invalid chunkIndex: %d", chunkIndex);
                    break;
                }

                const Prototype& proto = *protoPtr;

                bool capturesReferenceParameter = false;
                if (!callFrames.empty()) {
                    const auto& currentFrame = callFrames.back();
                    for (const auto& capture : proto.captures) {
                        if (capture.fromKind == "local" && capture.srcIndex >= 0 &&
                            capture.srcIndex < static_cast<int>(
                                currentFrame.referenceParameterSlots.size()) &&
                            currentFrame.referenceParameterSlots[
                                static_cast<size_t>(capture.srcIndex)]) {
                            capturesReferenceParameter = true;
                            break;
                        }
                    }
                }
                if (capturesReferenceParameter) {
                    VMException::VMException exp(
                        "a by-reference parameter cannot be captured by a nested function; reference parameters are valid only for the duration of the call",
                        __line,
                        instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                Closure clos;
                clos.protoId = protoId;
                clos.upvalues.resize(proto.captures.size());

                // 捕获模式 modeFlag: 0 = 复制 (copy), 1 = 引用 (ref)
                for (size_t i = 0; i < proto.captures.size(); ++i) {
                    const auto& cap = proto.captures[i];
                    int src = cap.srcIndex;

                    bool isLocal = (cap.fromKind == "local");
                    if (isLocal) {
                        // 这里处理对 local 变量的捕获：
                        // - 如果 local 数组太短则扩展
                        // - 如果 local[src] 为空则创建一个 Cell 占位
                        // - 解释器在 MAKE_FUNCTION/STORE_VALUE_LOCAL 时会把值分配到 Cell
                        if (src >= 0 && (size_t)src >= local.size()) {
                            local.resize(src + 1, nullptr);
                        }
                        if (src < 0) {
                            VMException::VMException exp("Capture local negative index: " + std::to_string(src), __line, instr);
                            exp._throw(vmstat, currentChunk);
                            continue;
                        }
                        if (!local[src]) {
                            local[src] = newCell(LexerType::Value(0), 0, "CAPTURE_placeholder"); // 占位 Cell（nil/0）
                        }
                        // 现在 local[src] 已可安全使用

                        if (cap.modeFlag == 1) {
                            // ref-mode: store pointer and retain
                            clos.upvalues[cap.dstIndex] = local[src];
                            clos.upvalues[cap.dstIndex]->retain();
                        } else {
                            // copy-mode: snapshot the value now (template), do not point to local
                            clos.upvalues[cap.dstIndex] = cloneCell(local[src], "CAPTURE_copy");
                        }
                    } else {
                        bool inRange = (src >= 0 && (size_t)src < upvalue.size() && upvalue[src]);

                        if (!inRange) {
                            VMException::VMException exp("Capture upvalue out of range: " + std::to_string(src), __line, instr);
                            exp._throw(vmstat, currentChunk);
                            continue;
                        }

                        if (cap.modeFlag == 1) {
                            // ref-mode: share outer upvalue pointer
                            clos.upvalues[cap.dstIndex] = upvalue[src];
                            clos.upvalues[cap.dstIndex]->retain();
                        } else {
                            // copy-mode: snapshot outer upvalue now
                            clos.upvalues[cap.dstIndex] = cloneCell(upvalue[src]);
                        }
                    }
                }

                int handle = allocClosure(std::move(clos));

                    // Debug: dump closure template upvalues (debug channel)
                    {
                        ZEUS_DEBUG_PRINT("[MAKE_FUNCTION] proto=%d clos=%d upvalues=", protoId, handle);
                        for (size_t ui = 0; ui < closures[handle].upvalues.size(); ++ui) {
                            Cell* c = closures[handle].upvalues[ui];
                            ZEUS_DEBUG_PRINT("  slot=%zu ptr=%p", ui, (void*)c);
                        }
                    }

                stack.push_back(ZeusType::ClosureHandle{ handle });
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CALL_CLOSURE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CALL_CLOSURE: {
                // ========== 函数调用入口 ==========
                const int argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));

                // 错误处理 helper：在出错时从栈中弹出 callee + argc（可选）并返回占位值
                auto fail = [&](const std::string& msg, bool includeCallee) {
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    int pops = argc + (includeCallee ? 1 : 0);
                    if (pops > 0 && stack.size() >= (size_t)pops) {
                        shrinkStackWithRelease(stack.size() - (size_t)pops);
                    } else {
                        shrinkStackWithRelease(0);
                    }
                    stack.push_back(LexerType::Value(0));
                };

                // 期望栈布局: [..., callee, arg0, arg1, ..., arg{n-1}]
                if (stack.size() < static_cast<size_t>(argc) + 1) {
                    fail("CALL_CLOSURE stack not enough for new layout", false);
                    break;
                }

                const size_t calleeIndex   = stack.size() - (argc + 1);
                const size_t callFrameBase = calleeIndex;

                vmMessage(
                    std::string("[CALL_CLOSURE_ENTER] stackSize=") + std::to_string(stack.size()) +
                    " argc=" + std::to_string(argc) +
                    " calleeIndex=" + std::to_string(calleeIndex)
                );

                ZeusType::UniversalVariant fnVar = stack[calleeIndex];

                // DEBUG: 输出 fnVar 的类型
                ZEUS_DEBUG_PRINT("[CALL_CLOSURE] fnVar index=%d, holds_alternative<ClosureHandle>=%d, holds_alternative<Value>=%d",
                    (int)calleeIndex,
                    std::holds_alternative<ZeusType::ClosureHandle>(fnVar),
                    std::holds_alternative<LexerType::Value>(fnVar));

                // late-resolve 尝试：当 callee 是字符串（函数名）时，尝试将其解析为 closure handle
                if (instr.metadata.tag == "generic_vm_specialization") {
                    std::string targetName;
                    if (instr.metadata.kind != IRType::IRMetadataKind::CallTarget ||
                        !instr.metadata.readDirectCall(targetName)) {
                        fail("explicit generic call has no resolved VM target", true);
                        break;
                    }
                    const auto target = functionNameToProtoId.find(targetName);
                    if (target == functionNameToProtoId.end() ||
                        getPrototype(target->second) == nullptr) {
                        fail("explicit generic target is missing from the VM prototype table: " +
                             targetName, true);
                        break;
                    }

                    bool alreadySpecialized = false;
                    if (const auto* closureHandle =
                            std::get_if<ZeusType::ClosureHandle>(&fnVar)) {
                        const Closure* currentClosure = getClosure(closureHandle->id);
                        alreadySpecialized = currentClosure != nullptr &&
                            currentClosure->protoId == target->second;
                    }
                    if (!alreadySpecialized) {
                        const ZeusType::UniversalVariant previousCallee = fnVar;
                        const uint8_t previousOwner = stack.ownerFlag(calleeIndex);
                        Closure specializedClosure;
                        specializedClosure.protoId = target->second;
                        specializedClosure.autoRelease = true;
                        const int specializedHandle =
                            allocClosure(std::move(specializedClosure));
                        stack.set(
                            calleeIndex,
                            ZeusType::ClosureHandle{specializedHandle});
                        dropStackItem(StackItem{previousCallee, previousOwner});
                        fnVar = stack[calleeIndex];
                    }
                }

                auto tryLateResolve = [&](ZeusType::UniversalVariant& v) {
                    // 如果已经是 ClosureHandle，直接返回
                    if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
                        return;
                    }
                    // 仅处理 Value<string> 类型
                    if (!std::holds_alternative<LexerType::Value>(v)) {
                        return;
                    }

                    const auto& vv = std::get<LexerType::Value>(v);
                    if (!std::holds_alternative<std::string>(vv.data)) {
                        return;
                    }

                    // 严格模式下不允许 late resolve
                    if (!enableLateResolve) {
                        VMException::VMException exp("Strict mode: unresolved function name before call", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        return;
                    }

                    const std::string& name = std::get<std::string>(vv.data);

                    // 1) 优先查找 nameToClosure 映射
                    auto itc = nameToClosure.find(name);
                    if (itc != nameToClosure.end()) {
                        int h = itc->second;
                        if (h >= 0 && h < (int)closures.size()) {
                            stack[calleeIndex] = ZeusType::ClosureHandle{ h };
                            v = stack[calleeIndex];
                            return;
                        }
                    }

                    // 2) 回退：遍历 globalNames，对应 global 插槽中查找 closure
                    if (!globalNames.empty()) {
                        for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                            if (globalNames[gi] == name && gi < (int)global.size() && global[gi]) {
                                const auto gval = global[gi]->readValue();
                                if (std::holds_alternative<ZeusType::ClosureHandle>(gval)) {
                                    int cid = std::get<ZeusType::ClosureHandle>(gval).id;
                                    if (cid >= 0 && cid < (int)closures.size()) {
                                        nameToClosure[name]   = cid;
                                        stack[calleeIndex]    = ZeusType::ClosureHandle{ cid };
                                        v                     = stack[calleeIndex];
                                    }
                                }
                                break; // 找到匹配后跳出循环
                            }
                        }
                    }
                };

                tryLateResolve(fnVar);

                // 调用前的类型检查：确保 callee 已解析为 closure handle
                if (!std::holds_alternative<ZeusType::ClosureHandle>(fnVar)) {
                    std::string actualType = ZeusVMUtils::valueToDebugString(fnVar);
                    std::string errorMsg = "CALL_CLOSURE: Attempted to call non-function value. ";
                    errorMsg += "Expected a function but got: " + actualType + ". ";
                    errorMsg += "This usually means the variable is nil or not properly initialized as a function.";
                    fail(errorMsg, true);
                    break;
                }

                const int handle = std::get<ZeusType::ClosureHandle>(fnVar).id;
                Closure* closPtr = getClosure(handle);
                if (!closPtr) {
                    fail("CALL_CLOSURE invalid handle", true);
                    break;
                }

                Closure&    clos  = *closPtr;
                const Prototype* protoPtr = getPrototype(clos.protoId);
                if (!protoPtr) {
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] invalid protoId=%d, closureId=%d, closures.size=%zu, sharedClosures=%p, sharedPrototypes=%p",
                        clos.protoId, handle, closures.size(), (void*)sharedClosures, (void*)sharedPrototypes);
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] closure refCount=%d, isShared=%d, upvalues=%zu, boundThis=%d",
                        clos.refCount, (int)isSharedClosure(handle), clos.upvalues.size(), clos.boundThis.has_value() ? 1 : 0);
                    if (sharedClosures) {
                        ZEUS_DEBUG_PRINT("[CALL_CLOSURE] sharedClosures->size=%zu", sharedClosures->size());
                    }
                    if (sharedPrototypes) {
                        ZEUS_DEBUG_PRINT("[CALL_CLOSURE] sharedPrototypes->size=%zu", sharedPrototypes->size());
                    }
                    fail("CALL_CLOSURE invalid protoId", true);
                    break;
                }
                const Prototype&  proto = *protoPtr;

                // 保存调用前的 local 和 upvalue 环境
                bool capturesReady = true;
                for (Cell* captureCell : clos.upvalues) {
                    if (!captureCell) {
                        fail("CALL_CLOSURE: closure has an uninitialized capture cell", true);
                        capturesReady = false;
                        break;
                    }
                }
                if (!capturesReady) break;

                auto oldLocal   = local;
                auto oldUpvalue = upvalue;

                const bool hasBorrowedReceiverSource = instr.args.size() >= 6 &&
                    std::holds_alternative<std::string>(instr.args[1]) &&
                    std::get<std::string>(instr.args[1]) ==
                        "__zeus_receiver_source";
                Cell* borrowedReceiverCell = nullptr;
                std::shared_ptr<ZeusShared::SharedCell> borrowedReceiverShared;
                ZeusType::UniversalVariant borrowedReceiverValue;
                ZeusType::UniversalVariant borrowedReceiverReference;
                bool borrowedReceiverReadOnly = false;
                std::string receiverSourceError;
                if (hasBorrowedReceiverSource) {
                    if (!std::holds_alternative<std::string>(instr.args[2]) ||
                        !std::holds_alternative<LexerType::Value>(instr.args[3]) ||
                        !std::holds_alternative<std::string>(instr.args[4]) ||
                        !std::holds_alternative<LexerType::Value>(instr.args[5])) {
                        receiverSourceError = "VM method receiver source metadata is invalid";
                    } else {
                        const std::string& scope = std::get<std::string>(instr.args[2]);
                        const std::string& name = std::get<std::string>(instr.args[4]);
                        int64_t rawIndex = -1;
                        int64_t rawReadOnly = 0;
                        if (!ZeusVMUtils::tryGetInt64FromValue(
                                std::get<LexerType::Value>(instr.args[3]), rawIndex) ||
                            !ZeusVMUtils::tryGetInt64FromValue(
                                std::get<LexerType::Value>(instr.args[5]), rawReadOnly)) {
                            receiverSourceError = "VM method receiver source locator is invalid";
                        } else {
                            borrowedReceiverReadOnly = rawReadOnly != 0;
                            if (scope == "local" && rawIndex >= 0 &&
                                rawIndex < static_cast<int64_t>(oldLocal.size())) {
                                borrowedReceiverCell = oldLocal[static_cast<size_t>(rawIndex)];
                            } else if (scope == "global" && rawIndex >= 0 &&
                                       rawIndex < static_cast<int64_t>(global.size())) {
                                borrowedReceiverCell = global[static_cast<size_t>(rawIndex)];
                            } else if (scope == "upvalue" && rawIndex >= 0 &&
                                       rawIndex < static_cast<int64_t>(oldUpvalue.size())) {
                                borrowedReceiverCell = oldUpvalue[static_cast<size_t>(rawIndex)];
                            } else if (scope == "shared" && !name.empty()) {
                                borrowedReceiverShared =
                                    ZeusShared::getGlobalHeap().find(name);
                            } else {
                                receiverSourceError =
                                    "VM method receiver source is outside its " + scope + " storage";
                            }

                            if (receiverSourceError.empty()) {
                                if (borrowedReceiverCell) {
                                    try {
                                        borrowedReceiverValue = loadCellValue(
                                            borrowedReceiverCell);
                                    } catch (const std::exception& error) {
                                        receiverSourceError = error.what();
                                    }
                                    borrowedReceiverReference =
                                        ZeusType::ReferenceHandle{
                                            borrowedReceiverCell, "cell"};
                                } else if (borrowedReceiverShared) {
                                    borrowedReceiverValue = borrowedReceiverShared->read();
                                    borrowedReceiverReference =
                                        ZeusType::ReferenceHandle{
                                            borrowedReceiverShared.get(), "shared",
                                            borrowedReceiverShared->generation};
                                } else if (receiverSourceError.empty()) {
                                    receiverSourceError =
                                        "VM method receiver source is no longer alive";
                                }
                            }
                        }
                    }
                }

                // 清空并初始化新的 local，用于被调用函数（大小为 proto.paramCount），并为每个参数创建占位 Cell
                const int callerLine = __line;
                ZeusVMDetail::CallSetupScope callSetup([&]() {
                    releaseCurrentFrameEnvironment();
                    local = std::move(oldLocal);
                    upvalue = std::move(oldUpvalue);
                    shrinkStackWithRelease(calleeIndex);
                    __line = callerLine;
                });
                local.clear();
                upvalue.clear();
                local.resize(proto.paramCount);
                for (int i = 0; i < proto.paramCount; ++i) {
                    local[i] = newCell(LexerType::Value(0), 0, "CALL_CLOSURE_local");
                    if (i < static_cast<int>(proto.paramTypes.size()) &&
                        (i >= static_cast<int>(proto.paramIsReference.size()) || !proto.paramIsReference[i])) {
                        configureNativeScalarCell(local[i], proto.paramTypes[i]);
                    }
                }
                // upvalue 指向 closure 的 upvalues（Cell 指针集合）
                // The closure owns the capture cells. Reference captures share
                // the outer cell; copy captures share the closure's snapshot cell.
                // Both modes use that same cell for each invocation, so a copied
                // capture can keep private mutable state across calls.
                upvalue = clos.upvalues;
                for (auto* cell : upvalue) cell->retain();
                const auto& callUpvalues = upvalue;
                    // Debug: dump callUpvalues (debug channel)
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] calling proto=%d clos=%d callUpvals=", clos.protoId, handle);
                    for (size_t ui = 0; ui < callUpvalues.size(); ++ui) {
                        ZEUS_DEBUG_PRINT("  slot=%zu ptr=%p", ui, (void*)callUpvalues[ui]);
                    }

                // 如果闭包有绑定的this (来自 OBJ_LOAD_ATTR 获取实例方法), 将其作为第一个参数插入
                // 当前栈: [callee, arg0, arg1, ...]
                // 需要变为: [callee, this, arg0, arg1, ...]
                int actualArgc = argc;
                if (clos.boundThis.has_value()) {
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Closure has boundThis, inserting as first arg");
                    // 在 calleeIndex + 1 位置插入 boundThis
                    retainObject(*clos.boundThis); // stack owns a reference
                    stack.insert(stack.begin() + (calleeIndex + 1), clos.boundThis.value());
                    actualArgc++;
                }


                // ===== 参数处理：支持默认参数、可变参数和命名参数 =====
                int regularParamCount = proto.paramCount;

                // 扩展 local 以容纳常规参数 + *args + **kwargs
                if (proto.hasVariadicArgs || proto.hasVariadicKwargs) {
                    int extraSlots = 0;
                    if (proto.hasVariadicArgs) extraSlots++;
                    if (proto.hasVariadicKwargs) extraSlots++;

                    int newLocalSize = regularParamCount + extraSlots;
                    local.resize(newLocalSize);
                    for (int idx = regularParamCount; idx < newLocalSize; ++idx) {
                        local[idx] = newCell(LexerType::Value(0), 0, "CALL_varargs");
                    }
                }

                // 扫描栈上的参数,分离位置参数和命名参数
                std::map<std::string, ZeusType::UniversalVariant> namedArgs;
                std::vector<ZeusType::UniversalVariant> positionalArgs;
                std::vector<bool> paramProvided(regularParamCount, false);

                for (int i = 0; i < actualArgc; ++i) {
                    auto& arg = stack[calleeIndex + 1 + i];
                    if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(arg)) {
                        // 命名参数
                        auto namedArgPtr = std::get<std::shared_ptr<ZeusType::NamedArg>>(arg);
                        namedArgs[namedArgPtr->name] = namedArgPtr->value;
                        ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Named arg: %s", namedArgPtr->name.c_str());
                    } else {
                        // 位置参数
                        positionalArgs.push_back(arg);
                    }
                }

                int positionalCount = (int)positionalArgs.size();
                ZEUS_DEBUG_PRINT("[CALL_CLOSURE] positional=%d, named=%d", positionalCount, (int)namedArgs.size());
                if (hasBorrowedReceiverSource && receiverSourceError.empty()) {
                    if (positionalArgs.empty() ||
                        runtimeValueTypeName(positionalArgs.front()) !=
                            runtimeValueTypeName(borrowedReceiverValue)) {
                        receiverSourceError =
                            "VM method receiver changed before its borrowed address was used";
                    }
                }

                // 填充常规参数
                std::set<std::string> consumedNamedArgs; // 记录已使用的命名参数

                bool parameterTypeMismatch = false;
                for (int i = 0; i < regularParamCount; ++i) {
                    bool filled = false;

                    // 优先使用位置参数
                    if (i < positionalCount) {
                        ZeusType::UniversalVariant argumentValue = positionalArgs[i];
                        const bool useBorrowedReceiver =
                            hasBorrowedReceiverSource && i == 0;
                        if (useBorrowedReceiver) {
                            if (!receiverSourceError.empty()) {
                                parameterTypeMismatch = true;
                                break;
                            }
                            argumentValue = i < static_cast<int>(proto.paramIsReference.size()) &&
                                    proto.paramIsReference[i]
                                ? borrowedReceiverReference
                                : borrowedReceiverValue;
                        } else if (i < static_cast<int>(proto.paramIsReference.size()) &&
                            proto.paramIsReference[i]) {
                            const int receiverOffset = clos.boundThis.has_value() ? 1 : 0;
                            const int positionalSourceIndex = i - receiverOffset;
                            if (!resolveReferenceArgument(
                                    instr,
                                    proto,
                                    oldLocal,
                                    oldUpvalue,
                                    i,
                                    positionalSourceIndex,
                                    i < static_cast<int>(proto.paramNames.size())
                                        ? proto.paramNames[i]
                                        : std::string(),
                                    positionalArgs[i],
                                    argumentValue)) {
                                parameterTypeMismatch = true;
                                break;
                            }
                        }
                        if (!validateFunctionArgument(proto, i, argumentValue, instr)) {
                            parameterTypeMismatch = true;
                            break;
                        }
                        if (useBorrowedReceiver) {
                            assignCellValue(local[i], borrowedReceiverReference);
                            if (borrowedReceiverReadOnly) local[i]->flags |= 1;
                        } else {
                            assignCellValue(local[i], argumentValue);
                        }
                        paramProvided[i] = true;
                        filled = true;
                    }

                    // 尝试从命名参数中按名字匹配
                    if (!filled && i < (int)proto.paramNames.size()) {
                        const std::string& paramName = proto.paramNames[i];
                        auto it = namedArgs.find(paramName);
                        if (it != namedArgs.end()) {
                            ZeusType::UniversalVariant argumentValue = it->second;
                            if (i < static_cast<int>(proto.paramIsReference.size()) &&
                                proto.paramIsReference[i]) {
                                if (!resolveReferenceArgument(
                                        instr, proto, oldLocal, oldUpvalue,
                                        i, -1, paramName,
                                        it->second, argumentValue)) {
                                    parameterTypeMismatch = true;
                                    break;
                                }
                            }
                            if (!validateFunctionArgument(proto, i, argumentValue, instr)) {
                                parameterTypeMismatch = true;
                                break;
                            }
                            assignCellValue(local[i], argumentValue);
                            consumedNamedArgs.insert(paramName);
                            paramProvided[i] = true;
                            filled = true;
                            ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Filled param[%d] '%s' from named arg", i, paramName.c_str());
                        }
                    }

                    if (!filled && !proto.hasRuntimeDefaultPrologue &&
                        i < (int)proto.paramHasDefault.size() && proto.paramHasDefault[i] &&
                        i < (int)proto.paramDefaultValues.size()) {
                        if (!validateFunctionArgument(
                                proto, i, proto.paramDefaultValues[i], instr)) {
                            parameterTypeMismatch = true;
                            break;
                        }
                        assignCellValue(local[i], proto.paramDefaultValues[i]);
                        filled = true;
                    }

                    // 最后用0初始化
                    if (!filled) {
                        assignCellValue(local[i], LexerType::Value(0));
                    }
                }

                if (parameterTypeMismatch) {
                    callSetup.rollback();
                    if (!receiverSourceError.empty()) {
                        VMException::VMException(
                            receiverSourceError, __line, instr)
                            ._throw(vmstat, currentChunk);
                    } else {
                        stack.push_back(LexerType::Value(0));
                    }
                    break;
                }

                for (int i = 0; i < regularParamCount; ++i) {
                    const bool hasDefault = i < (int)proto.paramHasDefault.size() && proto.paramHasDefault[i];
                    if (!paramProvided[i] && !hasDefault) {
                        const std::string paramName = i < (int)proto.paramNames.size()
                            ? proto.paramNames[i]
                            : std::to_string(i);
                        VMException::VMException exp(
                            "CALL_CLOSURE: missing required argument '" + paramName + "'",
                            __line,
                            instr
                        );
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                }
                if (vmstat != ZeusType::VMStatus::Running) break;

                // 检查是否传递了过多的参数（仅当没有 *args 时）
                if (!proto.hasVariadicArgs && positionalCount > regularParamCount) {
                    VMException::VMException exp(
                        "CALL_CLOSURE: too many arguments. Expected at most " +
                        std::to_string(regularParamCount) + " arguments, but got " +
                        std::to_string(positionalCount),
                        __line,
                        instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 检查未被消费的命名参数（仅当没有 **kwargs 时）
                if (!proto.hasVariadicKwargs) {
                    for (const auto& kv : namedArgs) {
                        if (consumedNamedArgs.find(kv.first) == consumedNamedArgs.end()) {
                            VMException::VMException exp(
                                "CALL_CLOSURE: unexpected named argument '" + kv.first + "'",
                                __line,
                                instr
                            );
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    }
                }

                // 构建 *args 数组(收集多余的位置参数)
                if (proto.hasVariadicArgs) {
                    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                    std::vector<ZeusType::UniversalVariant> argsVec;
                    for (int i = regularParamCount; i < positionalCount; ++i) {
                        argsVec.push_back(positionalArgs[i]);
                    }

                    auto arrayDataPtr = ZeusGC::makeArrayData();
                    arrayDataPtr->valueRefCount.store(0, std::memory_order_relaxed);
                    arrayDataPtr->data = argsVec;
                    ZeusType::Array argsArray;
                    argsArray.impl = arrayDataPtr;

                    ZeusType::Variant variantArgsArray = argsArray;
                    assignCellValue(local[regularParamCount], variantArgsArray);
                }

                // 构建 **kwargs 字典(收集未被消费的命名参数)
                if (proto.hasVariadicKwargs) {
                    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                    auto objDataPtr = ZeusGC::makeObjectData();

                    // 只加入未被消费的命名参数
                    for (const auto& kv : namedArgs) {
                        if (consumedNamedArgs.find(kv.first) == consumedNamedArgs.end()) {
                            objDataPtr->setValue(kv.first, kv.second);
                            ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Extra named arg '%s' -> kwargs", kv.first.c_str());
                        }
                    }

                    ZeusType::Object kwargsDict;
                    kwargsDict.impl = objDataPtr;

                    ZeusType::Variant variantKwargs = kwargsDict;
                    int kwargsSlot = regularParamCount;
                    if (proto.hasVariadicArgs) kwargsSlot++;
                    assignCellValue(local[kwargsSlot], variantKwargs);
                }

                // ========== 新的栈帧实现：不再使用while循环 ==========
                // 检查调用深度
                if (callFrames.size() >= (size_t)MAX_CALL_DEPTH) {
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1900_STACK_OVERFLOW,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "Stack overflow: possible infinite recursion",
                        __line, instr
                    );
                    exp.setDetails(
                        std::string("Call depth exceeded ") + std::to_string(MAX_CALL_DEPTH) + " levels\r\n" +
                        "Current call depth: " + std::to_string(callFrames.size())
                    );
                    exp.setSuggestion(
                        std::string("Check for infinite recursion, especially in:\r\n") +
                        "  • operator[.] overloads accessing 'this.member' (use getattr instead)\r\n" +
                        "  • Recursive function calls without proper base case\r\n" +
                        "  • Circular references in object construction"
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 创建新栈帧
                CallFrame newFrame;
                newFrame.code = &proto.body;
                newFrame.pc = 0;
                newFrame.savedLocal = oldLocal;
                newFrame.savedUpvalue = oldUpvalue;
                newFrame.stackBase = callFrameBase;
                newFrame.savedLine = __line;
                newFrame.paramProvided = std::move(paramProvided);
                newFrame.referenceParameterSlots = proto.paramIsReference;

                 // callee closure 的生命周期由栈收缩（RETURN -> shrinkStackWithRelease(stackBase)）统一处理。
                 // 这里不再基于 refCount 推断“临时闭包”，避免与栈/容器 retain 语义冲突。
                 newFrame.calleeAutoReleaseId = -1;
                 callFrames.push_back(newFrame);
                  callSetup.commit();
                 justPushedFrame = true;  // 标记已创建栈帧

                ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Pushed frame (depth=%zu), proto body size=%zu",
                    callFrames.size(), proto.body.size());

                // 不需要while循环了，runChunk会处理新栈帧的执行
                break;

                // ========== 以下是旧代码，已被栈帧机制替代 ==========
                #if 0
                while (pc < proto.body.size()) {
                    const auto& ins = proto.body[pc];

                    // ---- 跳过 FUNC_BEGIN / CAPTURE_OUTSIDE_VAR 指令 ----
                    if (ins.op == VMOperator::FUNC_BEGIN || ins.op == VMOperator::CAPTURE_OUTSIDE_VAR) {
                        pc++;
                        continue;
                    }

                    if (ins.op == VMOperator::FUNC_END) {
                        pc++;
                        continue;
                    }

                    // ---- RETURN ----
                    if (ins.op == VMOperator::RETURN) {
                        if (!validateNativeScalarReturn(proto.body, ins)) break;
                        if (!stack.empty()) {
                            retVal = stack.back();
                            stack.pop_back();
                            hasRet = true;
                        }
                        break;
                    }

                    // ---- 跳转指令处理 ----
                    if (ins.op == VMOperator::JUMP) {
                        int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                        pc += 1 + off;
                        continue;
                    }

                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));

                        if (!stack.empty()) {
                            auto cond = stack.back();
                            stack.pop_back();
                            if (!truthy(cond)) {
                                pc += 1 + off;
                                continue;
                            }
                        }
                        pc++;
                        continue;
                    }

                    if (ins.op == VMOperator::JUMP_IF_TRUE) {
                        int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));

                        if (!stack.empty()) {
                            auto cond = stack.back();
                            stack.pop_back();

                            if (truthy(cond)) {
                                pc += 1 + off;
                                continue;
                            }
                        }
                        pc++;
                        continue;
                    }

                    // ---- 执行普通指令 ----
                    execute(ins);
                    pc++;
                }
                #endif
                // ========== 旧的清理代码已移到RETURN处理（runChunk） ==========
                #if 0
                // 恢复调用前的 local/upvalue 状态，并对需要闭合的 local 做闭合处理
                {
                    // 收集当前 local 中的 Cell 指针
                    std::unordered_set<Cell*> localSet;
                    localSet.reserve(local.size());

                    for (Cell* c : local)
                        if (c) localSet.insert(c);

                    // 遍历所有 closures 的 upvalues，若某个 upvalue 指向当前 local 中的 Cell，则需要将其替换为闭包闭合的 Cell
                    std::unordered_map<Cell*, Cell*> closedMap; // oldLocalPtr -> closedCell

                    for (auto& c : closures) {
                        for (auto& uv : c.upvalues) {
                            if (uv && localSet.count(uv)) {
                                Cell* oldPtr = uv;
                                Cell* closed = nullptr;

                                auto it = closedMap.find(oldPtr);
                                if (it == closedMap.end()) {
                                    closed = cloneCell(oldPtr);
                                    closedMap.emplace(oldPtr, closed);
                                } else {
                                    closed = it->second;
                                }
                                // closed 是闭合后的 Cell 指针，增加引用计数
                                closed->retain();
                                // 释放原 upvalue 的引用
                                uv->release();
                                // 用闭合后的 Cell 替换 upvalue
                                uv = closed;
                            }
                        }
                    }

                    // 清理当前函数的 local：释放并置空所有占位 Cell
                    for (auto& cptr : local) {
                        if (cptr) { cptr->release(); cptr = nullptr; }
                    }
                    // release any temporary per-call upvalues we created earlier
                    for (size_t ci = 0; ci < /*callUpvalues size*/ (size_t)0; ++ci) {
                        // no-op placeholder: actual callUpvalues is out of scope here, so we cleanup below
                    }
                }

                // 在缩小栈回到 callFrameBase 时进行校验
                if (stack.size() < callFrameBase + argc + 1) {
                    vmMessage("[CALL_CLOSURE_WARN] unexpected stack shrink before frame cleanup: size=" + std::to_string(stack.size()) +
                        " expected>=" + std::to_string(callFrameBase + argc + 1));
                }
                shrinkStackWithRelease(callFrameBase);

                local = std::move(oldLocal);
                upvalue = std::move(oldUpvalue);

                // 将返回值压回栈（如果有返回值则压入返回值，否则压入 nil）
                stack.push_back(hasRet ? retVal : ZeusType::UniversalVariant(LexerType::Value(0)));
                vmMessage("[CALL_CLOSURE_EXIT] base=" + std::to_string(callFrameBase) + " finalStackSize=" + std::to_string(stack.size()) +
                    " retType=" + (hasRet?retVal.index()==0?"val":"complex":"void"));
                printStack();

                // 递归深度恢复
                callDepth--;
                #endif
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOAD_NATIVE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOAD_NATIVE: {
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("LOAD_NATIVE missing id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int nid = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));

                // Non-negative ids address builtins; negative ids are native
                // injections resolved from @native declarations.
                const bool validHandle = nid >= 0
                    ? ZeusBuiltins::get(nid) != nullptr
                    : ZeusBuiltins::nativeInjectionFromHandle(nid) != nullptr ||
                      ZeusBuiltins::dynamicNativeImportFromHandle(nid) != nullptr;
                if (!validHandle) {
                    VMException::VMException exp("LOAD_NATIVE invalid id: " + std::to_string(nid), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                stack.push_back(ZeusType::NativeFuncHandle{ nid });
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CALL_NATIVE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CALL_NATIVE: {
                NativeCallContext ctx;
                if (!prepareNativeCall(instr, ctx)) { break; }
                 if (!validateNativeArity(ctx, instr)) {
                    for (int i = 0; i < ctx.argc + 1; ++i) {
                        if (!stack.empty()) dropStackBack();
                    }
                    stack.push_back(LexerType::Value(0));
                    printStack();
                    break;
                 }
                if (startPendingObjectClone(ctx)) {
                    printStack();
                    break;
                }
                auto ret = invokeNative(ctx);
                cleanupNativeCall(ctx, ret);
                printStack();
                break;
            }
        }
    }
