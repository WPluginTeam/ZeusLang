// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_STORE_VALUE_LOCAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::STORE_VALUE_LOCAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                const int __varIndexInt = VMHelpers::getIntFromValue(__varIndex);

                auto const& __mutOrConst = std::get<std::string>(instr.args[1]);

                if (local.size() <= (size_t)__varIndexInt) {
                    local.resize(__varIndexInt + 1, nullptr);
                }

                const std::string& nativeType = instr.metadata.typeName;
                if (ZeusBuiltins::findNativeScalarCanonical(nativeType)) {
                    stack.back() = normalizeNativeScalarValue(nativeType, stack.back());
                }
                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                // local Cell 将持有新值的一份引用
                ZeusGC::retainVariantDeep(*this, newVal);

                if (!local[__varIndexInt]) {
                    local[__varIndexInt] = newCell(newVal, (__mutOrConst == "const") ? 1 : 0, "STORE_LOCAL");
                } else {
                    if (local[__varIndexInt]->flags & 0x1) {
                        VMException::VMException exp("Attempt to modify a const local variable at index " + std::to_string(__varIndexInt), __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    const auto oldVal = local[__varIndexInt]->readValue();
                    ZeusGC::releaseVariantDeep(*this, oldVal);

                    local[__varIndexInt]->writeValue(newVal);

                    if (__mutOrConst == "const") {
                        local[__varIndexInt]->flags |= 0x1;
                    } else {
                        local[__varIndexInt]->flags &= ~0x1;
                    }
                }

                configureNativeScalarCell(local[__varIndexInt], nativeType);

                // 丢弃栈持有的那一份引用（local cell 已 retain 一份）
                dropStackItem(newItem);

                printLocals();
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_STORE_VALUE_GLOBAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::STORE_VALUE_GLOBAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                const int __varIndexInt = VMHelpers::getIntFromValue(__varIndex);
                auto const& __mutOrConst = std::get<std::string>(instr.args[1]);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: index=%d, mutOrConst='%s', stack.size=%zu",
                    __varIndexInt, __mutOrConst.c_str(), stack.size());

                // 可选 inline 名称（来自 .zir），用于在 globalNames 中记录符号名称以便后续查找
                std::string inlineName;
                if (instr.args.size() > 2 && std::holds_alternative<std::string>(instr.args[2])) {
                    inlineName = std::get<std::string>(instr.args[2]);
                    if (__varIndexInt >= 0) {
                        if (__varIndexInt >= (int)globalNames.size()) globalNames.resize(__varIndexInt+1);
                        if (globalNames[__varIndexInt].empty()) globalNames[__varIndexInt] = inlineName;
                    }
                }

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Getting stack.back()...");
                const std::string& nativeType = instr.metadata.typeName;
                if (ZeusBuiltins::findNativeScalarCanonical(nativeType)) {
                    stack.back() = normalizeNativeScalarValue(nativeType, stack.back());
                }
                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value; // 将栈顶值视为要存储的新值（可能包含 closure）

                // global Cell 将持有新值的一份引用
                ZeusGC::retainVariantDeep(*this, newVal);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Resizing global if needed (current size=%zu)...", global.size());
                if (global.size() <= __varIndexInt) {
                    global.resize(__varIndexInt + 1, nullptr);
                }

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Setting global[%d]...", __varIndexInt);
                if (!global[__varIndexInt]) {
                    global[__varIndexInt] = newCell(newVal, (__mutOrConst == "const") ? 1 : 0, "STORE_GLOBAL");
                    ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Created new cell with flags=%d", global[__varIndexInt]->flags);
                } else {
                    ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Cell exists, flags=%d, mutOrConst=%s",
                        global[__varIndexInt]->flags, __mutOrConst.c_str());

                    if (global[__varIndexInt]->flags & 0x1) {
                        VMException::VMException exp("Attempt to modify a const global variable at index " + std::to_string(__varIndexInt), __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    const auto oldVal = global[__varIndexInt]->readValue();
                    ZeusGC::releaseVariantDeep(*this, oldVal);

                    global[__varIndexInt]->writeValue(newVal);

                    if (__mutOrConst == "const") {
                        global[__varIndexInt]->flags |= 0x1;
                    } else {
                        global[__varIndexInt]->flags &= ~0x1;
                    }
                }

                configureNativeScalarCell(global[__varIndexInt], nativeType);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Popping stack...");
                // 释放栈临时持有的对象引用（global cell 已 retain 一份）
                dropStackItem(newItem);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: tryRegisterGlobalClosure...");
                // 如果新值是 closure，则尝试在 nameToClosure/register 中注册
                tryRegisterGlobalClosure(__varIndexInt, newVal);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: printGlobals...");
                printGlobals();

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: printStack...");
                printStack();

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Done!");
                // TODO: future: 如果在注册 closure 时需要自动记录名称（nameToClosure），
                // 需要考虑名字冲突、作用域以及 late-resolve 的一致性。目前通过
                // tryRegisterGlobalClosure 做部分处理，后续可在这里统一改进。
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_ASSIGN_VALUE_GLOBAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::ASSIGN_VALUE_GLOBAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                int idx = VMHelpers::getIntFromValue(__varIndex);

                // 处理 inline 名称参数（index, mut/const, name?）
                if (instr.args.size() > 2 && std::holds_alternative<std::string>(instr.args[2])) {
                    if (idx >= 0) {
                        if (idx >= (int)globalNames.size()) {
                            globalNames.resize(idx + 1);
                        }
                        if (globalNames[idx].empty()) {
                            globalNames[idx] = std::get<std::string>(instr.args[2]);
                        }
                    }
                }

                if (idx < 0 || idx >= (int)global.size() || !global[idx]) {
                    VMException::VMException exp("ASSIGN global slot invalid: " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (global[idx]->flags & 0x1) {
                    std::string varName = (idx >= 0 && idx < (int)globalNames.size())
                        ? globalNames[idx] : ("g[" + std::to_string(idx) + "]");

                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1301_CONST_ASSIGNMENT,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "Cannot assign to constant '" + varName + "'",
                        __line, instr
                    );
                    exp.setDetails("The variable '" + varName + "' was declared as 'const' and cannot be modified");
                    exp.setSuggestion("If you need to modify this variable, declare it with 'let' instead of 'const'");
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN global stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                global[idx]->validateValue(stack.back());
                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                const auto referenceWrite = writeThroughReference(global[idx], newVal);
                if (referenceWrite != ReferenceWriteResult::NotReference) {
                    if (referenceWrite != ReferenceWriteResult::Stored) {
                        VMException::VMException exp(
                            referenceWrite == ReferenceWriteResult::ConstTarget
                                ? "Cannot assign through a reference to a const cell"
                                : "Cannot assign this value through the reference",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                    }
                    dropStackItem(newItem);
                    printGlobals();
                    printStack();
                    break;
                }

                const auto oldVal = global[idx]->readValue();
                ZeusGC::releaseVariantDeep(*this, oldVal);
                ZeusGC::retainVariantDeep(*this, newVal);

                global[idx]->writeValue(newVal);

                // TODO: 如果可能，应在 nameToClosure 中记录名称（当新值为 closure 时），以便 later-resolve
                tryRegisterGlobalClosure(idx, newVal);
                // 释放栈临时持有的对象引用（slot 已 retain）
                dropStackItem(newItem);

                printGlobals();
                printStack();
                // TODO: 需要将名称注册到 nameToClosure（当新值为 closure 时），以便 later-resolve
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_ASSIGN_VALUE_LOCAL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::ASSIGN_VALUE_LOCAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                int idx = VMHelpers::getIntFromValue(__varIndex);

                if (idx < 0 || idx >= (int)local.size() || !local[idx]) {
                    VMException::VMException exp("ASSIGN local slot invalid: " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (local[idx]->flags & 0x1) {
                    VMException::VMException exp("Assign to const local index " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN local stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                local[idx]->validateValue(stack.back());
                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                const auto referenceWrite = writeThroughReference(local[idx], newVal);
                if (referenceWrite != ReferenceWriteResult::NotReference) {
                    if (referenceWrite != ReferenceWriteResult::Stored) {
                        VMException::VMException exp(
                            referenceWrite == ReferenceWriteResult::ConstTarget
                                ? "Cannot assign through a reference to a const cell"
                                : "Cannot assign this value through the reference",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                    }
                    dropStackItem(newItem);
                    printLocals();
                    printStack();
                    break;
                }

                const auto oldVal = local[idx]->readValue();
                ZeusGC::releaseVariantDeep(*this, oldVal);
                ZeusGC::retainVariantDeep(*this, newVal);

                local[idx]->writeValue(newVal);
                // 释放栈临时持有的对象引用（slot 已 retain）
                dropStackItem(newItem);
                printLocals();
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_ASSIGN_VALUE_UPVALUE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::ASSIGN_VALUE_UPVALUE: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                int idx = VMHelpers::getIntFromValue(__varIndex);

                if (idx < 0 || idx >= (int)upvalue.size() || !upvalue[idx]) {
                    VMException::VMException exp("ASSIGN upvalue slot invalid: " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (upvalue[idx]->flags & 0x1) {
                    VMException::VMException exp("Assign to const upvalue index " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN upvalue stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                upvalue[idx]->validateValue(stack.back());
                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                const auto referenceWrite = writeThroughReference(upvalue[idx], newVal);
                if (referenceWrite != ReferenceWriteResult::NotReference) {
                    if (referenceWrite != ReferenceWriteResult::Stored) {
                        VMException::VMException exp(
                            referenceWrite == ReferenceWriteResult::ConstTarget
                                ? "Cannot assign through a reference to a const cell"
                                : "Cannot assign this value through the reference",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                    }
                    dropStackItem(newItem);
                    printVarStack(2);
                    printStack();
                    break;
                }

                const auto oldVal = upvalue[idx]->readValue();
                ZeusGC::releaseVariantDeep(*this, oldVal);
                ZeusGC::retainVariantDeep(*this, newVal);

                upvalue[idx]->writeValue(newVal);
                // 释放栈临时持有的对象引用（slot 已 retain）
                dropStackItem(newItem);
                printVarStack(2);
                printStack();
                break;
            }

            // ===== 共享堆操作 =====
        }
    }

    ZEUS_VM_NOINLINE void execute_STORE_VALUE_SHARED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::STORE_VALUE_SHARED: {
                // STORE_VALUE_SHARED "varName" "mut/const"
                if (instr.args.size() < 2) {
                    VMException::VMException exp("STORE_VALUE_SHARED missing arguments", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                std::string mutOrConst = std::get<std::string>(instr.args[1]);

                if (stack.empty()) {
                    VMException::VMException exp("STORE_VALUE_SHARED stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem incoming = popStackItem();
                auto cell = ZeusShared::getGlobalHeap().getOrCreate(
                    varName, ZeusType::Variant(ZeusType::Null{}));
                // Shared bindings hold the same class instance as local/global
                // bindings. The cell owns one reference independently of the
                // incoming stack slot; there is no serialized second owner.
                assignSharedCellValue(cell, incoming.value);
                dropStackItem(incoming);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_SHARED: name='%s', mutOrConst='%s'",
                    varName.c_str(), mutOrConst.c_str());

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOAD_VALUE_SHARED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOAD_VALUE_SHARED: {
                // LOAD_VALUE_SHARED "varName"
                if (instr.args.empty()) {
                    VMException::VMException exp("LOAD_VALUE_SHARED missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);

                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("LOAD_VALUE_SHARED: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // Keep reading and retaining indivisible with replacement:
                // another VM must not release the shared owner in between.
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                ZeusType::UniversalVariant val = cell->read();

                // 检查是否是序列化的对象（Variant 中的 Object 带 __className__）
                if (std::holds_alternative<ZeusType::Variant>(val)) {
                    auto& variantVal = std::get<ZeusType::Variant>(val);

                    if (std::holds_alternative<ZeusType::Object>(variantVal)) {
                        auto& obj = std::get<ZeusType::Object>(variantVal);

                        if (obj.impl && obj.impl->data.find("__className__") != obj.impl->data.end()) {
                            // 这是一个共享对象，返回 SharedObjectHandle
                            stack.push_back(ZeusType::SharedObjectHandle{varName, cell->generation});

                            ZEUS_DEBUG_PRINT("[VM] LOAD_VALUE_SHARED: name='%s' (shared object)", varName.c_str());
                            printStack();
                            break;
                        }
                    }
                }

                // The stack slot claims ownership of the loaded value, so hand it
                // its own reference while the shared cell keeps holding one too.
                ZeusGC::retainVariantDeep(*this, val);
                stack.push_back(val);

                ZEUS_DEBUG_PRINT("[VM] LOAD_VALUE_SHARED: name='%s'", varName.c_str());

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_ASSIGN_VALUE_SHARED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::ASSIGN_VALUE_SHARED: {
                // ASSIGN_VALUE_SHARED "varName"
                if (instr.args.empty()) {
                    VMException::VMException exp("ASSIGN_VALUE_SHARED missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN_VALUE_SHARED stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("ASSIGN_VALUE_SHARED: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem incoming = popStackItem();
                assignSharedCellValue(cell, incoming.value);
                dropStackItem(incoming);

                ZEUS_DEBUG_PRINT("[VM] ASSIGN_VALUE_SHARED: name='%s'", varName.c_str());

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_SHARED_LOCK(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::SHARED_LOCK: {
                // SHARED_LOCK "varName" - 获取共享变量的写锁
                if (instr.args.empty()) {
                    VMException::VMException exp("SHARED_LOCK missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("SHARED_LOCK: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                cell->lockWrite();

                ZEUS_DEBUG_PRINT("[VM] SHARED_LOCK: name='%s' locked", varName.c_str());
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_SHARED_UNLOCK(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::SHARED_UNLOCK: {
                // SHARED_UNLOCK "varName" - 释放共享变量的写锁
                if (instr.args.empty()) {
                    VMException::VMException exp("SHARED_UNLOCK missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("SHARED_UNLOCK: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                cell->unlockWrite();

                ZEUS_DEBUG_PRINT("[VM] SHARED_UNLOCK: name='%s' unlocked", varName.c_str());
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOAD_CELL_REFERENCE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOAD_CELL_REFERENCE: {
                if (instr.args.size() < 2 || !std::holds_alternative<std::string>(instr.args[0]) ||
                    !std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    VMException::VMException exp("LOAD_CELL_REFERENCE requires a scope and index", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                const auto& scope = std::get<std::string>(instr.args[0]);
                const int index = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));
                if (scope != "global") {
                    VMException::VMException exp(
                        "reference return requires stable global storage; local and upvalue references cannot escape",
                        __line, instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                if (index < 0 || index >= static_cast<int>(global.size()) || !global[index]) {
                    VMException::VMException exp("LOAD_CELL_REFERENCE global slot is invalid", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                stack.push_back(ZeusType::ReferenceHandle{global[index], "cell"});
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LOAD_VALUE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LOAD_VALUE: {
                // Support multiple IR forms:
                // 1) [ string name ] -> resolve & load variable (late-resolve / global name lookup)
                // 2) [ LexerType::Value literal ] -> push literal value (for late-resolve class names etc.)
                // 3) [ string scope, LexerType::Value index, (optional string name) ] -> load cell

                // Form (2): single LexerType::Value argument - push as literal
                if (instr.args.size() == 1 && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    stack.push_back(std::get<LexerType::Value>(instr.args[0]));
                    printStack();
                    break;
                }

                if (instr.args.size() == 1 && std::holds_alternative<IRType::NullLiteral>(instr.args[0])) {
                    stack.push_back(ZeusType::Variant(ZeusType::Null()));
                    printStack();
                    break;
                }

                // Form (1): single string argument - resolve variable name
                if (instr.args.size() == 1 && std::holds_alternative<std::string>(instr.args[0])) {
                    // form (1) - resolve variable name to its value (top-level/global expected)
                    std::string name = std::get<std::string>(instr.args[0]);
                    bool found = false;

                    // 类型实现的静态方法通过类型名调用：int.twice(3)
                    // 编译期把被调用者记成限定名 "TypeName::method"。
                    const size_t separator = name.rfind("::");
                    const bool isTypeStaticName =
                        separator != std::string::npos && separator > 0 &&
                        separator + 2 < name.size();
                    if (isTypeStaticName) {
                        const std::string typeName = name.substr(0, separator);
                        const std::string methodName = name.substr(separator + 2);

                        const auto implementation =
                            typeImplementations.find(typeName);
                        if (implementation != typeImplementations.end()) {
                            const auto method =
                                implementation->second.staticMethods.find(
                                    methodName);
                            if (method !=
                                implementation->second.staticMethods.end() &&
                                getPrototype(method->second)) {
                                Closure clo;
                                clo.protoId = method->second;
                                clo.autoRelease = true;
                                const int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                found = true;
                            }
                        }

                        if (!found) {
                            // 回退到该类型的运行时类值：暴露类型的类(例如
                            // `class string {}`)上的静态方法仍然可用类型名调用。
                            for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                                const std::string& fullName = globalNames[gi];
                                const size_t dotPos = fullName.rfind('.');
                                const bool matches = fullName == typeName ||
                                    (dotPos != std::string::npos &&
                                     fullName.substr(dotPos + 1) == typeName);
                                if (!matches || gi >= (int)global.size() ||
                                    !global[gi]) {
                                    continue;
                                }
                                const ZeusType::UniversalVariant value =
                                    loadCellValue(global[gi]);
                                const auto* classHandle =
                                    std::get_if<ZeusType::ClassHandle>(&value);
                                if (classHandle == nullptr) continue;
                                auto* classPtr = getClass(classHandle->id);
                                if (classPtr == nullptr) continue;
                                const auto method =
                                    classPtr->staticMethods.find(methodName);
                                if (method == classPtr->staticMethods.end() ||
                                    !getPrototype(method->second)) {
                                    continue;
                                }
                                Closure clo;
                                clo.protoId = method->second;
                                clo.autoRelease = true;
                                const int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                found = true;
                                break;
                            }
                        }

                        if (!found) {
                            VMException::VMException exp(
                                "unknown static method '" + methodName +
                                    "' for type '" + typeName + "'",
                                __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        printStack();
                        break;
                    }

                    if (!globalNames.empty()) {
                        // First try: exact match
                        for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                            if (globalNames[gi] == name) {
                                if (gi < (int)global.size() && global[gi]) {
                                    pushLoadedCellValue(global[gi]);
                                    found = true;
                                }
                                break;
                            }
                        }

                        // Second try: match with package prefix (for module-internal references)
                        // e.g., "Bytes" can match "zeus.socket.Bytes"
                        if (!found) {
                            for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                                const std::string& fullName = globalNames[gi];

                                // Check if fullName ends with ".name"
                                size_t dotPos = fullName.rfind('.');

                                if (dotPos != std::string::npos) {
                                    std::string suffix = fullName.substr(dotPos + 1);

                                    if (suffix == name) {
                                        if (gi < (int)global.size() && global[gi]) {
                                            pushLoadedCellValue(global[gi]);
                                            found = true;
                                            vmMessage(std::string("[LOAD_VALUE] Resolved '") + name + "' to '" + fullName + "'");
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (!found) {
                        VMException::VMException exp(std::string("NameError: undefined variable '") + name + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    printStack();
                    break;
                }

                // Form (3): scope + index format
                auto const& kind = std::get<std::string>(instr.args[0]);
                auto const& indexVal = std::get<LexerType::Value>(instr.args[1]);
                int index = VMHelpers::getIntFromValue(indexVal);

                if (kind == "local") {
                    if (index >= 0 && index < (int)local.size() && local[index]) {
                        pushLoadedCellValue(local[index]);
                    } else {
                        std::string varname = "<unknown>";
                        // 尝试从args[2]获取变量名
                        if (instr.args.size() >= 3 && std::holds_alternative<std::string>(instr.args[2])) {
                            varname = std::get<std::string>(instr.args[2]);
                        }
                        // local变量没有globalNames映射，保持<unknown>
                        VMException::VMException exp(
                            std::string("NameError: undefined variable '") + varname + "'",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (kind == "global") {
                    if (index >= 0 && index < (int)global.size() && global[index]) {
                        const auto loadedVal = global[index]->readValue();
                        // DEBUG: 检查 global[46] 的值
                        if (index == 46) {
                            std::string valType = "unknown";
                            if (std::holds_alternative<ZeusType::ClosureHandle>(loadedVal)) {
                                valType = "ClosureHandle(" + std::to_string(std::get<ZeusType::ClosureHandle>(loadedVal).id) + ")";
                            } else if (std::holds_alternative<LexerType::Value>(loadedVal)) {
                                valType = "Value: " + ZeusVMUtils::valueToDebugString(loadedVal);
                            } else {
                                valType = "OtherType";
                            }
                            ZEUS_DEBUG_PRINT("[LOAD_VALUE global 46] Cell=%p, refCount=%d, value=%s",
                                (void*)global[index], global[index]->refCount, valType.c_str());
                        }
                        pushLoadedCellValue(global[index]);
                    } else {
                        std::string varname = "<unknown>";
                        // 尝试从args[2]获取变量名
                        if (instr.args.size() >= 3 && std::holds_alternative<std::string>(instr.args[2])) {
                            varname = std::get<std::string>(instr.args[2]);
                        }
                        // 如果args[2]不存在，尝试从globalNames查找
                        else if (index >= 0 && index < (int)globalNames.size()) {
                            varname = globalNames[index];
                        }
                        VMException::VMException exp(std::string("NameError: undefined variable '") + varname + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (kind == "upvalue") {
                    if (index >= 0 && index < (int)upvalue.size() && upvalue[index]) {
                        pushLoadedCellValue(upvalue[index]);
                    } else {
                        std::string varname = "<unknown>";
                        // 尝试从args[2]获取变量名
                        if (instr.args.size() >= 3 && std::holds_alternative<std::string>(instr.args[2])) {
                            varname = std::get<std::string>(instr.args[2]);
                        }
                        // upvalue变量没有globalNames映射，保持<unknown>
                        VMException::VMException exp(std::string("NameError: undefined variable '") + varname + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else {
                    // 直接把 kind（字面量或标识）压入栈
                    stack.push_back(kind);
                }

                printStack();
                break;
            }
        }
    }
