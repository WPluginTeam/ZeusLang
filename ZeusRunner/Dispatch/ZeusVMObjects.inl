// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_OBJ_LOAD_ATTR(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::OBJ_LOAD_SAFE_ATTR: {
                // Safe navigation: a nil receiver short-circuits to nil.
                // Anything else falls through to the ordinary attribute load.
                if (stack.empty()) {
                    VMException::VMException exp("OBJ_LOAD_SAFE_ATTR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                if (ZeusVMUtils::isNull(stack.back())) {
                    StackItem dead = popStackItem();
                    dropStackItem(dead);
                    stack.push_back(ZeusType::Variant(ZeusType::Null()));
                    printStack();
                    break;
                }
                [[fallthrough]];
            }
            case VMOperator::OBJ_LOAD_ATTR: {
                if (instr.args.empty() || !std::holds_alternative<std::string>(instr.args[0])) {
                    VMException::VMException exp("OBJ_LOAD_ATTR missing attrName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("OBJ_LOAD_ATTR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string attr = std::get<std::string>(instr.args[0]);

                StackItem objItem = popStackItem();
                auto objU = objItem.value;
                struct DropObj {
                    VM* vm;
                    StackItem* it;
                    ~DropObj() { if (vm && it) vm->dropStackItem(*it); }
                } dropObj{this, &objItem};

                // 特殊处理：如果是ModuleProxy，将属性访问映射到全局变量
                if (std::holds_alternative<ZeusType::ModuleProxy>(objU)) {
                    const auto& proxy = std::get<ZeusType::ModuleProxy>(objU);
                    std::string fullName = proxy.modulePath + "." + attr;

                    // 从globalNames查找索引
                    int index = -1;
                    for (size_t i = 0; i < globalNames.size(); ++i) {
                        if (globalNames[i] == fullName) {
                            index = (int)i;
                            break;
                        }
                    }

                    if (index < 0) {
                        VMException::VMException exp("Module '" + proxy.modulePath + "' has no attribute '" + attr + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    if ((size_t)index >= global.size()) {
                        VMException::VMException exp("Module attribute index out of range: " + fullName, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    if (!global[index]) {
                        VMException::VMException exp("Module attribute not initialized: " + fullName, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // 压入全局变量的值（为栈建立所有权引用）
                    auto value = global[index]->readValue();
                    ZeusGC::retainVariantDeep(*this, value);
                    stack.push_back(std::move(value));
                    break;
                }

                // 处理 SharedObjectHandle：从共享堆读取属性
                if (std::holds_alternative<ZeusType::SharedObjectHandle>(objU)) {
                    const auto sharedHandle = std::get<ZeusType::SharedObjectHandle>(objU);
                    const std::string& sharedName = sharedHandle.name;

                    auto cell = ZeusShared::getGlobalHeap().find(
                        sharedName, sharedHandle.generation);
                    if (!cell) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: undefined shared object '" + sharedName + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    ZeusType::UniversalVariant sharedVal = cell->read();

                    // 检查是否是 Variant 中的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(sharedVal)) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& variantVal = std::get<ZeusType::Variant>(sharedVal);
                    if (!std::holds_alternative<ZeusType::Object>(variantVal)) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& obj = std::get<ZeusType::Object>(variantVal);
                    if (!obj.impl) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared object has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto attrIt = obj.impl->data.find(attr);
                    if (attrIt == obj.impl->data.end()) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared object has no attribute '" + attr + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    ZeusGC::retainVariantDeep(*this, attrIt->second);
                    stack.push_back(attrIt->second);
                    ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: shared object '%s', attr='%s'", sharedName.c_str(), attr.c_str());
                    break;
                }

                if (!std::holds_alternative<ZeusType::ObjectHandle>(objU)) {
                    // 检查是否是ClassHandle（试图访问类的静态方法-MODIFIED）
                    std::string actualType = "unknown";
                    if (std::holds_alternative<ZeusType::ClassHandle>(objU)) {
                        int cid = std::get<ZeusType::ClassHandle>(objU).id;
                        auto* classPtr = getClass(cid);
                        if (!classPtr) {
                            VMException::VMException exp("OBJ_LOAD_ATTR invalid class id", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        std::string className = classPtr->name;

                        // 尝试加载静态方法
                        auto itStatic = classPtr->staticMethods.find(attr);
                        if (itStatic != classPtr->staticMethods.end()) {
                            // 找到静态方法，创建闭包并压栈
                            int protoId = itStatic->second;
                            if (!getPrototype(protoId)) {
                                VMException::VMException exp("OBJ_LOAD_ATTR invalid static method protoId", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                            Closure clo;
                            clo.protoId = protoId;
                            // 如果下一个指令是 CALL_CLOSURE 则视为临时调用并自动释放
                            // We execute this closure synchronously; mark as autoRelease
                            clo.autoRelease = true;
                            int cloId = allocClosure(std::move(clo));
                            stack.push_back(ZeusType::ClosureHandle{cloId});
                            break;
                        }

                        // 没找到静态方法，报错
                        auto exp = VMException::VMException(
                            VMException::ErrorCode::E1600_ATTRIBUTE_NOT_FOUND,
                            VMException::ErrorCategory::ATTRIBUTE_ERROR,
                            "Cannot find static member '" + attr + "' on class '" + className + "'",
                            __line, instr
                        );
                        exp.setDetails("Class '" + className + "' does not have a static method or static variable named '" + attr + "'");
                        exp.setSuggestion(
                            "Check if the member name is correct, or if it should be an instance member: 'let obj = new " +
                            className + "(...); obj." + attr + "'"
                        );
                        exp._throw(vmstat, currentChunk);
                        break;
                    } else if (std::holds_alternative<LexerType::Value>(objU)) {
                        actualType = "primitive value";
                    } else if (std::holds_alternative<ZeusType::ClosureHandle>(objU)) {
                        actualType = "function";
                    }

                    auto exp = VMException::typeError_ExpectedObject(
                        "Attribute access", actualType, __line, instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 获取 ObjectHandle 并验证 generation
                ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(objU);
                auto* objPtr = getValidObject(objHandle);

                if (!objPtr) {
                    // 对象已被释放或 slot 已被复用
                    // 提供更详细的错误信息
                    std::string detailMsg = "OBJ_LOAD_ATTR: object has been freed or reused (stale reference)";
                    auto* rawObj = getObject(objHandle.id);
                    if (!rawObj) {
                        detailMsg += " [object not found: id=" + std::to_string(objHandle.id) + "]";
                    } else if (rawObj->generation != objHandle.generation) {
                        detailMsg += " [generation mismatch: handle.gen=" + std::to_string(objHandle.generation)
                                   + " obj.gen=" + std::to_string(rawObj->generation) + "]";
                    } else if (rawObj->markedForDeletion) {
                        detailMsg += " [object marked for deletion]";
                    } else if (rawObj->classId < 0) {
                        detailMsg += " [object cleared, classId=" + std::to_string(rawObj->classId) + "]";
                    }
                    VMException::VMException exp(detailMsg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto& obj = *objPtr;

                // 首先检查是否有 operator[.] 重载
                int classId = obj.classId;
                auto* classPtr = getClass(classId);

                ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: obj.classId=%d, attr='%s', classCount=%zu",
                    classId, attr.c_str(), getClassCount());

                if (classPtr) {
                    const auto& ci = *classPtr;
                    ZEUS_DEBUG_PRINT("[VM]   class='%s', operatorMethods.size()=%zu",
                        ci.name.c_str(), ci.operatorMethods.size());

                    for (const auto& kv : ci.operatorMethods) {
                        ZEUS_DEBUG_PRINT("[VM]     operator '%s' -> protoId=%d", kv.first.c_str(), kv.second);
                    }

                    auto opIt = ci.operatorMethods.find("[.]");

                    if (opIt != ci.operatorMethods.end()) {
                        // 有 operator[.] 重载,调用它
                        int protoId = opIt->second;
                        ZEUS_DEBUG_PRINT("[VM] Found operator[.]! protoId=%d, getPrototype=%p)",
                            protoId, (void*)getPrototype(protoId));

                        if (!getPrototype(protoId)) {
                            ZEUS_DEBUG_PRINT("[VM] ERROR: Invalid protoId!");
                            VMException::VMException exp("OBJ_LOAD_ATTR: invalid protoId for operator[.]", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }

                        // 创建 Closure 包装 protoId
                        Closure clo;
                        clo.protoId = protoId;
                        clo.autoRelease = true; // operator[.] is an immediate call wrapper
                        int cloId = allocClosure(std::move(clo));

                        ZEUS_DEBUG_PRINT("[VM] Created closure #%d for protoId=%d", cloId, protoId);

                        // 压入: closure, this(对象), 属性名
                        stack.push_back(ZeusType::ClosureHandle{cloId});
                        // objItem remains owned by DropObj until this opcode
                        // finishes, so the synchronous call needs its own copy.
                        ZeusGC::retainVariantDeep(*this, objU);
                        stack.push_back(objU);  // this
                        stack.push_back(LexerType::Value(attr));  // 属性名作为参数

                        ZEUS_DEBUG_PRINT("[VM] Stack prepared, calling executeClosureSync with 2 args...");

                        // 使用同步调用 operator[.]
                        if (!executeClosureSync(cloId, 2)) {
                            if (vmstat != ZeusType::VMStatus::Running) break;
                            VMException::VMException exp("OBJ_LOAD_ATTR: operator[.] sync call failed", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }

                        ZEUS_DEBUG_PRINT("[VM] executeClosureSync returned, stack.size=%zu", stack.size());

                        // 方法调用完成,结果在栈顶,继续执行
                        break;
                    } else {
                        ZEUS_DEBUG_PRINT("[VM]   operator[.] NOT found!");
                    }
                }

                // 没有operator[.]重载,使用默认行为
                if (!hasObjectField(obj, attr)) {
                    // 如果不在values中，尝试查找instanceMethods
                    if (classPtr) {
                        const auto& ci = *classPtr;
                        auto methodIt = ci.instanceMethods.find(attr);
                        if (methodIt != ci.instanceMethods.end()) {
                            // 找到实例方法，创建一个绑定了this的闭包
                            int protoId = methodIt->second;
                            ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: found instance method '%s', protoId=%d", attr.c_str(), protoId);

                            if (!getPrototype(protoId)) {
                                VMException::VMException exp("OBJ_LOAD_ATTR: invalid protoId for instance method '" + attr + "'", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }

                            // 创建闭包并绑定this（使用完整的 ObjectHandle 包含 generation）
                            Closure clo;
                            clo.protoId = protoId;
                            retainObject(objHandle); // closure will own & later release boundThis
                            clo.boundThis = objHandle;
                            int cloId = allocClosure(std::move(clo));
                            stack.push_back(ZeusType::ClosureHandle{cloId});
                            ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: created bound closure #%d for method '%s'", cloId, attr.c_str());
                            break;
                        }
                    }

                    // 打印所有可用的属性名
                    std::string availableAttrs;
                    for (const auto& kv : snapshotObjectFields(obj)) {
                        if (!availableAttrs.empty()) availableAttrs += ", ";
                        availableAttrs += kv.first;
                    }
                    ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: object has fields: [%s]", availableAttrs.c_str());

                    VMException::VMException exp("OBJ_LOAD_ATTR missing field '" + attr + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: found field '%s', pushing to stack", attr.c_str());
                auto fieldVal = readObjectField(obj, attr);
                ZeusGC::retainVariantDeep(*this, fieldVal);
                stack.push_back(fieldVal);
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_OBJ_STORE_ATTR(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::OBJ_STORE_ATTR: {
                if (instr.args.empty() || !std::holds_alternative<std::string>(instr.args[0])) {
                    VMException::VMException exp("OBJ_STORE_ATTR missing attrName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.size() < 2) {
                    VMException::VMException exp("OBJ_STORE_ATTR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string attr = std::get<std::string>(instr.args[0]);

                StackItem valItem = popStackItem();
                StackItem objItem = popStackItem();
                auto val = valItem.value;
                auto objU = objItem.value;
                struct DropInputs {
                    VM* vm;
                    StackItem* a;
                    StackItem* b;
                    ~DropInputs() {
                        if (vm && a) vm->dropStackItem(*a);
                        if (vm && b) vm->dropStackItem(*b);
                    }
                } dropInputs{this, &valItem, &objItem};

                // 处理 SharedObjectHandle：写入共享堆中的对象属性
                if (std::holds_alternative<ZeusType::SharedObjectHandle>(objU)) {
                    const auto sharedHandle = std::get<ZeusType::SharedObjectHandle>(objU);
                    const std::string& sharedName = sharedHandle.name;

                    auto cell = ZeusShared::getGlobalHeap().find(
                        sharedName, sharedHandle.generation);
                    if (!cell) {
                        VMException::VMException exp("OBJ_STORE_ATTR: undefined shared object '" + sharedName + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    std::string expectedFieldType;
                    const std::string sharedClassName =
                        sharedObjectTypeName(sharedHandle);
                    if (!sharedClassName.empty()) {
                        const int sharedClassId = findClassByName(sharedClassName);
                        const auto* sharedClass = getClass(sharedClassId);
                        if (sharedClass) {
                            const auto fieldType =
                                sharedClass->explicitFieldTypes.find(attr);
                            if (fieldType != sharedClass->explicitFieldTypes.end()) {
                                expectedFieldType = fieldType->second;
                            }
                        }
                    }
                    if (expectedFieldType.empty()) {
                        expectedFieldType = serializedSharedFieldType(
                            sharedHandle, attr);
                    }
                    const bool fieldTypeMismatch = !expectedFieldType.empty() && [&] {
                        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                        return !isValueCompatibleWithType(val, expectedFieldType);
                    }();
                    if (fieldTypeMismatch) {
                        const std::string actualType = runtimeValueTypeName(val);
                        VMException::VMException exp(
                            "TypeError: field '" + sharedClassName + "." + attr +
                                "' expects '" + expectedFieldType + "', got '" +
                                actualType + "'",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // 加锁修改共享对象
                    cell->lockWrite();
                    struct SharedObjectWriteUnlock {
                        std::shared_ptr<ZeusShared::SharedCell> cell;
                        ~SharedObjectWriteUnlock() {
                            if (cell) cell->unlockWrite();
                        }
                    } sharedWriteUnlock{cell};

                    std::string sharedObjectError;
                    cell->withValue([&](ZeusType::UniversalVariant& sharedValue) {
                        auto* variantValue = std::get_if<ZeusType::Variant>(&sharedValue);
                        auto* objectValue = variantValue != nullptr
                            ? std::get_if<ZeusType::Object>(variantValue)
                            : nullptr;
                        if (objectValue == nullptr) {
                            sharedObjectError =
                                "OBJ_STORE_ATTR: shared variable is not an object";
                            return;
                        }
                        if (!objectValue->impl) {
                            sharedObjectError =
                                "OBJ_STORE_ATTR: shared object has null impl";
                            return;
                        }
                        objectValue->impl->setValue(attr, val);
                    });
                    if (!sharedObjectError.empty()) {
                        VMException::VMException exp(sharedObjectError, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    ZEUS_DEBUG_PRINT("[VM] OBJ_STORE_ATTR: shared object '%s', attr='%s'", sharedName.c_str(), attr.c_str());
                    break;
                }

                if (!std::holds_alternative<ZeusType::ObjectHandle>(objU)) {
                    VMException::VMException exp("OBJ_STORE_ATTR needs ObjectHandle", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());

                // 获取 ObjectHandle 并验证 generation
                ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(objU);
                auto* objPtr = getValidObject(objHandle);

                if (!objPtr) {
                    // 对象已被释放或 slot 已被复用
                    // 提供更详细的错误信息
                    std::string detailMsg = "OBJ_STORE_ATTR: object has been freed or reused (stale reference)";
                    auto* rawObj = getObject(objHandle.id);
                    if (!rawObj) {
                        detailMsg += " [object not found: id=" + std::to_string(objHandle.id) + "]";
                    } else if (rawObj->generation != objHandle.generation) {
                        detailMsg += " [generation mismatch: handle.gen=" + std::to_string(objHandle.generation)
                                   + " obj.gen=" + std::to_string(rawObj->generation) + "]";
                    } else if (rawObj->markedForDeletion) {
                        detailMsg += " [object marked for deletion]";
                    } else if (rawObj->classId < 0) {
                        detailMsg += " [object cleared, classId=" + std::to_string(rawObj->classId) + "]";
                    }
                    VMException::VMException exp(detailMsg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto& obj = *objPtr;

                // 首先检查是否有 operator[.=] 重载
                int classId = obj.classId;
                auto* classPtr = getClass(classId);
                if (classPtr) {
                    const auto& ci = *classPtr;
                    auto opIt = ci.operatorMethods.find("[.=]");
                    if (opIt != ci.operatorMethods.end()) {
                        // 有 operator[.=] 重载,调用它
                        int protoId = opIt->second;
                        ZEUS_DEBUG_PRINT("[VM] Calling operator[.=] for attr '%s' (protoId=%d)", attr.c_str(), protoId);

                        // 创建 Closure 包装 protoId
                        Closure clo;
                        clo.protoId = protoId;
                        int cloId = allocClosure(std::move(clo));

                        // 压入: closure, this(对象), 属性名, 值
                        stack.push_back(ZeusType::ClosureHandle{cloId});
                        ZeusGC::retainVariantDeep(*this, objU);
                        stack.push_back(objU);  // this
                        stack.push_back(attr);  // 属性名
                        ZeusGC::retainVariantDeep(*this, val);
                        stack.push_back(val);   // 值

                        // 调用 CALL_CLOSURE,3个参数(this + 属性名 + 值)
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

                        // 方法调用完成,继续执行
                        break;
                    }
                }

                // 没有operator[.=]重载,使用默认行为
                auto fit = obj.flags.find(attr);
                if (fit != obj.flags.end() && (fit->second & 0x1)) {
                    VMException::VMException exp("OBJ_STORE_ATTR write to const field '" + attr + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (classPtr) {
                    const auto fieldType =
                        classPtr->explicitFieldTypes.find(attr);
                    if (fieldType != classPtr->explicitFieldTypes.end() &&
                        !isValueCompatibleWithType(val, fieldType->second)) {
                        const std::string actualType = runtimeValueTypeName(val);
                        VMException::VMException exp(
                            "TypeError: field '" + classPtr->name + "." + attr +
                                "' expects '" + fieldType->second + "', got '" +
                                actualType + "'",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                }

                // Replace the field in its declared slot or dynamic extension.
                // Keep the old VM reference alive until the new value is stored.
                std::optional<ZeusType::UniversalVariant> oldValue;
                if (hasObjectField(obj, attr)) {
                    oldValue = readObjectField(obj, attr);
                }
                ZeusGC::retainVariantDeep(*this, val);
                try {
                    writeObjectField(obj, attr, val);
                } catch (...) {
                    ZeusGC::releaseVariantDeep(*this, val);
                    throw;
                }
                if (oldValue) ZeusGC::releaseVariantDeep(*this, *oldValue);
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_OBJ_REPLACE_STATE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::OBJ_REPLACE_STATE: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                if (stack.size() < 2) {
                    VMException::VMException("@this replacement stack underflow", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                StackItem replacementItem = popStackItem();
                StackItem receiverItem = popStackItem();
                struct DropReplacementInputs {
                    VM* vm;
                    StackItem* replacement;
                    StackItem* receiver;
                    ~DropReplacementInputs() {
                        if (vm && replacement) vm->dropStackItem(*replacement);
                        if (vm && receiver) vm->dropStackItem(*receiver);
                    }
                } dropInputs{this, &replacementItem, &receiverItem};

                if (!std::holds_alternative<ZeusType::ObjectHandle>(receiverItem.value) ||
                    !std::holds_alternative<ZeusType::ObjectHandle>(replacementItem.value)) {
                    VMException::VMException(
                        "@this replacement requires two class instances", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                const auto receiverHandle =
                    std::get<ZeusType::ObjectHandle>(receiverItem.value);
                const auto replacementHandle =
                    std::get<ZeusType::ObjectHandle>(replacementItem.value);
                auto* receiver = getValidObject(receiverHandle);
                auto* replacement = getValidObject(replacementHandle);
                if (!receiver || !replacement) {
                    VMException::VMException(
                        "@this replacement received a stale class instance", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }
                if (receiver->classId != replacement->classId) {
                    std::string expectedType;
                    if (!instr.args.empty() &&
                        std::holds_alternative<std::string>(instr.args[0])) {
                        expectedType = std::get<std::string>(instr.args[0]);
                    }
                    const auto* receiverClass = getClass(receiver->classId);
                    const auto* replacementClass = getClass(replacement->classId);
                    const std::string receiverType = receiverClass ? receiverClass->name :
                        std::string("unknown");
                    const std::string replacementType = replacementClass
                        ? replacementClass->name : std::string("unknown");
                    VMException::VMException(
                        "@this replacement type mismatch: receiver '" + receiverType +
                            "', replacement '" + replacementType +
                            "' (declared receiver type '" + expectedType + "')",
                        __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                if (receiverHandle.id == replacementHandle.id &&
                    receiverHandle.generation == replacementHandle.generation) {
                    break;
                }

                const auto replacementValues = snapshotObjectFields(*replacement);
                auto replacementDeclaredFields = replacement->declaredFields
                    ? replacement->declaredFields->clone()
                    : std::shared_ptr<ZeusType::DeclaredFieldBuffer>{};
                auto replacementExtensions = replacement->extensionValues;
                auto replacementFlags = replacement->flags;
                const auto oldFields = snapshotObjectFields(*receiver);
                for (const auto& field : replacementValues) {
                    ZeusGC::retainVariantDeep(*this, field.second);
                }
                auto oldDeclaredFields = std::move(receiver->declaredFields);
                auto oldExtensions = std::move(receiver->extensionValues);
                receiver->declaredFields = std::move(replacementDeclaredFields);
                receiver->extensionValues = std::move(replacementExtensions);
                receiver->flags = std::move(replacementFlags);
                for (const auto& field : oldFields) {
                    ZeusGC::releaseVariantDeep(*this, field.second);
                }
                oldDeclaredFields.reset();
                oldExtensions.clear();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CLASS_STATIC_METHOD_CALL(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CLASS_STATIC_METHOD_CALL: {
                if (instr.args.size() < 2 || !std::holds_alternative<std::string>(instr.args[0]) || !std::holds_alternative<LexerType::Value>(instr.args[1]) || !std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[1]).data)) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL bad args", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string method = std::get<std::string>(instr.args[0]);
                int argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));

                if (stack.size() < (size_t)argc + 1) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL stack not enough", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                size_t calleeIndex = stack.size() - (argc + 1);
                auto& callee = stack[calleeIndex];
                if (!std::holds_alternative<ZeusType::ClassHandle>(callee)) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL callee not class handle", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int classId = std::get<ZeusType::ClassHandle>(callee).id;
                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL invalid class id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto it = classPtr->staticMethods.find(method);
                if (it == classPtr->staticMethods.end()) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL unknown static method '" + method + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int protoId = it->second;
                if (!getPrototype(protoId)) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL invalid protoId", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 将类的静态方法包装成一个临时 closure 并压入栈
                // 注意：该 closure 没有捕获 upvalue（纯静态方法）
                Closure clo;
                clo.protoId = protoId;
                clo.autoRelease = true;

                int cloId = allocClosure(std::move(clo));

                stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});

                // 直接用 CALL_CLOSURE 指令调用新创建的 closure（相当于 execute(CALL_CLOSURE ...)）
                // Dispatch through a real frame so the callee gets its own local
                // slots instead of sharing this frame's local buffer.
                nextInstruction.emplace(instr);
                nextInstruction->op = VMOperator::CALL_CLOSURE;
                nextInstruction->args = {LexerType::Value(argc)};

                return;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_OBJ_CALL_METHOD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::OBJ_CALL_SAFE:
            case VMOperator::OBJ_CALL_METHOD: {
                // args: [ methodName (string), argc (Value int) ]
                if (instr.args.size() < 2 || !std::holds_alternative<std::string>(instr.args[0]) || !std::holds_alternative<LexerType::Value>(instr.args[1]) || !std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[1]).data)) {
                    VMException::VMException exp("OBJ_CALL_METHOD bad args", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string method = std::get<std::string>(instr.args[0]);
                int argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));

                if (stack.size() < (size_t)argc + 1) {
                    VMException::VMException exp("OBJ_CALL_METHOD stack not enough", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // callee is at position stack.size() - (argc + 1)
                size_t calleeIndex = stack.size() - (argc + 1);
                int explicitGenericProtoId = -1;
                if (instr.metadata.tag == "generic_vm_specialization") {
                    std::string targetName;
                    if (instr.metadata.kind != IRType::IRMetadataKind::CallTarget ||
                        !instr.metadata.readDirectCall(targetName)) {
                        VMException::VMException exp(
                            "explicit generic method call has no resolved VM target",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    const auto target = functionNameToProtoId.find(targetName);
                    if (target == functionNameToProtoId.end() ||
                        getPrototype(target->second) == nullptr) {
                        VMException::VMException exp(
                            "explicit generic method target is missing from the VM prototype table: " +
                                targetName,
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    explicitGenericProtoId = target->second;
                }
                auto& callee = stack[calleeIndex];

                // Safe navigation: a nil receiver discards the call (the
                // already evaluated arguments included) and pushes nil.
                // Anything else runs the ordinary method dispatch below.
                if (instr.op == VMOperator::OBJ_CALL_SAFE &&
                    ZeusVMUtils::isNull(callee)) {
                    shrinkStackWithRelease(calleeIndex);
                    stack.push_back(ZeusType::Variant(ZeusType::Null()));
                    printStack();
                    break;
                }

                // Support static-style call on ClassHandle as well: ClassName.method(...)
                if (std::holds_alternative<ZeusType::ClassHandle>(callee)) {
                    int classId = std::get<ZeusType::ClassHandle>(callee).id;
                    auto* classPtr = getClass(classId);
                    if (!classPtr) {
                        VMException::VMException exp("OBJ_CALL_METHOD invalid class id", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto itStatic = classPtr->staticMethods.find(method);
                    if (itStatic == classPtr->staticMethods.end()) {
                        VMException::VMException exp("OBJ_CALL_METHOD unknown static method '" + method + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    int protoId = explicitGenericProtoId >= 0
                        ? explicitGenericProtoId : itStatic->second;
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("OBJ_CALL_METHOD invalid protoId", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                     // Create closure for proto and replace callee with closure handle. Static methods do NOT receive implicit this.
                     Closure clo;
                     clo.protoId = protoId;
                     clo.autoRelease = true;
                     int cloId = allocClosure(std::move(clo));
                     stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});

                     // Reuse CALL_CLOSURE with same argc
                     // Dispatch through a real frame so the callee gets its own local
                     // slots instead of sharing this frame's local buffer.
                     nextInstruction.emplace(instr);
                     nextInstruction->op = VMOperator::CALL_CLOSURE;
                     nextInstruction->args = {LexerType::Value(argc)};

                     return;
                 }

                // 处理 SharedObjectHandle：从共享堆读取对象并调用方法
                if (std::holds_alternative<ZeusType::SharedObjectHandle>(callee)) {
                    const auto sharedHandle = std::get<ZeusType::SharedObjectHandle>(callee);
                    const std::string& sharedName = sharedHandle.name;

                    auto cell = ZeusShared::getGlobalHeap().find(
                        sharedName, sharedHandle.generation);
                    if (!cell) {
                        VMException::VMException exp("SharedObjectHandle: undefined shared object '" + sharedName + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // 读取共享对象数据
                    ZeusType::UniversalVariant sharedVal = cell->read();

                    // 检查是否是 Variant 中的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(sharedVal)) {
                        VMException::VMException exp("SharedObjectHandle: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& variantVal = std::get<ZeusType::Variant>(sharedVal);
                    if (!std::holds_alternative<ZeusType::Object>(variantVal)) {
                        VMException::VMException exp("SharedObjectHandle: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& obj = std::get<ZeusType::Object>(variantVal);
                    if (!obj.impl) {
                        VMException::VMException exp("SharedObjectHandle: shared object has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // 获取 instanceMethods（存储为 Variant 中的 Object）
                    auto methodsIt = obj.impl->data.find("__instanceMethods__");
                    if (methodsIt == obj.impl->data.end()) {
                        VMException::VMException exp("SharedObjectHandle: object has no instanceMethods", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // instanceMethods 是 Variant 包装的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(methodsIt->second)) {
                        VMException::VMException exp("SharedObjectHandle: instanceMethods is not a Variant", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& methodsVariant = std::get<ZeusType::Variant>(methodsIt->second);
                    if (!std::holds_alternative<ZeusType::Object>(methodsVariant)) {
                        VMException::VMException exp("SharedObjectHandle: instanceMethods is not an Object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& methodsObj = std::get<ZeusType::Object>(methodsVariant);
                    if (!methodsObj.impl) {
                        VMException::VMException exp("SharedObjectHandle: instanceMethods has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto protoIt = methodsObj.impl->data.find(method);
                    if (protoIt == methodsObj.impl->data.end()) {
                        VMException::VMException exp("SharedObjectHandle: unknown method '" + method + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // 获取 protoId
                    int protoId = explicitGenericProtoId;
                    if (std::holds_alternative<LexerType::Value>(protoIt->second)) {
                        if (protoId < 0) {
                            protoId = VMHelpers::getIntFromValue(
                                std::get<LexerType::Value>(protoIt->second));
                        }
                    }

                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("SharedObjectHandle: invalid protoId for method '" + method + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    // 创建闭包
                    Closure clo;
                    clo.protoId = protoId;
                    clo.autoRelease = true;
                    int cloId = allocClosure(std::move(clo));

                    // 替换 callee 为闭包，实例方法需要 this 作为第一个参数
                    // 当前栈布局: [..., SharedObjectHandle(at calleeIndex), arg0, arg1, ...]
                    // 需要变成: [..., ClosureHandle, SharedObjectHandle(作为this), arg0, arg1, ...]

                    // 把 SharedObjectHandle 作为 this 参数插入
                    stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});

                    // 在参数列表开头插入 this (SharedObjectHandle)
                    // 需要将 this 插入到 args 之前
                    // 栈变为: [closure, this, arg0, arg1, ...]
                    stack.insert(stack.begin() + calleeIndex + 1,
                        ZeusType::SharedObjectHandle{sharedName, sharedHandle.generation});

                    // 调用闭包，argc + 1 因为添加了 this
                    // Dispatch through a real frame so the callee gets its own local
                    // slots instead of sharing this frame's local buffer.
                    nextInstruction.emplace(instr);
                    nextInstruction->op = VMOperator::CALL_CLOSURE;
                    nextInstruction->args = {LexerType::Value(argc + 1)};
                    for (auto& source : nextInstruction->metadata.callArguments) {
                        if (source.positionalIndex >= 0) ++source.positionalIndex;
                    }

                    return;
                }

                {
                    const ZeusType::UniversalVariant receiver = callee;
                    const std::string receiverType = typeImplementationName(receiver);
                    auto implementation = typeImplementations.find(receiverType);
                    if (implementation != typeImplementations.end()) {
                        auto methodIt = implementation->second.instanceMethods.find(method);
                        if (methodIt != implementation->second.instanceMethods.end()) {
                            const int protoId = explicitGenericProtoId >= 0
                                ? explicitGenericProtoId : methodIt->second;
                            if (!getPrototype(protoId)) {
                                VMException::VMException exp(
                                    "TYPE_IMPL_BUILD registered an invalid method prototype",
                                    __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }

                            Closure clo;
                            clo.protoId = protoId;
                            clo.autoRelease = true;
                            const int cloId = allocClosure(std::move(clo));
                            stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});
                            stack.insert(calleeIndex + 1, receiver);
                            // Use the ordinary closure frame dispatcher. Running a
                            // nested interpreter here grows the C++ stack for every
                            // impl method, including Array/string library methods.
                            nextInstruction.emplace(instr);
                            nextInstruction->op = VMOperator::CALL_CLOSURE;
                            nextInstruction->args = {LexerType::Value(argc + 1)};
                            if ((receiverType == "string" || receiverType == "bytes") &&
                                instr.args.size() >= 7 &&
                                std::holds_alternative<std::string>(instr.args[2]) &&
                                std::get<std::string>(instr.args[2]) ==
                                    "__zeus_receiver_source") {
                                nextInstruction->args.insert(
                                    nextInstruction->args.end(),
                                    instr.args.begin() + 2,
                                    instr.args.end());
                            }
                            for (auto& source : nextInstruction->metadata.callArguments) {
                                if (source.positionalIndex >= 0) ++source.positionalIndex;
                            }

                            return;
                        }
                    }
                }

                if (!std::holds_alternative<ZeusType::ObjectHandle>(callee)) {
                    // 报出接收者的运行时类型：只写 "non-object" 的话，遇到装箱值
                    // 或者没装载 impl 的类型（例如没导入 zeus.byte 时的 bytes）
                    // 根本看不出问题在哪一层。
                    std::string msg = "Cannot call method '" + method +
                        "' on a non-object value (receiver type: " +
                        runtimeValueTypeName(callee) + ")";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 获取 ObjectHandle 并验证 generation
                ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(callee);
                auto* objPtr = getValidObject(objHandle);

                if (!objPtr) {
                    // 对象已被释放或 slot 已被复用
                    std::string msg = "OBJ_CALL_METHOD: object has been freed or reused (stale reference) when calling method '" + method + "'";
                    msg += " [handle.id=" + std::to_string(objHandle.id) +
                           " handle.gen=" + std::to_string(objHandle.generation);
                    auto* rawObj = getObject(objHandle.id);
                    if (!rawObj) {
                        msg += " object not found]";
                    } else {
                        msg += " object.gen=" + std::to_string(rawObj->generation) +
                               " classId=" + std::to_string(rawObj->classId) +
                               " markedForDeletion=" + (rawObj->markedForDeletion ? "true]" : "false]");
                    }
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int classId = objPtr->classId;
                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    std::string msg = "Object has invalid class when calling method '" + method + "'";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto it = classPtr->instanceMethods.find(method);
                int resolvedMethodProtoId = it == classPtr->instanceMethods.end()
                    ? -1 : it->second;
                if (resolvedMethodProtoId < 0 && method.compare(0, 8, "operator") == 0 &&
                    method.size() > 9 && method[8] == '[' && method.back() == ']') {
                    const auto operatorMethod = classPtr->operatorMethods.find(
                        method.substr(8));
                    if (operatorMethod != classPtr->operatorMethods.end()) {
                        resolvedMethodProtoId = operatorMethod->second;
                    }
                }

                if (resolvedMethodProtoId < 0) {
                    // 方法不存在于 instanceMethods 中
                    // 首先检查对象的实例变量中是否有该名称的可调用值
                    if (hasObjectField(*objPtr, method)) {
                        // 找到了实例变量，检查它是否是可调用的
                        const auto fieldValue = readObjectField(*objPtr, method);
                        bool isCallable = std::holds_alternative<ZeusType::ClosureHandle>(fieldValue) ||
                                         std::holds_alternative<ZeusType::NativeFuncHandle>(fieldValue);

                        if (isCallable) {
                            // 这是一个存储在实例变量中的函数，调用它
                            // 当前栈布局: [..., object(在calleeIndex), arg0, arg1, ..., argN-1]
                            // 需要变成: [..., function, arg0, arg1, ..., argN-1]

                            // 替换 callee 位置的 object 为函数
                            stack[calleeIndex] = fieldValue;

                            // 根据函数类型使用不同的调用指令
                            if (std::holds_alternative<ZeusType::ClosureHandle>(fieldValue)) {
                                // 使用 CALL_CLOSURE 调用闭包
                                IRType::IRInstr callIns{ VMOperator::CALL_CLOSURE, { LexerType::Value(argc) } };
                                callIns.line = instr.line;  // 继承原指令的行号
                                execute(callIns);
                            } else {
                                // 使用 CALL_NATIVE 调用原生函数
                                IRType::IRInstr callIns{ VMOperator::CALL_NATIVE, { LexerType::Value(argc) } };
                                callIns.line = instr.line;  // 继承原指令的行号
                                execute(callIns);
                            }
                            break;
                        }
                    }

                    // 检查是否有 operator[.] 重载
                    auto opIt = classPtr->operatorMethods.find("[.]");

                    if (opIt != classPtr->operatorMethods.end()) {
                        // 通过 operator[.] 获取成员再调用
                        // 当前栈布局: [..., object(在calleeIndex), arg0, arg1, ..., argN-1]
                        int opProtoId = opIt->second;

                        vmMessage("Found operator[.]! protoId=" + std::to_string(opProtoId) + ", getPrototype=" + (getPrototype(opProtoId) ? "valid" : "null") + ")");

                        if (getPrototype(opProtoId)) {
                            // 步骤1: 保存原始参数到临时数组
                            std::vector<ZeusType::UniversalVariant> savedArgs;
                            savedArgs.reserve(argc);

                            for (int i = 0; i < argc; ++i) {
                                savedArgs.push_back(stack.back());
                                stack.pop_back();
                            }

                            // 现在栈顶是object,将其弹出
                            stack.pop_back();

                            // 步骤2: 同步调用 operator[.](methodName)
                            // 压入调用所需的栈: [closure, this(object), methodName]
                            Closure opClo;
                            opClo.protoId = opProtoId;
                            int opCloId = allocClosure(std::move(opClo));

                            stack.push_back(ZeusType::ClosureHandle{opCloId});
                            stack.push_back(objHandle);  // 使用完整的 ObjectHandle
                            stack.push_back(LexerType::Value(method));

                            vmMessage("Calling operator[.] SYNC with method='" + method + "'");

                            // 使用同步调用，等待 operator[.] 返回
                            if (!executeClosureSync(opCloId, 2)) {
                                if (vmstat != ZeusType::VMStatus::Running) break;
                                VMException::VMException exp("operator[.] sync call failed", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                            // temporary opClo was used only for this sync call, release it now
                            releaseClosure(opCloId);

                            // 步骤3: operator[.] 返回值在栈顶,这应该是要调用的函数
                            if (stack.empty()) {
                                VMException::VMException exp("operator[.] returned nothing", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }

                            auto retrievedMember = stack.back();
                            stack.pop_back();

                            vmMessage("operator[.] returned: " + ZeusVMUtils::valueToDebugString(retrievedMember));

                            // 步骤4: 设置调用目标函数的栈布局
                            // 需要: [function, arg0, arg1, ..., argN-1]
                            stack.push_back(retrievedMember);

                            // 恢复参数(注意savedArgs是反序的)
                            for (int i = (int)savedArgs.size() - 1; i >= 0; --i) {
                                stack.push_back(savedArgs[i]);
                            }

                            // 步骤5: 调用函数（使用栈帧机制）
                            IRType::IRInstr callFuncInstr{VMOperator::CALL_CLOSURE, {LexerType::Value(argc)}};
                            execute(callFuncInstr);
                            break;
                        }
                    }

                    // 没有 operator[.] 或调用失败,报错
                    std::string className = classPtr->name;
                    std::string msg = "Object of type '" + className + "' has no method '" + method + "'";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int protoId = explicitGenericProtoId >= 0
                    ? explicitGenericProtoId : resolvedMethodProtoId;
                if (!getPrototype(protoId)) {
                    VMException::VMException exp("OBJ_CALL_METHOD invalid protoId", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // We will call method with implicit 'this' via Closure::boundThis.
                // This avoids inserting an extra arg manually and keeps ownership symmetric:
                // - closure owns boundThis (retained)
                // - stack drops its callee object reference when overwritten
                ZeusType::UniversalVariant oldCallee = callee;
                uint8_t oldOwner = stack.ownerFlag(calleeIndex);

                Closure clo;
                clo.protoId = protoId;
                clo.autoRelease = true;
                retainObject(objHandle); // closure will own & later release boundThis
                clo.boundThis = objHandle;

                int cloId = allocClosure(std::move(clo));

                // Replace callee (object handle) with closure handle
                stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});

                // Drop the overwritten callee slot (stack-owned object ref)
                dropStackItem(StackItem{oldCallee, oldOwner});

                // Reuse the normal CALL_CLOSURE frame path. Running a nested
                // runFrameLoop here grows the native C++ stack for every method
                // call; ordinary method chains can overflow at shallow Zeus depth.
                nextInstruction.emplace(instr);
                nextInstruction->op = VMOperator::CALL_CLOSURE;
                nextInstruction->args = {LexerType::Value(argc)};

                return;
            }
        }
    }
