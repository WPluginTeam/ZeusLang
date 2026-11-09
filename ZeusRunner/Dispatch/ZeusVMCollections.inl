// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_LIST_BUILD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LIST_BUILD: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // LIST_BUILD 指令的第一个参数 args[0] 为元素数量（int Value）
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("LIST_BUILD missing elem count", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                int n = 0;
                const auto &val = std::get<LexerType::Value>(instr.args[0]);
                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) n = VMHelpers::getIntFromValue(val);
                // 检查栈中是否有足够的元素
                if (n > 0 && stack.size() < (size_t)n) {
                    VMException::VMException exp("LIST_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                ZeusType::Array array;
                array.impl = ZeusGC::makeArrayData();
                if (n > 0) {
                    array.impl->data.resize(static_cast<size_t>(n));
                    for (int i = n - 1; i >= 0; --i) {
                        StackItem item = popStackItem();
                        array.impl->data[static_cast<size_t>(i)] = std::move(item.value);
                        // The element's existing stack ownership transfers to
                        // ArrayData; releasing it here would drop the value
                        // while the array is taking ownership of it.
                        item.owner = 0;
                    }
                }
                stack.push_back(ZeusType::Variant(std::move(array)));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_DICT_BUILD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::DICT_BUILD: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // DICT_BUILD 的第一个参数 args[0] 为键值对数量（pairCount，int）
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("DICT_BUILD missing pair count", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                int pairCount = 0;
                const auto &pcVal = std::get<LexerType::Value>(instr.args[0]);
                if (std::holds_alternative<ZeusType::BigInteger>(pcVal.data)) pairCount = VMHelpers::getIntFromValue(pcVal);
                if (pairCount < 0) {
                    VMException::VMException exp("DICT_BUILD negative pair count", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                size_t need = (size_t)pairCount * 2;
                if (stack.size() < need) {
                    VMException::VMException exp("DICT_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 更新调试计数（初始键值对数）
                debugDictInitialPairs += (size_t)pairCount;
                // 从栈中弹出 2N 个元素，顺序为: valN, keyN, ..., val1, key1
                std::vector<ZeusType::UniversalVariant> tmp;
                tmp.reserve(need);
                for (size_t i = 0; i < need; ++i) {
                    tmp.push_back(stack.back());
                    stack.pop_back();
                }
                // tmp: [valN, keyN, ..., val1, key1]
                auto objData = ZeusGC::makeObjectData();
                for (int i = 0; i < pairCount; ++i) {
                    // tmp 中的排列: [valN, keyN, ..., val1, key1]
                    // 计算第 i 对应的 key 和 value 在 tmp 中的位置
                    // key_i 在 tmp 的索引为 need - 1 - (i * 2)
                    // value_i 在 tmp 的索引为 need - 2 - (i * 2)
                    auto &keyVar = tmp[need - 1 - (i * 2)];
                    auto &valVar = tmp[need - 2 - (i * 2)];

                    const std::string keyStr = dictionaryKeyString(keyVar);
                    auto it = (*objData).data.find(keyStr);
                    if (it == (*objData).data.end()) {
                        (*objData).setValue(keyStr, valVar);
                    } else {
                        it->second = valVar; // 覆盖已有键的值
                    }
                }
                ZeusType::Object objWrap; objWrap.impl = objData;
                ZeusType::UniversalVariant uv = objWrap;
                stack.push_back(uv);
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_DICT_KEYS_BUILD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::DICT_KEYS_BUILD: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                if (stack.empty()) {
                    VMException::VMException exp("DICT_KEYS_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                StackItem topItem = popStackItem();
                ZeusType::UniversalVariant top = topItem.value;
                // expect a dictionary/object
                ZeusType::Array arr; arr.impl = ZeusGC::makeArrayData();
                if (std::holds_alternative<ZeusType::Variant>(top)) {
                    const ZeusType::Variant &var = std::get<ZeusType::Variant>(top);
                    if (std::holds_alternative<ZeusType::Object>(var)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(var);
                        if (obj.impl) {
                            for (std::size_t i = 0; i < obj.impl->order.size(); ++i) {
                                if (!obj.impl->isLiveOrderEntry(i)) continue;
                                arr.impl->data.push_back(
                                    LexerType::Value(obj.impl->order[i]));
                            }
                        }
                    }
                }
                dropStackItem(topItem);
                ZeusType::Variant v = arr;
                stack.push_back(ZeusType::UniversalVariant(v));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_DICT_LOAD_VALUE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::DICT_LOAD_VALUE: {
                if (stack.size() < 2) {
                    VMException::VMException exp("DICT_LOAD_VALUE stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                StackItem keyItem = popStackItem();
                StackItem dictItem = popStackItem();
                ZeusType::UniversalVariant keyVar = keyItem.value;
                ZeusType::UniversalVariant dictVar = dictItem.value;

                struct DropInputs {
                    VM* vm;
                    StackItem* a;
                    StackItem* b;
                    ~DropInputs() {
                        if (vm && a) vm->dropStackItem(*a);
                        if (vm && b) vm->dropStackItem(*b);
                    }
                } dropInputs{this, &keyItem, &dictItem};

                const ZeusType::UniversalVariant* operatorArguments[] = {
                    &dictVar, &keyVar
                };
                if (tryDispatchTypeImplementationOperator(
                        "[[]]", operatorArguments, 2, instr, nextInstruction)) break;

                ZeusType::UniversalVariant valueOut = ZeusType::Null();
                if (const auto* range =
                        std::get_if<ZeusType::RawBytesRange>(&dictVar)) {
                    int64_t index = 0;
                    if (!ZeusVMUtils::tryGetInt64(keyVar, index)) {
                        VMException::VMException(
                            "raw bytes range index must be an int", __line, instr)
                            ._throw(vmstat, currentChunk);
                        break;
                    }
                    if (index < 0 || static_cast<uint64_t>(index) >= range->length) {
                        VMException::VMException(
                            "raw bytes range index out of range", __line, instr)
                            ._throw(vmstat, currentChunk);
                        break;
                    }
                    const uintptr_t address = range->address +
                        static_cast<uintptr_t>(index);
                    std::string error;
                    if (!ZeusVMAddress::fetch(
                            reinterpret_cast<void*>(address), "int",
                            valueOut, error, 1)) {
                        VMException::VMException(error, __line, instr)
                            ._throw(vmstat, currentChunk);
                        break;
                    }
                    stack.push_back(std::move(valueOut));
                    break;
                }

                // default missing value -> nil (ZeusType::Null)
                // 处理下标/键访问（数组或对象）
                if (std::holds_alternative<ZeusType::Variant>(dictVar)) {
                    const ZeusType::Variant &dv = std::get<ZeusType::Variant>(dictVar);
                    if (std::holds_alternative<ZeusType::Array>(dv)) {
                        // 数组访问：使用整数索引
                        int idx = 0;
                        bool idxIsInt = false;
                        if (std::holds_alternative<LexerType::Value>(keyVar)) {
                            const auto &iv = std::get<LexerType::Value>(keyVar);
                            if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) {
                                idx = VMHelpers::getIntFromValue(iv);
                                idxIsInt = true;
                            }
                        }
                        if (!idxIsInt) {
                            VMException::VMException exp("TypeError: array index must be integer", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }

                        // 进行边界检查
                        const ZeusType::Array &arr = std::get<ZeusType::Array>(dv);
                        if (!arr.impl || idx < 0 || (size_t)idx >= arr.impl->data.size()) {
                            std::string msg = "Array index out of bounds: index=" + std::to_string(idx) +
                                            ", size=" + std::to_string(arr.impl ? arr.impl->data.size() : 0);
                            VMException::VMException exp(msg, __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }

                        valueOut = ZeusVMUtils::arrayRead(dictVar, idx);
                    }
                    else if (std::holds_alternative<ZeusType::Object>(dv)) {
                        // 对象访问：使用字符串键
                        const std::string keyStr = dictionaryKeyString(keyVar);
                            const ZeusType::Object &obj = std::get<ZeusType::Object>(dv);
                            if (obj.impl) {
                                auto it = obj.impl->data.find(keyStr);
                                if (it != obj.impl->data.end()) {
                                    valueOut = it->second;
                                } else {
                                    // Missing key -> raise an error instead of returning nil
                                    std::string msg = "KeyError: missing key '" + keyStr + "'";
                                    VMException::VMException exp(msg, __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                            } else {
                                // Empty object (no impl) -> missing key
                                std::string msg = "KeyError: missing key '" + keyStr + "'";
                                VMException::VMException exp(msg, __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                    }
                } else if (std::holds_alternative<ZeusType::ObjectHandle>(dictVar)) {
                    // 类实例：尝试调用 operator[[]]
                    ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(dictVar);
                    auto* objPtr = getValidObject(objHandle);
                    if (!objPtr) {
                        VMException::VMException exp("DICT_LOAD_VALUE: object has been freed or reused (stale reference)", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    int classId = objPtr->classId;
                    auto* classPtr = getClass(classId);
                    if (!classPtr) {
                        VMException::VMException exp("DICT_LOAD_VALUE invalid class id", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& classInfo = *classPtr;
                    auto opIt = classInfo.operatorMethods.find("[[]]");
                    if (opIt == classInfo.operatorMethods.end()) {
                        VMException::VMException exp("TypeError: class '" + classInfo.name + "' does not support operator[[]]", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    int protoId = opIt->second;
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("DICT_LOAD_VALUE invalid operator[[]] protoId", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                     // 调用 operator[[]](key)
                     // 栈布局: [ closure, this, key ]
                     Closure clo;
                     clo.protoId = protoId;
                     clo.autoRelease = true;
                     int cloId = allocClosure(std::move(clo));

                    stack.push_back(ZeusType::ClosureHandle{cloId});  // callee
                    // dictItem/keyItem stay owned by DropInputs through the
                    // synchronous call; retain the copies placed on the VM stack.
                    ZeusGC::retainVariantDeep(*this, dictVar);
                    stack.push_back(objHandle);                       // this (完整 ObjectHandle)
                    ZeusGC::retainVariantDeep(*this, keyVar);
                    stack.push_back(keyVar);                          // key argument

                     // 调用 CALL_CLOSURE (argc=2: this + key)
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
                     break;  // 结果已在栈顶
                 } else {
                    // dictVar 不是 Variant 也不是 ObjectHandle，生成更友好的类型错误信息
                    std::string msg = "TypeError: attempted subscript on non-container";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                ZeusGC::retainVariantDeep(*this, valueOut);
                stack.push_back(valueOut);
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_DICT_STORE_VALUE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::DICT_STORE_VALUE: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                if (stack.size() < 3) {
                    VMException::VMException exp("DICT_STORE_VALUE stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                debugDictStoreCount++;
                StackItem newItem = popStackItem();
                StackItem keyItem = popStackItem();
                StackItem dictItem = popStackItem();
                ZeusType::UniversalVariant& newVal = newItem.value;
                ZeusType::UniversalVariant& keyVar = keyItem.value;
                ZeusType::UniversalVariant& dictVar = dictItem.value;
                struct DropInputs {
                    VM* vm;
                    StackItem* first;
                    StackItem* second;
                    StackItem* third;
                    ~DropInputs() {
                        if (vm && first) vm->dropStackItem(*first);
                        if (vm && second) vm->dropStackItem(*second);
                        if (vm && third) vm->dropStackItem(*third);
                    }
                } dropInputs{this, &newItem, &keyItem, &dictItem};

                const ZeusType::UniversalVariant* operatorArguments[] = {
                    &dictVar, &keyVar, &newVal
                };
                if (tryDispatchTypeImplementationOperator(
                        "[[]=]", operatorArguments, 3, instr, nextInstruction)) break;

                if (const auto* range =
                        std::get_if<ZeusType::RawBytesRange>(&dictVar)) {
                    int64_t index = 0;
                    int64_t byteValue = 0;
                    if (!ZeusVMUtils::tryGetInt64(keyVar, index) ||
                        !ZeusVMUtils::tryGetInt64(newVal, byteValue)) {
                        VMException::VMException(
                            "raw bytes range assignment requires int index and value",
                            __line, instr)._throw(vmstat, currentChunk);
                        break;
                    }
                    if (index < 0 || static_cast<uint64_t>(index) >= range->length) {
                        VMException::VMException(
                            "raw bytes range index out of range", __line, instr)
                            ._throw(vmstat, currentChunk);
                        break;
                    }
                    if (byteValue < 0 || byteValue > 255) {
                        VMException::VMException(
                            "raw bytes range assignment requires a value from 0 to 255",
                            __line, instr)._throw(vmstat, currentChunk);
                        break;
                    }
                    const uintptr_t address = range->address +
                        static_cast<uintptr_t>(index);
                    std::string error;
                    if (!ZeusVMAddress::store(
                            reinterpret_cast<void*>(address), "int",
                            newVal, error, 1)) {
                        VMException::VMException(error, __line, instr)
                            ._throw(vmstat, currentChunk);
                        break;
                    }
                    stack.push_back(std::move(newItem.value));
                    newItem.owner = 0;
                    break;
                }

                if (std::holds_alternative<ZeusType::Variant>(dictVar)) {
                    ZeusType::Variant &dv = std::get<ZeusType::Variant>(dictVar);

                    if (std::holds_alternative<ZeusType::Array>(dv)) {
                        int idx = 0;
                        if (std::holds_alternative<LexerType::Value>(keyVar)) {
                            const auto &iv = std::get<LexerType::Value>(keyVar);
                            if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) idx = VMHelpers::getIntFromValue(iv);
                        }
                        auto* array = ZeusBuiltins::getArrayPtr(dictVar);
                        if (array && array->impl && idx >= 0 &&
                            idx < static_cast<int>(array->impl->data.size())) {
                            const auto oldValue = array->impl->data[idx];
                            const size_t ownerCount = array->impl->valueRefCount.load(
                                std::memory_order_relaxed);
                            if (ZeusVMUtils::arrayWrite(dictVar, idx, newVal)) {
                                for (size_t i = 0; i < ownerCount; ++i) {
                                    ZeusGC::retainVariantDeep(*this, newVal);
                                    ZeusGC::releaseVariantDeep(*this, oldValue);
                                }
                            }
                        }
                    }
                    else if (std::holds_alternative<ZeusType::Object>(dv)) {
                        const std::string keyStr = dictionaryKeyString(keyVar);
                        ZeusType::Object &obj = std::get<ZeusType::Object>(dv);
                        if (obj.impl) {
                            auto it2 = obj.impl->data.find(keyStr);
                            const size_t ownerCount = obj.impl->valueRefCount.load(
                                std::memory_order_relaxed);
                            if (it2 == obj.impl->data.end()) {
                                for (size_t i = 0; i < ownerCount; ++i) {
                                    ZeusGC::retainVariantDeep(*this, newVal);
                                }
                                obj.impl->setValue(keyStr, newVal);
                            } else {
                                for (size_t i = 0; i < ownerCount; ++i) {
                                    ZeusGC::retainVariantDeep(*this, newVal);
                                    ZeusGC::releaseVariantDeep(*this, it2->second);
                                }
                                it2->second = newVal;
                            }
                        }
                    }
                } else if (std::holds_alternative<ZeusType::ObjectHandle>(dictVar)) {
                    // 类实例：尝试调用 operator[[]=]
                    ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(dictVar);
                    auto* objPtr = getValidObject(objHandle);
                    if (!objPtr) {
                        VMException::VMException exp("DICT_STORE_VALUE: object has been freed or reused (stale reference)", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    int classId = objPtr->classId;
                    auto* classPtr = getClass(classId);
                    if (!classPtr) {
                        VMException::VMException exp("DICT_STORE_VALUE invalid class id", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& classInfo = *classPtr;
                    auto opIt = classInfo.operatorMethods.find("[[]=]");
                    if (opIt == classInfo.operatorMethods.end()) {
                        VMException::VMException exp("TypeError: class '" + classInfo.name + "' does not support operator[[]=]", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    int protoId = opIt->second;
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("DICT_STORE_VALUE invalid operator[[]=] protoId", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                     // 调用 operator[[]=](key, value)
                     // 栈布局: [ closure, this, key, value ]
                     Closure clo;
                     clo.protoId = protoId;
                     clo.autoRelease = true;
                     int cloId = allocClosure(std::move(clo));

                    stack.push_back(ZeusType::ClosureHandle{cloId});  // callee
                    stack.push_back(objHandle);                       // this (完整 ObjectHandle)
                    stack.push_back(keyVar);                          // key argument
                    // The value is also returned as the assignment expression result.
                    // Keep the popped input alive separately while the operator call
                    // owns and consumes its argument copy.
                    ZeusGC::retainVariantDeep(*this, newItem.value);
                    stack.push_back(newVal);                          // value argument
                    keyItem.owner = 0;
                    dictItem.owner = 0;

                     // 调用 CALL_CLOSURE (argc=3: this + key + value)
                     // Dispatch through a real frame: executeClosureSync pushes a
                     // CallFrame and runs it to completion, so the callee gets its own
                     // local slots. A recursive execute() would share this frame's local
                     // buffer and leave the callee's slots behind in it.
                     if (!executeClosureSync(cloId, 3)) {
                         if (vmstat != ZeusType::VMStatus::Running) break;
                         VMException::VMException exp(
                             "[execute] nested dispatch failed", __line, instr);
                         exp._throw(vmstat, currentChunk);
                         break;
                     }

                     // operator[[]=] 的返回值不会成为赋值表达式的值。
                     // 正常释放它；newItem 仍持有赋值表达式结果。
                     dropStackBack();
                 }
                // 恢复栈状态：将新值压回栈（与 LIST_STORE_ELEM 保持一致的行为）
                stack.push_back(std::move(newItem.value));
                newItem.owner = 0;
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LIST_LENGTH(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LIST_LENGTH: {
                if (stack.empty()) {
                    VMException::VMException exp("LIST_LENGTH stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem topItem = popStackItem();

                int lengthValue = static_cast<int>(
                    ZeusVMUtils::arrayLength(topItem.value));
                dropStackItem(topItem);
                stack.push_back(LexerType::Value(lengthValue));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LIST_LOAD_ELEM(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LIST_LOAD_ELEM: {
                if (stack.size() < 2) {
                    VMException::VMException exp("LIST_LOAD_ELEM stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem indexItem = popStackItem();
                StackItem listItem = popStackItem();

                int idx = 0;
                if (std::holds_alternative<LexerType::Value>(indexItem.value)) {
                    const auto &iv = std::get<LexerType::Value>(indexItem.value);
                    if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) idx = VMHelpers::getIntFromValue(iv);
                }
                ZeusType::UniversalVariant elem = ZeusVMUtils::arrayRead(
                    listItem.value, idx);
                ZeusGC::retainVariantDeep(*this, elem);
                dropStackItem(indexItem);
                dropStackItem(listItem);
                stack.push_back(std::move(elem));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LIST_STORE_ELEM(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LIST_STORE_ELEM: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                if (stack.size() < 3) {
                    VMException::VMException exp("LIST_STORE_ELEM stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem valueItem = popStackItem();
                StackItem indexItem = popStackItem();
                StackItem listItem = popStackItem();

                int idx = 0;
                if (std::holds_alternative<LexerType::Value>(indexItem.value)) {
                    const auto &iv = std::get<LexerType::Value>(indexItem.value);
                    if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) idx = VMHelpers::getIntFromValue(iv);
                }
                if (const auto* array = ZeusBuiltins::getArrayPtr(listItem.value);
                    array && array->impl && idx >= 0 &&
                    idx < static_cast<int>(array->impl->data.size())) {
                    const auto oldValue = array->impl->data[idx];
                    const size_t ownerCount = array->impl->valueRefCount.load(
                        std::memory_order_relaxed);
                    if (ZeusVMUtils::arrayWrite(
                            listItem.value, idx, valueItem.value)) {
                        for (size_t i = 0; i < ownerCount; ++i) {
                            ZeusGC::retainVariantDeep(*this, valueItem.value);
                            ZeusGC::releaseVariantDeep(*this, oldValue);
                        }
                    }
                }
                dropStackItem(indexItem);
                dropStackItem(listItem);
                stack.push_back(std::move(valueItem.value));
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_COUNT_CHECK_NEG(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::COUNT_CHECK_NEG: {
                if (stack.empty()) {
                    VMException::VMException exp("COUNT_CHECK_NEG stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant top = stack.back();
                stack.pop_back();

                bool isNeg = false;

                if (std::holds_alternative<LexerType::Value>(top)) {
                    const auto &val = std::get<LexerType::Value>(top);
                    if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                        const ZeusType::BigInteger& n = std::get<ZeusType::BigInteger>(val.data);
                        if (n < ZeusType::BigInteger(0)) {
                            isNeg = true;
                        }
                        stack.push_back(LexerType::Value(n));
                    } else {
                        stack.push_back(top);
                    }
                } else {
                    stack.push_back(top);
                }

                if (isNeg) {
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1904_INVALID_OPERATION,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "For loop end value cannot be negative",
                        __line, instr
                    );
                    exp.setDetails("For loop range must have a non-negative end value");
                    exp.setSuggestion("Check the loop range: for (start..end..step) requires end >= 0");
                    exp._throw(vmstat, currentChunk);
                }

                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_COUNT_CHECK_STEP_ZERO(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::COUNT_CHECK_STEP_ZERO: {
                if (stack.empty()) {
                    VMException::VMException exp("COUNT_CHECK_STEP_ZERO stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant top = stack.back();
                stack.pop_back();

                bool isZero = false;

                if (std::holds_alternative<LexerType::Value>(top)) {
                    const auto &val = std::get<LexerType::Value>(top);
                    if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                        const ZeusType::BigInteger& n = std::get<ZeusType::BigInteger>(val.data);
                        if (n == ZeusType::BigInteger(0)) {
                            isZero = true;
                        }
                        stack.push_back(LexerType::Value(n));
                    } else {
                        stack.push_back(top);
                    }
                } else {
                    stack.push_back(top);
                }

                if (isZero) {
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1904_INVALID_OPERATION,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "For loop step cannot be zero",
                        __line, instr
                    );
                    exp.setDetails("A zero step would cause an infinite loop");
                    exp.setSuggestion("Use a non-zero step value in for (start..end..step)");
                    exp._throw(vmstat, currentChunk);
                }

                printStack();
                break;
            }
        }
    }
