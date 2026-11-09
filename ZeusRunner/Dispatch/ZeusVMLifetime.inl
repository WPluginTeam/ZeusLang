// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_DELETE_VAR(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::DELETE_VAR: {
                // DELETE_VAR: 删除变量，释放其 Cell 对象
                // 参数: [string varName, string kind, Value index]
                // kind: "local", "upvalue", "global"
                // 如果对象有 operator[~]，会在释放前调用它

                if (instr.args.empty() || !std::holds_alternative<std::string>(instr.args[0])) {
                    vmMessage("[DELETE_VAR] Error: Missing variable name");
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                std::string kind = "global";
                int varIndex = -1;

                // 解析类型和索引参数
                if (instr.args.size() >= 2 && std::holds_alternative<std::string>(instr.args[1])) {
                    kind = std::get<std::string>(instr.args[1]);
                }

                if (instr.args.size() >= 3 && std::holds_alternative<LexerType::Value>(instr.args[2])) {
                    varIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[2]));
                }

                ZEUS_DEBUG_PRINT("[DELETE_VAR] Deleting %s variable: %s (index=%d)", kind.c_str(), varName.c_str(), varIndex);

                // 调用 operator[~] 的辅助 lambda
                auto objectHandleFromStoredObject = [&](const ZeusType::UniversalVariant& value) {
                    ZeusType::ObjectHandle invalid{-1, 0};
                    const auto* wrapped = std::get_if<ZeusType::Variant>(&value);
                    if (!wrapped) return invalid;
                    const auto* object = std::get_if<ZeusType::Object>(wrapped);
                    if (!object || !object->impl) return invalid;

                    const auto idIt = object->impl->data.find("__obj_id__");
                    if (idIt == object->impl->data.end() ||
                        !std::holds_alternative<LexerType::Value>(idIt->second)) {
                        return invalid;
                    }
                    const int objectId = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(idIt->second));
                    int generation = 0;
                    const auto generationIt = object->impl->data.find("__obj_generation__");
                    if (generationIt != object->impl->data.end() &&
                        std::holds_alternative<LexerType::Value>(generationIt->second)) {
                        generation = VMHelpers::getIntFromValue(
                            std::get<LexerType::Value>(generationIt->second));
                    }
                    return ZeusType::ObjectHandle{objectId, generation};
                };

                auto resolveObjectHandle = [&](const ZeusType::UniversalVariant& value) {
                    if (std::holds_alternative<ZeusType::ObjectHandle>(value)) {
                        return std::get<ZeusType::ObjectHandle>(value);
                    }
                    if (std::holds_alternative<ZeusType::SharedObjectHandle>(value)) {
                        const auto& shared = std::get<ZeusType::SharedObjectHandle>(value);
                        auto source = ZeusShared::getGlobalHeap().find(shared.name, shared.generation);
                        return source ? objectHandleFromStoredObject(source->read())
                                      : ZeusType::ObjectHandle{-1, 0};
                    }
                    return objectHandleFromStoredObject(value);
                };

                auto callDestructor = [&](const ZeusType::UniversalVariant& cellVal) {
                    ZeusType::ObjectHandle objH = resolveObjectHandle(cellVal);
                    if (objH.id < 0) return;
                    invokeObjectDestructor(objH);
                };

                // 释放 Cell 中持有的对象或闭包的辅助 lambda
                auto releaseRefInCell = [&](const ZeusType::UniversalVariant& cellVal) {
                    if (std::holds_alternative<ZeusType::ObjectHandle>(cellVal)) {
                        releaseObject(std::get<ZeusType::ObjectHandle>(cellVal));
                    } else if (std::holds_alternative<ZeusType::SharedObjectHandle>(cellVal)) {
                        const auto shared = std::get<ZeusType::SharedObjectHandle>(cellVal);
                        auto target = ZeusShared::getGlobalHeap().find(shared.name, shared.generation);
                        if (target) {
                            const auto targetObject = objectHandleFromStoredObject(target->read());
                            auto removedTarget = ZeusShared::getGlobalHeap().erase(
                                shared.name, shared.generation);
                            if (removedTarget) {
                                removedTarget->write(ZeusType::Variant(ZeusType::Null{}));
                                if (targetObject.id >= 0) releaseObject(targetObject);
                            }
                        }
                    } else if (std::holds_alternative<ZeusType::ClosureHandle>(cellVal)) {
                        int cloId = std::get<ZeusType::ClosureHandle>(cellVal).id;
                        releaseClosure(cloId);
                        ZEUS_DEBUG_PRINT("[DELETE_VAR] Released closure #%d from cell", cloId);
                    }
                };

                bool found = false;

                // 提取对象 ID 并打断闭包循环的辅助 lambda
                auto breakCyclesIfObject = [&](const ZeusType::UniversalVariant& cellVal) {
                    const auto handle = resolveObjectHandle(cellVal);
                    if (handle.id < 0 || !getValidObject(handle)) return;
                    forceBreakClosureCycles(handle);
                    destroyObjectSource(handle);
                };

                if (kind == "shared") {
                    auto source = ZeusShared::getGlobalHeap().find(varName);
                    if (source) {
                        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                        source->lockWrite();
                        struct SharedDeleteUnlock {
                            std::shared_ptr<ZeusShared::SharedCell> cell;
                            ~SharedDeleteUnlock() {
                                if (cell) cell->unlockWrite();
                            }
                        } sharedDeleteUnlock{source};

                        const auto value = source->read();
                        callDestructor(value);
                        if (vmstat != ZeusType::VMStatus::Running) break;
                        breakCyclesIfObject(value);
                        auto removed = ZeusShared::getGlobalHeap().erase(
                            varName, source->generation);
                        if (removed) {
                            // Erasing a shared binding releases exactly its one
                            // owner. Explicit delete has already invalidated the
                            // instance; aliases keep tombstones until released.
                            removed->write(ZeusType::Variant(ZeusType::Null{}));
                            ZeusGC::releaseVariantDeep(*this, value);
                            const auto representedObject = objectHandleFromStoredObject(value);
                            if (representedObject.id >= 0) releaseObject(representedObject);
                        }
                        found = !ZeusShared::getGlobalHeap().find(
                            varName, source->generation);
                    }
                } else if (kind == "local" && varIndex >= 0 && varIndex < (int)local.size()) {
                    // 删除局部变量
                    Cell* cell = local[varIndex];

                    if (cell) {
                        callDestructor(cell->readValue());
                        if (vmstat != ZeusType::VMStatus::Running) break;
                        const auto value = cell->readValue();
                        breakCyclesIfObject(value);
                        releaseRefInCell(value);
                        cell->release();
                        local[varIndex] = nullptr;
                        found = true;
                    }
                } else if (kind == "upvalue" && varIndex >= 0 && varIndex < (int)upvalue.size()) {
                    // 删除 upvalue
                    Cell* cell = upvalue[varIndex];

                    if (cell) {
                        callDestructor(cell->readValue());
                        if (vmstat != ZeusType::VMStatus::Running) break;
                        const auto value = cell->readValue();
                        breakCyclesIfObject(value);
                        releaseRefInCell(value);
                        cell->release();
                        upvalue[varIndex] = nullptr;
                        found = true;
                    }
                } else {
                    // 删除全局变量（按名称查找）
                    for (size_t i = 0; i < global.size(); ++i) {
                        if (i < globalNames.size() && globalNames[i] == varName) {
                            if (global[i]) {
                                callDestructor(global[i]->readValue());
                                if (vmstat != ZeusType::VMStatus::Running) break;
                                const auto value = global[i]->readValue();
                                breakCyclesIfObject(value);
                                releaseRefInCell(value);
                                global[i]->release();
                                global[i] = nullptr;
                                found = true;
                            }
                            break;
                        }
                    }
                }

                if (vmstat != ZeusType::VMStatus::Running) break;

                if (!found) {
                    vmMessage("[DELETE_VAR] Warning: Variable not found or already null: " + varName);
                }

                // Safe point: sweep truly unreachable objects/closures.
                collectGarbageSweep();

                printStack();
                break;
            }

            // ===== 异常处理操作码 =====
        }
    }

    ZEUS_VM_NOINLINE void execute_WITH_BEGIN(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::WITH_BEGIN: {
                // WITH_BEGIN slotIdx: 开始 with 块（栈顶是 with 对象）
                // slotIdx: capture 变量槽索引，-1 表示无 capture
                if (stack.empty()) {
                    VMException::VMException exp("WITH_BEGIN: stack is empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto withObj = stack.back();
                stack.pop_back();

                if (!std::holds_alternative<ZeusType::ObjectHandle>(withObj)) {
                    VMException::VMException exp("WITH_BEGIN: expected object, got " +
                        ZeusVMUtils::valueToDebugString(withObj), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto& oh = std::get<ZeusType::ObjectHandle>(withObj);
                auto* obj = getValidObject(oh);
                if (!obj) {
                    VMException::VMException exp("WITH_BEGIN: object not found", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 检查是否有 operator[with] 方法
                auto* classPtr = getClass(obj->classId);
                if (!classPtr) {
                    VMException::VMException exp("WITH_BEGIN: class not found", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto opIt = classPtr->operatorMethods.find("[with]");
                if (opIt == classPtr->operatorMethods.end()) {
                    VMException::VMException exp("WITH_BEGIN: object does not have operator[with] method", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 如果有 capture 槽，将对象引用存入该槽
                int captureSlot = -1;
                if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    captureSlot = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }

                if (captureSlot >= 0) {
                    // 将对象引用存入 capture 槽
                    if (local.size() <= static_cast<size_t>(captureSlot)) {
                        local.resize(captureSlot + 1, nullptr);
                    }
                    if (!local[captureSlot]) {
                        local[captureSlot] = newCell(withObj, 0, "WITH_CAPTURE");
                    } else {
                        assignCellValue(local[captureSlot], withObj);
                    }
                    ZEUS_DEBUG_PRINT("[VM] WITH_BEGIN: stored object to capture slot %d", captureSlot);
                }

                // 调用 operator[with]()
                //
                // 用 executeClosureSync 而不是递归 execute()：它会压一个真正的栈帧
                // 并把帧跑完，局部变量随帧保存/还原。递归 execute() 会共用同一份
                // local 缓冲区，被调用帧的槽位会污染调用者。
                int protoId = opIt->second;
                Closure clo;
                clo.protoId = protoId;
                int cloId = allocClosure(std::move(clo));
                stack.push_back(ZeusType::ClosureHandle{cloId});
                // The nested call machinery releases every argument slot it pops,
                // so the stack has to own a reference for the receiver it pushes.
                ZeusGC::retainVariantDeep(*this, withObj);
                stack.push_back(withObj);  // this 参数
                // Dispatch through a real frame: executeClosureSync pushes a
                // CallFrame and runs it to completion, so the callee gets its own
                // local slots. A recursive execute() would share this frame's local
                // buffer and leave the callee's slots behind in it.
                if (!executeClosureSync(cloId, 1)) {
                    if (vmstat != ZeusType::VMStatus::Running) break;
                    VMException::VMException exp("WITH_BEGIN: operator[with] call failed", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 弹出 operator[with]() 的返回值（不需要使用）
                if (!stack.empty()) {
                    stack.pop_back();
                }

                // 检查是否有 operator[~with] 方法
                bool hasExitMethod = (classPtr->operatorMethods.find("[~with]") != classPtr->operatorMethods.end());

                // 保存 with 上下文
                WithContext ctx;
                ctx.objHandle = oh;
                ctx.hasExitMethod = hasExitMethod;
                withContextStack.push_back(ctx);

                ZEUS_DEBUG_PRINT("[VM] WITH_BEGIN: entered with block, hasExitMethod=%d", hasExitMethod);
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_WITH_END(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::WITH_END: {
                // WITH_END: 结束 with 块
                if (withContextStack.empty()) {
                    VMException::VMException exp("WITH_END: no active with block", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                WithContext ctx = withContextStack.back();
                withContextStack.pop_back();

                if (!invokeWithExit(ctx, instr)) break;

                ZEUS_DEBUG_PRINT("[VM] WITH_END: exited with block");
                break;
            }
        }
    }

    // 调用 operator[~with]。正常退出和异常展开共用这一份，避免两条路径的
    // 语义分叉（比如只有一条路做了 retain/release）。
    // 返回 false 表示调用过程本身抛了异常，调用方必须停止当前操作。
    ZEUS_VM_NOINLINE bool invokeWithExit(
        const WithContext& ctx,
        const IRType::IRInstr& instr
    ) {
        // 如果有 operator[~with] 方法，调用它
        if (!ctx.hasExitMethod) return true;

        auto* obj = getValidObject(ctx.objHandle);
        if (!obj) return true;
        auto* classPtr = getClass(obj->classId);
        if (!classPtr) return true;

        auto opIt = classPtr->operatorMethods.find("[~with]");
        if (opIt == classPtr->operatorMethods.end()) return true;

        int protoId = opIt->second;
        Closure clo;
        clo.protoId = protoId;
        int cloId = allocClosure(std::move(clo));
        stack.push_back(ZeusType::ClosureHandle{cloId});
        // Same ownership rule as WITH_BEGIN: the nested call releases the
        // receiver slot it pops.
        ZeusGC::retainVariantDeep(
            *this, ZeusType::ObjectHandle{ctx.objHandle.id, ctx.objHandle.generation});
        stack.push_back(ZeusType::ObjectHandle{ctx.objHandle.id, ctx.objHandle.generation});
        // Dispatch through a real frame: executeClosureSync pushes a CallFrame
        // and runs it to completion, so the callee gets its own local slots
        // instead of sharing this frame's buffer.
        if (!executeClosureSync(cloId, 1)) {
            if (vmstat != ZeusType::VMStatus::Running) return false;
            VMException::VMException exp(
                "WITH_END: operator[~with] call failed", __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        // 弹出 operator[~with]() 的返回值（不需要使用）
        if (!stack.empty()) {
            stack.pop_back();
        }
        return true;
    }
