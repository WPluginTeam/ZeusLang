// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_TYPE_IMPL_BUILD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::TYPE_IMPL_BUILD: {
                auto failImpl = [&](const std::string& message) {
                    VMException::VMException exp(message, __line, instr);
                    exp._throw(vmstat, currentChunk);
                };
                if (instr.args.empty() ||
                    !std::holds_alternative<std::string>(instr.args[0]) ||
                    std::get<std::string>(instr.args[0]).empty()) {
                    failImpl("TYPE_IMPL_BUILD requires a non-empty target type");
                    break;
                }

                ZeusType::ClassInfo additions;
                additions.name = std::get<std::string>(instr.args[0]);
                size_t cursor = 1;
                auto readMethodMap = [&](std::unordered_map<std::string, int>& methods) {
                    if (cursor >= instr.args.size() ||
                        !std::holds_alternative<LexerType::Value>(instr.args[cursor])) {
                        return false;
                    }
                    const int count = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(instr.args[cursor++]));
                    if (count < 0 || static_cast<size_t>(count) >
                            (instr.args.size() - cursor) / 2) {
                        return false;
                    }
                    for (int i = 0; i < count; ++i) {
                        if (cursor + 1 >= instr.args.size() ||
                            !std::holds_alternative<std::string>(instr.args[cursor]) ||
                            !std::holds_alternative<LexerType::Value>(instr.args[cursor + 1])) {
                            return false;
                        }
                        const std::string& name = std::get<std::string>(instr.args[cursor++]);
                        const int protoId = VMHelpers::getIntFromValue(
                            std::get<LexerType::Value>(instr.args[cursor++]));
                        if (name.empty() || protoId < 0 || !getPrototype(protoId) ||
                            !methods.emplace(name, protoId).second) {
                            return false;
                        }
                    }
                    return true;
                };

                if (!readMethodMap(additions.instanceMethods) ||
                    !readMethodMap(additions.staticMethods) ||
                    !readMethodMap(additions.operatorMethods) ||
                    cursor != instr.args.size()) {
                    failImpl("TYPE_IMPL_BUILD contains malformed or duplicate method metadata");
                    break;
                }

                ZeusType::ClassInfo mergedType = additions;
                auto existing = typeImplementations.find(additions.name);
                if (existing != typeImplementations.end()) {
                    mergedType = existing->second;
                    std::string conflict;
                    if (!mergeTypeImplementationMethods(mergedType, additions, conflict)) {
                        failImpl("impl member '" + conflict +
                                 "' is already registered for type '" + additions.name + "'");
                        break;
                    }
                }

                std::vector<std::pair<size_t, ZeusType::ClassInfo>> updatedClasses;
                for (size_t i = 0; i < classes.size(); ++i) {
                    if (classes[i].name != additions.name) continue;
                    ZeusType::ClassInfo updated = classes[i];
                    std::string conflict;
                    if (!mergeTypeImplementationMethods(updated, additions, conflict)) {
                        failImpl("impl member '" + conflict +
                                 "' conflicts with class member on type '" + additions.name + "'");
                        updatedClasses.clear();
                        break;
                    }
                    updatedClasses.emplace_back(i, std::move(updated));
                }
                if (vmstat != ZeusType::VMStatus::Running) break;

                std::vector<std::pair<size_t, ZeusType::ClassInfo>> updatedSharedClasses;
                if (sharedClasses && sharedClasses != &classes) {
                    for (size_t i = 0; i < sharedClasses->size(); ++i) {
                        if ((*sharedClasses)[i].name != additions.name) continue;
                        ZeusType::ClassInfo updated = (*sharedClasses)[i];
                        std::string conflict;
                        if (!mergeTypeImplementationMethods(updated, additions, conflict)) {
                            failImpl("impl member '" + conflict +
                                     "' conflicts with shared class member on type '" +
                                     additions.name + "'");
                            updatedSharedClasses.clear();
                            break;
                        }
                        updatedSharedClasses.emplace_back(i, std::move(updated));
                    }
                }
                if (vmstat != ZeusType::VMStatus::Running) break;

                typeImplementations[additions.name] = std::move(mergedType);
                for (auto& entry : updatedClasses) {
                    classes[entry.first] = std::move(entry.second);
                }
                if (sharedClasses && sharedClasses != &classes) {
                    for (auto& entry : updatedSharedClasses) {
                        (*sharedClasses)[entry.first] = std::move(entry.second);
                    }
                }

                auto sharedClassCell = ZeusShared::getGlobalHeap().find(
                    "__class__" + additions.name);
                if (sharedClassCell) {
                    sharedClassCell->withValue([&](ZeusType::UniversalVariant& value) {
                        auto* classVariant = std::get_if<ZeusType::Variant>(&value);
                        auto* classObject = classVariant != nullptr
                            ? std::get_if<ZeusType::Object>(classVariant)
                            : nullptr;
                        if (classObject == nullptr || !classObject->impl) return;
                        for (const auto& entry : additions.staticMethods) {
                            classObject->impl->setValue(
                                "__static_" + entry.first,
                                LexerType::Value(entry.second));
                        }
                        for (const auto& entry : additions.instanceMethods) {
                            classObject->impl->setValue(
                                "__inst_" + entry.first,
                                LexerType::Value(entry.second));
                        }
                        for (const auto& entry : additions.operatorMethods) {
                            classObject->impl->setValue(
                                "__op_" + entry.first,
                                LexerType::Value(entry.second));
                        }
                    });
                }
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CLASS_BUILD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CLASS_BUILD: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // 说明：栈布局 [ className, usingThisDefault, staticMethods(dict), instanceMethods(dict), operatorMethods(dict), fields(dict) ]
                if (stack.size() < 6) {
                    VMException::VMException exp("CLASS_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto fieldsU   = stack.back();
                stack.pop_back();

                auto operatorMethU = stack.back();
                stack.pop_back();

                auto instMethU = stack.back();
                stack.pop_back();

                auto staticMethU = stack.back();
                stack.pop_back();

                auto usingThisU = stack.back();
                stack.pop_back();

                auto nameU     = stack.back();
                stack.pop_back();

                std::string className = "";
                std::string usingThisAlias = "";

                if (std::holds_alternative<std::string>(nameU)) {
                    className = std::get<std::string>(nameU);
                } else if (std::holds_alternative<LexerType::Value>(nameU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(nameU).data)) {
                    className = std::get<std::string>(std::get<LexerType::Value>(nameU).data);
                } else {
                    VMException::VMException exp("CLASS_BUILD className must be string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // usingThisU is expected to be a string (alias) or empty string pushed as Value("")
                if (std::holds_alternative<std::string>(usingThisU)) {
                    usingThisAlias = std::get<std::string>(usingThisU);
                } else if (std::holds_alternative<LexerType::Value>(usingThisU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(usingThisU).data)) {
                    usingThisAlias = std::get<std::string>(std::get<LexerType::Value>(usingThisU).data);
                }

                ZeusType::ClassInfo info;
                info.name = className;
                info.usingThisAlias = usingThisAlias;

                // fields/staticMethods/instanceMethods/operatorMethods are provided as Variant (Object) values.
                // Parse instanceMethods/staticMethods/operatorMethods dicts if present and populate info maps.
                if (std::holds_alternative<ZeusType::Variant>(staticMethU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(staticMethU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                // keys are method names, values expected to be integer protoId
                                int pid = -1;
                                const ZeusType::UniversalVariant &val = kv.second;
                                if (std::holds_alternative<LexerType::Value>(val)) {
                                    const auto &lv = std::get<LexerType::Value>(val);
                                    if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) pid = VMHelpers::getIntFromValue(lv);
                                }
                                if (pid >= 0) info.staticMethods[kv.first] = pid;
                            }
                        }
                    }
                }

                if (std::holds_alternative<ZeusType::Variant>(instMethU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(instMethU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                int pid = -1;
                                const ZeusType::UniversalVariant &val = kv.second;
                                if (std::holds_alternative<LexerType::Value>(val)) {
                                    const auto &lv = std::get<LexerType::Value>(val);
                                    if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) pid = VMHelpers::getIntFromValue(lv);
                                }
                                if (pid >= 0) info.instanceMethods[kv.first] = pid;
                            }
                        }
                    }
                }

                // operatorMethods: parse operator symbols -> protoId
                if (std::holds_alternative<ZeusType::Variant>(operatorMethU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(operatorMethU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                int pid = -1;
                                const ZeusType::UniversalVariant &val = kv.second;
                                if (std::holds_alternative<LexerType::Value>(val)) {
                                    const auto &lv = std::get<LexerType::Value>(val);
                                    if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) pid = VMHelpers::getIntFromValue(lv);
                                }
                                if (pid >= 0) info.operatorMethods[kv.first] = pid;
                            }
                        }
                    }
                }

                // fields: if provided, evaluate default values will have been pushed as Values/Variants
                if (std::holds_alternative<ZeusType::Variant>(fieldsU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(fieldsU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            auto addField = [&](const std::string& fieldName,
                                                const ZeusType::UniversalVariant& value) {
                                ZeusType::FieldDef fdef;
                                fdef.defaultValue = value;
                                fdef.isConst = false;
                                if (info.fields.find(fieldName) == info.fields.end()) {
                                    info.fieldOrder.push_back(fieldName);
                                }
                                info.fields[fieldName] = std::move(fdef);
                            };
                            for (const auto& fieldName : obj.impl->order) {
                                if (!obj.impl->hasOrderKey(fieldName)) continue;
                                const auto field = obj.impl->data.find(fieldName);
                                if (field != obj.impl->data.end()) {
                                    addField(fieldName, field->second);
                                }
                            }
                            for (const auto &kv : obj.impl->data) {
                                addField(kv.first, kv.second);
                            }
                        }
                    }
                }

                std::string fieldMetadataError;
                if (!applyClassFieldTypeMetadata(
                        instr.metadata, className, info, fieldMetadataError)) {
                    VMException::VMException exp(
                        fieldMetadataError, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto implementation = typeImplementations.find(className);
                if (implementation != typeImplementations.end()) {
                    ZeusType::ClassInfo merged = info;
                    std::string conflict;
                    if (!mergeTypeImplementationMethods(
                            merged, implementation->second, conflict)) {
                        VMException::VMException exp(
                            "impl method '" + conflict +
                            "' conflicts with class member on type '" + className + "'",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    info = std::move(merged);
                }

                int classId = (int)classes.size();
                classes.push_back(std::move(info));
                nameToClass[className] = classId;

                stack.push_back(ZeusType::ClassHandle{classId});
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_CLASS_BUILD_SHARED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::CLASS_BUILD_SHARED: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // 说明：栈布局 [ className, usingThisDefault, staticMethods(dict), instanceMethods(dict), operatorMethods(dict), fields(dict) ]
                // 与 CLASS_BUILD 类似，但类定义存储到共享堆上
                if (stack.size() < 6) {
                    VMException::VMException exp("CLASS_BUILD_SHARED stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto fieldsU       = stack.back(); stack.pop_back();
                auto operatorMethU = stack.back(); stack.pop_back();
                auto instMethU     = stack.back(); stack.pop_back();
                auto staticMethU   = stack.back(); stack.pop_back();
                auto usingThisU    = stack.back(); stack.pop_back();
                auto nameU         = stack.back(); stack.pop_back();

                std::string className = "";
                std::string usingThisAlias = "";

                if (std::holds_alternative<std::string>(nameU)) {
                    className = std::get<std::string>(nameU);
                } else if (std::holds_alternative<LexerType::Value>(nameU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(nameU).data)) {
                    className = std::get<std::string>(std::get<LexerType::Value>(nameU).data);
                } else {
                    VMException::VMException exp("CLASS_BUILD_SHARED className must be string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (std::holds_alternative<std::string>(usingThisU)) {
                    usingThisAlias = std::get<std::string>(usingThisU);
                } else if (std::holds_alternative<LexerType::Value>(usingThisU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(usingThisU).data)) {
                    usingThisAlias = std::get<std::string>(std::get<LexerType::Value>(usingThisU).data);
                }

                ZeusType::ClassInfo info;
                info.name = className;
                info.usingThisAlias = usingThisAlias;

                // 解析方法映射 (与 CLASS_BUILD 相同)
                auto parseMethodDict = [](const ZeusType::UniversalVariant& dictU, std::unordered_map<std::string, int>& outMap) {
                    if (std::holds_alternative<ZeusType::Variant>(dictU)) {
                        const ZeusType::Variant& v = std::get<ZeusType::Variant>(dictU);
                        if (std::holds_alternative<ZeusType::Object>(v)) {
                            const ZeusType::Object& obj = std::get<ZeusType::Object>(v);
                            if (obj.impl) {
                                for (const auto& kv : obj.impl->data) {
                                    int pid = -1;
                                    const ZeusType::UniversalVariant& val = kv.second;
                                    if (std::holds_alternative<LexerType::Value>(val)) {
                                        const auto& lv = std::get<LexerType::Value>(val);
                                        if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) {
                                            pid = VMHelpers::getIntFromValue(lv);
                                        }
                                    }
                                    if (pid >= 0) outMap[kv.first] = pid;
                                }
                            }
                        }
                    }
                };

                parseMethodDict(staticMethU, info.staticMethods);
                parseMethodDict(instMethU, info.instanceMethods);
                parseMethodDict(operatorMethU, info.operatorMethods);

                // 解析字段
                if (std::holds_alternative<ZeusType::Variant>(fieldsU)) {
                    const ZeusType::Variant& v = std::get<ZeusType::Variant>(fieldsU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object& obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            auto addField = [&](const std::string& fieldName,
                                                const ZeusType::UniversalVariant& value) {
                                ZeusType::FieldDef fdef;
                                fdef.defaultValue = value;
                                fdef.isConst = false;
                                if (info.fields.find(fieldName) == info.fields.end()) {
                                    info.fieldOrder.push_back(fieldName);
                                }
                                info.fields[fieldName] = std::move(fdef);
                            };
                            for (const auto& fieldName : obj.impl->order) {
                                if (!obj.impl->hasOrderKey(fieldName)) continue;
                                const auto field = obj.impl->data.find(fieldName);
                                if (field != obj.impl->data.end()) {
                                    addField(fieldName, field->second);
                                }
                            }
                            for (const auto& kv : obj.impl->data) {
                                addField(kv.first, kv.second);
                            }
                        }
                    }
                }

                std::string fieldMetadataError;
                if (!applyClassFieldTypeMetadata(
                        instr.metadata, className, info, fieldMetadataError)) {
                    VMException::VMException exp(
                        fieldMetadataError, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto implementation = typeImplementations.find(className);
                if (implementation != typeImplementations.end()) {
                    ZeusType::ClassInfo merged = info;
                    std::string conflict;
                    if (!mergeTypeImplementationMethods(
                            merged, implementation->second, conflict)) {
                        VMException::VMException exp(
                            "impl method '" + conflict +
                            "' conflicts with shared class member on type '" + className + "'",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    info = std::move(merged);
                }

                // 同时存储到本地 classes 数组（用于本 VM 访问）
                int classId = (int)classes.size();
                classes.push_back(info);
                nameToClass[className] = classId;

                // 将 ClassInfo 序列化为 Object 存储到共享堆
                // 为了跨 VM 访问，我们创建一个包含类信息的 ObjectData
                auto classObjData = ZeusGC::makeObjectData();
                classObjData->setValue("__class_id__", LexerType::Value(classId));
                classObjData->setValue("__class_name__", LexerType::Value(className));
                classObjData->setValue("__using_this__", LexerType::Value(usingThisAlias));
                classObjData->setValue("__is_shared_class__", LexerType::Value(true));

                // 存储方法信息
                for (const auto& kv : info.staticMethods) {
                    classObjData->setValue("__static_" + kv.first, LexerType::Value(kv.second));
                }
                for (const auto& kv : info.instanceMethods) {
                    classObjData->setValue("__inst_" + kv.first, LexerType::Value(kv.second));
                }
                for (const auto& kv : info.operatorMethods) {
                    classObjData->setValue("__op_" + kv.first, LexerType::Value(kv.second));
                }
                for (size_t index = 0; index < info.fieldOrder.size(); ++index) {
                    classObjData->setValue(
                        "__zeus_field_order_" + std::to_string(index),
                        LexerType::Value(info.fieldOrder[index]));
                }
                for (const auto& kv : info.fields) {
                    classObjData->setValue("__field_" + kv.first, kv.second.defaultValue);
                }
                for (const auto& initializer : info.fieldInitializerNames) {
                    classObjData->setValue(
                        "__zeus_field_initializer_" + initializer.first,
                        LexerType::Value(initializer.second));
                }
                for (const auto& kv : info.explicitFieldTypes) {
                    classObjData->setValue(
                        "__field_type__" + kv.first,
                        LexerType::Value(kv.second));
                }

                ZeusType::Object sharedClassObj;
                sharedClassObj.impl = classObjData;

                // 存储到共享堆
                ZeusShared::getGlobalHeap().getOrCreate(
                    "__class__" + className,
                    ZeusType::Variant(sharedClassObj)
                );

                // 返回 SharedClassHandle
                stack.push_back(ZeusType::SharedClassHandle{className});
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_OBJ_BUILD(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::OBJ_BUILD: {
                // 读取参数个数
                int argc = 0;
                if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }

                std::string nativeTargetType;
                if (instr.args.size() > 1 &&
                    std::holds_alternative<std::string>(instr.args[1])) {
                    nativeTargetType = std::get<std::string>(instr.args[1]);
                }
                const bool hasNativeTarget = !nativeTargetType.empty();

                const size_t requiredStackItems = static_cast<size_t>(argc) +
                    (hasNativeTarget ? 0u : 1u);
                if (stack.size() < requiredStackItems) {
                    VMException::VMException exp("OBJ_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 保存构造函数参数（从栈中弹出，并转移“栈所有权”到本地临时列表）
                // 重要：不能直接 stack.pop_back()，否则 Object/Closure 的引用计数不会对称释放，导致泄露或悬空引用。
                std::vector<StackItem> ctorArgItems;
                ctorArgItems.reserve(argc);
                struct CtorArgGuard {
                    VM* vm;
                    std::vector<StackItem>* items;
                    ~CtorArgGuard() {
                        if (!vm || !items) return;
                        for (auto& it : *items) {
                            if (it.owner) vm->dropStackItem(it);
                        }
                    }
                } ctorArgGuard{this, &ctorArgItems};

                for (int i = 0; i < argc; ++i) {
                    ctorArgItems.insert(ctorArgItems.begin(), popStackItem());
                }

                ZeusType::UniversalVariant clsU = LexerType::Value(nativeTargetType);
                if (!hasNativeTarget) {
                    StackItem clsItem = popStackItem();
                    clsU = clsItem.value;
                    dropStackItem(clsItem);
                }

                std::string genericClassName;
                std::vector<std::string> genericTypeArguments;
                std::string specializedClassName;
                if (instr.metadata.readClassInstantiation(
                        genericClassName, genericTypeArguments,
                        &specializedClassName) &&
                    !specializedClassName.empty()) {
                    // A generic construction names its specialized class
                    // directly; the popped class handle is only the source-level
                    // class expression and may still refer to the template.
                    clsU = LexerType::Value(specializedClassName);
                }

                // DEBUG: 输出 clsU 的类型
                ZEUS_DEBUG_PRINT("[OBJ_BUILD] clsU type: ClassHandle=%d, SharedClassHandle=%d, Value=%d",
                    std::holds_alternative<ZeusType::ClassHandle>(clsU),
                    std::holds_alternative<ZeusType::SharedClassHandle>(clsU),
                    std::holds_alternative<LexerType::Value>(clsU));
                if (std::holds_alternative<LexerType::Value>(clsU)) {
                    ZEUS_DEBUG_PRINT("[OBJ_BUILD] clsU value: %s", ZeusVMUtils::valueToDebugString(clsU).c_str());
                }

                int classId = -1;

                auto invokeNewFactory = [&](int protoId) {
                    const Prototype* proto = getPrototype(protoId);
                    if (!proto) {
                        VMException::VMException exp(
                            "operator[new] has an invalid function prototype",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        return false;
                    }
                    if (!proto->captures.empty()) {
                        VMException::VMException exp(
                            "operator[new] cannot capture variables",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        return false;
                    }

                    Closure factory;
                    factory.protoId = protoId;
                    factory.autoRelease = true;
                    const int closureId = allocClosure(std::move(factory));
                    stack.push_back(ZeusType::ClosureHandle{closureId});
                    for (auto& argument : ctorArgItems) {
                        stack.push_back(argument.value);
                        argument.owner = 0;
                    }
                    if (!executeClosureSync(closureId, argc)) {
                        if (vmstat == ZeusType::VMStatus::Running) {
                            VMException::VMException exp(
                                "operator[new] dispatch failed", __line, instr);
                            exp._throw(vmstat, currentChunk);
                        }
                        return false;
                    }
                    return true;
                };

                if (hasNativeTarget) {
                    if (const auto* abi =
                            ZeusBuiltins::findNativeTypeABI(nativeTargetType)) {
                        nativeTargetType = std::string(abi->canonicalName);
                    }
                    const auto implementation = typeImplementations.find(nativeTargetType);
                    if (implementation == typeImplementations.end()) {
                        VMException::VMException exp(
                            "OBJ_BUILD native type has no impl: " + nativeTargetType,
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    const auto factory =
                        implementation->second.operatorMethods.find("[new]");
                    if (factory == implementation->second.operatorMethods.end()) {
                        VMException::VMException exp(
                            "type '" + nativeTargetType +
                            "' does not define operator[new]",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    invokeNewFactory(factory->second);
                    break;
                }

                // 支持多种形式：ClassHandle、SharedClassHandle 或类名字符串
                if (std::holds_alternative<ZeusType::ClassHandle>(clsU)) {
                    classId = std::get<ZeusType::ClassHandle>(clsU).id;
                } else if (std::holds_alternative<ZeusType::SharedClassHandle>(clsU)) {
                    // SharedClassHandle：从共享堆获取类信息
                    std::string className = std::get<ZeusType::SharedClassHandle>(clsU).name;

                    // 先在本地 classes 中查找
                    for (size_t i = 0; i < classes.size(); ++i) {
                        if (classes[i].name == className) {
                            classId = (int)i;
                            break;
                        }
                    }

                    // 如果本地没有，尝试从共享堆恢复类定义
                    if (classId == -1) {
                        auto cellPtr = ZeusShared::getGlobalHeap().find("__class__" + className);
                        std::string sharedInitializerError;

                        if (cellPtr) {
                            const ZeusType::UniversalVariant uniVar = cellPtr->read();

                            if (std::holds_alternative<ZeusType::Variant>(uniVar)) {
                                const ZeusType::Variant& variant = std::get<ZeusType::Variant>(uniVar);

                                if (std::holds_alternative<ZeusType::Object>(variant)) {
                                    const ZeusType::Object& objWrapper = std::get<ZeusType::Object>(variant);

                                    if (objWrapper.impl) {
                                        // 从共享堆恢复 ClassInfo
                                        ZeusType::ClassInfo info;
                                        info.name = className;
                                        std::map<size_t, std::string> fieldOrderByIndex;
                                        const std::string orderPrefix = "__zeus_field_order_";
                                        const std::string initializerPrefix =
                                            "__zeus_field_initializer_";

                                        for (const auto& kv : objWrapper.impl->data) {
                                            const std::string& key = kv.first;
                                            const ZeusType::UniversalVariant& val = kv.second;

                                            if (key == "__using_this__") {
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    const LexerType::Value& lv = std::get<LexerType::Value>(val);
                                                    if (std::holds_alternative<std::string>(lv.data)) {
                                                        info.usingThisAlias = std::get<std::string>(lv.data);
                                                    }
                                                }
                                            } else if (key.rfind(orderPrefix, 0) == 0) {
                                                try {
                                                    const size_t index = static_cast<size_t>(
                                                        std::stoull(key.substr(orderPrefix.size())));
                                                    if (std::holds_alternative<LexerType::Value>(val)) {
                                                        const auto& literal =
                                                            std::get<LexerType::Value>(val);
                                                        if (std::holds_alternative<std::string>(literal.data)) {
                                                            fieldOrderByIndex[index] =
                                                                std::get<std::string>(literal.data);
                                                        }
                                                    }
                                                } catch (const std::exception&) {
                                                    sharedInitializerError =
                                                        "invalid shared class field-order metadata";
                                                }
                                            } else if (key.rfind(initializerPrefix, 0) == 0) {
                                                const std::string fieldName =
                                                    key.substr(initializerPrefix.size());
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    const auto& literal =
                                                        std::get<LexerType::Value>(val);
                                                    if (std::holds_alternative<std::string>(literal.data)) {
                                                        const std::string initializerName =
                                                            std::get<std::string>(literal.data);
                                                        const auto proto =
                                                            functionNameToProtoId.find(initializerName);
                                                        if (proto == functionNameToProtoId.end() ||
                                                            !getPrototype(proto->second) ||
                                                            !getPrototype(proto->second)->captures.empty()) {
                                                            sharedInitializerError =
                                                                "shared class field initializer '" +
                                                                initializerName + "' is unavailable";
                                                        } else {
                                                            info.fieldInitializerNames[fieldName] =
                                                                initializerName;
                                                        }
                                                    }
                                                }
                                            } else if (key.rfind("__static_", 0) == 0) {
                                                std::string methName = key.substr(9);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    info.staticMethods[methName] = VMHelpers::getIntFromValue(std::get<LexerType::Value>(val));
                                                }
                                            } else if (key.rfind("__inst_", 0) == 0) {
                                                std::string methName = key.substr(7);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    info.instanceMethods[methName] = VMHelpers::getIntFromValue(std::get<LexerType::Value>(val));
                                                }
                                            } else if (key.rfind("__op_", 0) == 0) {
                                                std::string opName = key.substr(5);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    info.operatorMethods[opName] = VMHelpers::getIntFromValue(std::get<LexerType::Value>(val));
                                                }
                                            } else if (key.rfind("__field_type__", 0) == 0) {
                                                const std::string fieldName = key.substr(14);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    const auto& literal = std::get<LexerType::Value>(val);
                                                    if (std::holds_alternative<std::string>(literal.data)) {
                                                        info.explicitFieldTypes[fieldName] =
                                                            std::get<std::string>(literal.data);
                                                    }
                                                }
                                            } else if (key.rfind("__field_", 0) == 0) {
                                                std::string fieldName = key.substr(8);
                                                ZeusType::FieldDef fdef;
                                                fdef.defaultValue = val;
                                                fdef.isConst = false;
                                                if (info.fields.find(fieldName) == info.fields.end()) {
                                                    info.fieldOrder.push_back(fieldName);
                                                }
                                                info.fields[fieldName] = fdef;
                                            }
                                        }

                                        for (const auto& initializer : info.fieldInitializerNames) {
                                            const auto proto = functionNameToProtoId.find(
                                                initializer.second);
                                            if (proto == functionNameToProtoId.end() ||
                                                !getPrototype(proto->second) ||
                                                !getPrototype(proto->second)->captures.empty()) {
                                                sharedInitializerError =
                                                    "shared class field initializer '" +
                                                    initializer.second + "' is unavailable";
                                                break;
                                            }
                                            info.fields[initializer.first].initializerProtoId =
                                                proto->second;
                                        }

                                        if (!fieldOrderByIndex.empty()) {
                                            info.fieldOrder.clear();
                                            for (const auto& ordered : fieldOrderByIndex) {
                                                info.fieldOrder.push_back(ordered.second);
                                            }
                                        }

                                        if (sharedInitializerError.empty()) {
                                            classId = (int)classes.size();
                                            classes.push_back(info);
                                        }
                                    }
                                }
                            }
                        }

                        if (!sharedInitializerError.empty()) {
                            VMException::VMException exp(
                                sharedInitializerError, __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    }

                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD SharedClassHandle not found in heap: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (std::holds_alternative<std::string>(clsU)) {
                    // 延迟解析：在 nameToClass 映射中查找类名
                    std::string className = std::get<std::string>(clsU);

                    // 优先在 nameToClass 映射中查找
                    auto it = nameToClass.find(className);
                    if (it != nameToClass.end()) {
                        classId = it->second;
                        vmMessage("[late-resolve] OBJ_BUILD resolved class '" + className + "' -> #" + std::to_string(classId));
                    } else {
                        // 回退：在全局变量槽中查找（兼容性）
                        for (size_t i = 0; i < global.size(); ++i) {
                            if (global[i]) {
                                const auto value = global[i]->readValue();
                                if (!std::holds_alternative<ZeusType::ClassHandle>(value)) continue;
                                int cid = std::get<ZeusType::ClassHandle>(value).id;
                                auto* classPtr = getClass(cid);
                                if (classPtr && classPtr->name == className) {
                                    classId = cid;
                                    break;
                                }
                            }
                        }
                    }

                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD class not found: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (std::holds_alternative<LexerType::Value>(clsU)) {
                    // 处理 LOAD_VALUE 发出的 Value 类型
                    const auto& val = std::get<LexerType::Value>(clsU);
                    if (std::holds_alternative<std::string>(val.data)) {
                        std::string className = std::get<std::string>(val.data);

                        // 优先在 nameToClass 映射中查找
                        auto it = nameToClass.find(className);
                        if (it != nameToClass.end()) {
                            classId = it->second;
                            vmMessage("[late-resolve] OBJ_BUILD resolved class '" + className + "' -> #" + std::to_string(classId));
                        } else {
                            // 回退：在全局变量槽中查找（兼容性）
                            for (size_t i = 0; i < global.size(); ++i) {
                                if (global[i]) {
                                    const auto value = global[i]->readValue();
                                    if (!std::holds_alternative<ZeusType::ClassHandle>(value)) continue;
                                    int cid = std::get<ZeusType::ClassHandle>(value).id;
                                    auto* classPtr = getClass(cid);
                                    if (classPtr && classPtr->name == className) {
                                        classId = cid;
                                        break;
                                    }
                                }
                            }
                        }

                        if (classId == -1) {
                            VMException::VMException exp("OBJ_BUILD class not found: " + className, __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    } else {
                        VMException::VMException exp("OBJ_BUILD needs ClassHandle or className string/Value", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else {
                    VMException::VMException exp("OBJ_BUILD needs ClassHandle or className string/Value", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    VMException::VMException exp("OBJ_BUILD invalid class id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                const auto factory = classPtr->operatorMethods.find("[new]");
                if (factory != classPtr->operatorMethods.end()) {
                    const bool hasFieldInitializer = std::any_of(
                        classPtr->fields.begin(), classPtr->fields.end(),
                        [](const auto& field) {
                            return field.second.initializerProtoId >= 0;
                        });
                    if (hasFieldInitializer) {
                        VMException::VMException exp(
                            "operator[new] cannot be combined with field default initializers for class '" +
                                classPtr->name + "'",
                            __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    invokeNewFactory(factory->second);
                    break;
                }

                ZeusType::ObjectInstance obj;
                obj.classId = classId;

                // initialize object fields from class info
                const ZeusType::ClassInfo ci = *classPtr;
                std::unordered_set<std::string> initializedFieldNames;
                // A container default is mutable, so every instance has to receive its own
                // copy. Storing the class-level value directly would make all
                // instances alias one list or dict, and the first append from any
                // instance would be visible in all the others.
                auto copyFieldDefault = [&](const std::string& fieldName) {
                    const auto field = ci.fields.find(fieldName);
                    if (field == ci.fields.end() ||
                        !initializedFieldNames.insert(fieldName).second) {
                        return;
                    }
                    ZeusType::UniversalVariant instanceDefault =
                        field->second.defaultValue;
                    if (auto* variant =
                            std::get_if<ZeusType::Variant>(&instanceDefault)) {
                        if (auto* source =
                                std::get_if<ZeusType::Array>(variant)) {
                            ZeusType::Array fresh;
                            fresh.impl = ZeusGC::makeArrayData();
                            if (source->impl) {
                                fresh.impl->data = source->impl->data;
                            }
                            instanceDefault = ZeusType::Variant(std::move(fresh));
                        } else if (auto* source =
                                       std::get_if<ZeusType::Object>(variant)) {
                            ZeusType::Object fresh;
                            fresh.impl = ZeusGC::makeObjectData();
                            if (source->impl) {
                                fresh.impl->order = source->impl->order;
                                fresh.impl->data = source->impl->data;
                            }
                            instanceDefault = ZeusType::Variant(std::move(fresh));
                        }
                    }
                    obj.extensionValues[fieldName] = std::move(instanceDefault);
                    // The object now holds a reference to the default value, so it
                    // has to be retained. Without this the instance owns a
                    // reference it never took, and the first OBJ_STORE_ATTR on the
                    // field releases the value out from under every other instance
                    // that was initialised from the same default.
                    ZeusGC::retainVariantDeep(*this, obj.extensionValues[fieldName]);
                    obj.flags[fieldName] = field->second.isConst ? 0x1 : 0x0;
                };
                for (const auto& fieldName : ci.fieldOrder) {
                    copyFieldDefault(fieldName);
                }
                for (const auto& field : ci.fields) {
                    copyFieldDefault(field.first);
                }

                ZeusType::ObjectHandle objHandle = allocObject(std::move(obj));
                int objId = objHandle.id;
                // 保护构造过程中对象不被提前释放（调用结束后会对称释放一次）
                retainObject(objHandle);
                bool constructorFrameScheduled = false;

                std::unordered_set<std::string> initializedDefaultsInOrder;
                for (const auto& fieldName : ci.fieldOrder) {
                    const auto field = ci.fields.find(fieldName);
                    if (field == ci.fields.end()) continue;
                    initializedDefaultsInOrder.insert(fieldName);
                    if (field->second.initializerProtoId < 0) continue;
                    std::string initializerError;
                    if (!runFieldInitializer(
                            field->second.initializerProtoId,
                            classId,
                            ZeusType::UniversalVariant(objHandle), objHandle,
                            ci.name, fieldName, nullptr, initializerError)) {
                        if (vmstat == ZeusType::VMStatus::Running) {
                            VMException::VMException exp(
                                initializerError, __line, instr);
                            exp._throw(vmstat, currentChunk);
                        }
                        break;
                    }
                }

                if (vmstat == ZeusType::VMStatus::Running) {
                    for (const auto& field : ci.fields) {
                        if (field.second.initializerProtoId >= 0 &&
                            initializedDefaultsInOrder.find(field.first) ==
                                initializedDefaultsInOrder.end()) {
                            VMException::VMException exp(
                                "field initializer has no declaration order metadata for '" +
                                    ci.name + "." + field.first + "'",
                                __line,
                                instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    }
                }

                // 检查是否有 init 构造函数方法
                if (vmstat == ZeusType::VMStatus::Running) {
                ZEUS_DEBUG_PRINT("[VM] OBJ_BUILD: Looking for init in instanceMethods (size=%zu)", ci.instanceMethods.size());
                for (const auto& kv : ci.instanceMethods) {
                    ZEUS_DEBUG_PRINT("[VM]   - method '%s' -> protoId=%d", kv.first.c_str(), kv.second);
                }

                auto initIt = ci.instanceMethods.find("init");
                if (initIt != ci.instanceMethods.end()) {
                    // 调用 init 方法
                    int protoId = initIt->second;
                    const Prototype* protoPtr = getPrototype(protoId);
                    ZEUS_DEBUG_PRINT("[VM] Found init: protoId=%d, protoPtr=%p", protoId, (void*)protoPtr);
                    if (protoPtr) {
                        const Prototype& proto = *protoPtr;
                        ZEUS_DEBUG_PRINT("[VM] Calling init constructor with %d args", argc);
                        // 创建临时闭包
                        Closure clo;
                        clo.protoId = protoId;
                        clo.autoRelease = true;
                        int cloId = allocClosure(std::move(clo));

                         stack.push_back(ZeusType::ClosureHandle{cloId});
                         retainObject(objHandle); // stack will later release this slot in shrinkStackWithRelease
                         stack.push_back(objHandle);  // 使用完整的 ObjectHandle（包含 generation）
                         for (auto& argIt : ctorArgItems) {
                             stack.push_back(argIt.value);
                             // transfer ownership from ctorArgItems -> stack
                             argIt.owner = 0;
                         }

                        // 保存oldLocal（但在OBJ_BUILD中，local应该是当前frame的）
                        auto oldLocal = local;
                        auto oldUpvalue = upvalue;

                        // 设置init的参数（使用与CALL_CLOSURE相同的逻辑）
                        int calleeIndex = stack.size() - (argc + 2);  // argc+1(this) + 1(closure)

                        // 初始化local数组
                        int regularParamCount = proto.paramCount;
                        local.clear();
                        local.resize(regularParamCount);
                        std::vector<bool> paramProvided(regularParamCount, false);
                        upvalue.clear();
                        for (int i = 0; i < regularParamCount; ++i) {
                            local[i] = newCell(LexerType::Value(0), 0, "CONSTRUCTOR_local");
                        }

                        // 扩展 local 以容纳 *args 和 **kwargs
                        if (proto.hasVariadicArgs || proto.hasVariadicKwargs) {
                            int extraSlots = 0;
                            if (proto.hasVariadicArgs) extraSlots++;
                            if (proto.hasVariadicKwargs) extraSlots++;

                            int newLocalSize = regularParamCount + extraSlots;
                            local.resize(newLocalSize);
                            for (int idx = regularParamCount; idx < newLocalSize; ++idx) {
                                local[idx] = newCell(LexerType::Value(0), 0, "CONSTRUCTOR_varargs");
                            }
                        }

                        // 分离位置参数和命名参数
                        std::map<std::string, ZeusType::UniversalVariant> namedArgs;
                        std::vector<ZeusType::UniversalVariant> positionalArgs;

                        // this是第一个参数
                        positionalArgs.push_back(stack[calleeIndex + 1]);

                        // 处理构造函数的其他参数
                        for (int i = 0; i < argc; ++i) {
                            auto& arg = stack[calleeIndex + 2 + i];  // +2因为有closure和this
                            if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(arg)) {
                                auto namedArgPtr = std::get<std::shared_ptr<ZeusType::NamedArg>>(arg);
                                namedArgs[namedArgPtr->name] = namedArgPtr->value;
                            } else {
                                positionalArgs.push_back(arg);
                            }
                        }

                        int positionalCount = (int)positionalArgs.size();

                        // 填充常规参数
                        std::set<std::string> consumedNamedArgs;

                        for (int i = 0; i < regularParamCount; ++i) {
                            bool filled = false;

                            // 优先使用位置参数
                            if (i < positionalCount) {
                                assignCellValue(local[i], positionalArgs[i]);
                                paramProvided[i] = true;
                                filled = true;
                            }

                            // 尝试从命名参数匹配
                            if (!filled && i < (int)proto.paramNames.size()) {
                                const std::string& paramName = proto.paramNames[i];
                                auto it = namedArgs.find(paramName);
                                if (it != namedArgs.end()) {
                                    assignCellValue(local[i], it->second);
                                    consumedNamedArgs.insert(paramName);
                                    paramProvided[i] = true;
                                    filled = true;
                                }
                            }

                            if (!filled && !proto.hasRuntimeDefaultPrologue &&
                                i < (int)proto.paramHasDefault.size() && proto.paramHasDefault[i] &&
                                i < (int)proto.paramDefaultValues.size()) {
                                assignCellValue(local[i], proto.paramDefaultValues[i]);
                                filled = true;
                            }

                            // 最后用0初始化
                            if (!filled) {
                                assignCellValue(local[i], LexerType::Value(0));
                            }
                        }

                        // 构建 *args 数组
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

                        // 构建 **kwargs 字典
                        if (proto.hasVariadicKwargs) {
                            std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                            auto objDataPtr = ZeusGC::makeObjectData();

                            for (const auto& kv : namedArgs) {
                                if (consumedNamedArgs.find(kv.first) == consumedNamedArgs.end()) {
                                    objDataPtr->setValue(kv.first, kv.second);
                                }
                            }

                            ZeusType::Object kwargsDict;
                            kwargsDict.impl = objDataPtr;

                            ZeusType::Variant variantKwargs = kwargsDict;
                            int kwargsSlot = regularParamCount;
                            if (proto.hasVariadicArgs) kwargsSlot++;
                            assignCellValue(local[kwargsSlot], variantKwargs);
                        }

                        // Use the shared frame loop while retaining constructor argument binding.
                        CallFrame constructorFrame;
                        constructorFrame.code = &proto.body;
                        constructorFrame.pc = 0;
                        constructorFrame.savedLocal = oldLocal;
                        constructorFrame.savedUpvalue = oldUpvalue;
                        constructorFrame.stackBase = calleeIndex;
                        constructorFrame.savedLine = __line;
                        constructorFrame.paramProvided = std::move(paramProvided);
                        constructorFrame.referenceParameterSlots = proto.paramIsReference;
                        constructorFrame.constructorResult = objHandle;
                        callFrames.push_back(std::move(constructorFrame));
                        justPushedFrame = true;
                        constructorFrameScheduled = true;
                    }
                }

                // 释放构造保护 retain（创建时 +1）
                }
                releaseObject(objHandle);
                if (vmstat != ZeusType::VMStatus::Running) {
                    if (!constructorFrameScheduled) {
                        releaseObject(objHandle); // no result is transferred to a frame or stack
                    }
                    break;
                }
                if (!constructorFrameScheduled) {
                    stack.push_back(objHandle);  // 使用完整的 ObjectHandle（包含 generation）
                }
                printStack();
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_OBJ_BUILD_SHARED(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::OBJ_BUILD_SHARED: {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // OBJ_BUILD_SHARED "objName" argc
                // 创建共享对象，存储到共享堆上
                // 栈布局: [classHandle/className, arg1, arg2, ...]

                std::string objName = "";
                int argc = 0;

                if (instr.args.size() >= 1 && std::holds_alternative<std::string>(instr.args[0])) {
                    objName = std::get<std::string>(instr.args[0]);
                }
                if (instr.args.size() >= 2 && std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));
                }

                if (objName.empty()) {
                    VMException::VMException exp("OBJ_BUILD_SHARED requires object name", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.size() < (size_t)(argc + 1)) {
                    VMException::VMException exp("OBJ_BUILD_SHARED stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 保存构造函数参数
                std::vector<ZeusType::UniversalVariant> ctorArgs;
                for (int i = 0; i < argc; ++i) {
                    ctorArgs.insert(ctorArgs.begin(), stack.back());
                    stack.pop_back();
                }

                // 获取类
                auto clsU = stack.back();
                stack.pop_back();

                int classId = -1;

                if (std::holds_alternative<ZeusType::ClassHandle>(clsU)) {
                    classId = std::get<ZeusType::ClassHandle>(clsU).id;
                } else if (std::holds_alternative<ZeusType::SharedClassHandle>(clsU)) {
                    // 共享类：需要从共享堆获取类信息
                    std::string className = std::get<ZeusType::SharedClassHandle>(clsU).name;

                    // 查找本地 classes 数组中是否已有该类
                    for (size_t i = 0; i < classes.size(); ++i) {
                        if (classes[i].name == className) {
                            classId = (int)i;
                            break;
                        }
                    }

                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD_SHARED: shared class not found locally: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (std::holds_alternative<std::string>(clsU)) {
                    std::string className = std::get<std::string>(clsU);
                    for (size_t i = 0; i < classes.size(); ++i) {
                        if (classes[i].name == className) {
                            classId = (int)i;
                            break;
                        }
                    }
                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD_SHARED class not found: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else {
                    VMException::VMException exp("OBJ_BUILD_SHARED needs ClassHandle or className", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    VMException::VMException exp("OBJ_BUILD_SHARED invalid class id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 创建对象实例（同时存储到本地和共享堆）
                ZeusType::ObjectInstance obj;
                obj.classId = classId;

                const auto ci = *classPtr;
                for (const auto& kv : ci.fields) {
                    obj.extensionValues[kv.first] = kv.second.defaultValue;
                    // Same ownership rule as OBJ_BUILD: the instance owns its
                    // field values, so the shared default has to be retained.
                    ZeusGC::retainVariantDeep(*this, obj.extensionValues[kv.first]);
                    obj.flags[kv.first]  = kv.second.isConst ? 0x1 : 0x0;
                }

                ZeusType::ObjectHandle objHandle = allocObject(std::move(obj));
                int objId = objHandle.id;

                // 将对象序列化为 ObjectData 存储到共享堆
                auto sharedObjData = ZeusGC::makeObjectData();
                sharedObjData->setValue("__obj_id__", LexerType::Value(objId));
                sharedObjData->setValue("__obj_generation__", LexerType::Value(objHandle.generation));
                sharedObjData->setValue("__class_id__", LexerType::Value(classId));
                sharedObjData->setValue("__class_name__", LexerType::Value(ci.name));
                for (const auto& fieldType : ci.explicitFieldTypes) {
                    sharedObjData->setValue(
                        "__field_type__" + fieldType.first,
                        LexerType::Value(fieldType.second));
                }

                // 注意：这里需要重新获取对象引用，因为 allocObject 可能复用槽位
                auto* objRef = getObject(objId);
                if (objRef) {
                    for (const auto& kv : snapshotObjectFields(*objRef)) {
                        sharedObjData->setValue(kv.first, kv.second);
                    }
                }

                ZeusType::Object sharedObj;
                sharedObj.impl = sharedObjData;

                auto sharedCell = ZeusShared::getGlobalHeap().getOrCreate(
                    objName,
                    ZeusType::Variant(sharedObj)
                );

                // Field initializers run only after the shared object is
                // registered, so `this.field` reads the same live data that
                // later initializers will observe.
                const ZeusType::SharedObjectHandle sharedHandle{
                    objName, sharedCell->generation};
                std::unordered_set<std::string> initializedFields;
                for (const auto& fieldName : ci.fieldOrder) {
                    const auto field = ci.fields.find(fieldName);
                    if (field == ci.fields.end()) continue;
                    initializedFields.insert(fieldName);
                    if (field->second.initializerProtoId < 0) continue;

                    std::string initializerError;
                    if (!runFieldInitializer(
                            field->second.initializerProtoId,
                            classId,
                            ZeusType::UniversalVariant(sharedHandle),
                            objHandle,
                            ci.name,
                            fieldName,
                            sharedObjData,
                            initializerError)) {
                        if (vmstat == ZeusType::VMStatus::Running) {
                            VMException::VMException exp(
                                initializerError, __line, instr);
                            exp._throw(vmstat, currentChunk);
                        }
                        break;
                    }
                }

                if (vmstat == ZeusType::VMStatus::Running) {
                    for (const auto& field : ci.fields) {
                        if (field.second.initializerProtoId >= 0 &&
                            initializedFields.find(field.first) ==
                                initializedFields.end()) {
                            VMException::VMException exp(
                                "field initializer has no declaration order metadata for '" +
                                    ci.name + "." + field.first + "'",
                                __line,
                                instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    }
                }

                if (vmstat != ZeusType::VMStatus::Running) {
                    break;
                }

                // 调用 init 构造函数（如果存在）
                auto initIt = ci.instanceMethods.find("init");
                if (initIt != ci.instanceMethods.end() && getPrototype(initIt->second)) {
                    // 简化处理：不调用 init，让用户自己调用
                    // 或者之后扩展支持
                }

                // 返回 SharedObjectHandle
                stack.push_back(sharedHandle);
                printStack();
                break;
            }
        }
    }
