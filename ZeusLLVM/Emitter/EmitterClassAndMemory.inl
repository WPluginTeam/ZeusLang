// Emitter member definitions: class lifetime finalizers, embedded slots, @faddr memory, and container helpers; included inside class Emitter.
    template <typename EmitLine, typename EmitLabel, typename FreshTemporary,
              typename ConvertArgument, typename Fail>
    bool emitClassConstruction(
        const IRType::IRInstr& ins,
        const std::string& className,
        const std::vector<NativeValue>& arguments,
        NativeValue& instance,
        EmitLine&& emit,
        EmitLabel&& emitLabel,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        Fail&& failAt
    ) {
        if (findNewOperatorSignature(className) != nullptr) {
            return emitNewOperatorConstruction(
                ins,
                className,
                arguments,
                instance,
                emit,
                freshTemporary,
                convertArgument,
                failAt);
        }

        const ClassLayout* layout = findClassLayout(className);
        if (layout == nullptr) {
            return failAt(ins, "[emit-llvm] new 目标没有 LLVM class 布局");
        }
        if (layout->fieldNames.size() != layout->fieldTypes.size()) {
            return failAt(ins, "[emit-llvm] class 布局的字段名与类型数量不一致");
        }

        const FunctionSignature* constructor = nullptr;
        for (const auto& entry : functionSignatures) {
            const auto& candidate = entry.second;
            if (candidate.ownerType == className &&
                candidate.name == className + "::init") {
                constructor = &candidate;
                break;
            }
        }
        if (constructor == nullptr && !arguments.empty()) {
            return failAt(ins, "[emit-llvm] class 没有 init 构造函数");
        }
        if (constructor != nullptr && constructor->captureCount != 0) {
            return failAt(ins, "[emit-llvm] init 构造函数包含捕获变量，不能走原生 ABI");
        }
        if (constructor != nullptr &&
            (constructor->parameterTypes.empty() ||
             arguments.size() + 1 < static_cast<size_t>(constructor->minRequiredParams) ||
             arguments.size() + 1 > constructor->parameterTypes.size() ||
             constructor->parameterTypes.front() != className)) {
            std::string signatureShape = "none";
            if (!constructor->parameterTypes.empty()) {
                signatureShape.clear();
                for (size_t i = 0; i < constructor->parameterTypes.size(); ++i) {
                    if (i > 0) signatureShape += ", ";
                    signatureShape += constructor->parameterTypes[i];
                }
            }
            return failAt(ins,
                "[emit-llvm] init 构造函数参数数量或类型不匹配 (class '" + className +
                "', 实参 " + std::to_string(arguments.size() + 1) +
                " 个, 需要 " + std::to_string(constructor->minRequiredParams) +
                ".." + std::to_string(constructor->parameterTypes.size()) +
                " 个, 签名 (" + signatureShape + "))");
        }

        if (!layout->fieldInitializerNames.empty() &&
            layout->fieldInitializerNames.size() != layout->fieldNames.size()) {
            return failAt(ins,
                "[emit-llvm] class field initializer metadata does not match its field layout");
        }

        std::vector<const FunctionSignature*> fieldInitializers(
            layout->fieldNames.size(), nullptr);
        for (size_t fieldIndex = 0; fieldIndex < layout->fieldNames.size(); ++fieldIndex) {
            const std::string initializerName =
                layout->fieldInitializerNames.empty()
                    ? std::string()
                    : layout->fieldInitializerNames[fieldIndex];
            if (initializerName.empty()) continue;

            const auto initializerEntry = functionSignatures.find(initializerName);
            if (initializerEntry == functionSignatures.end()) {
                return failAt(ins,
                    "[emit-llvm] missing per-instance field initializer function '" +
                    initializerName + "' for '" + className + "." +
                    layout->fieldNames[fieldIndex] + "'");
            }

            const FunctionSignature& initializer = initializerEntry->second;
            const std::string& fieldTypeName = layout->fieldTypes[fieldIndex];
            const std::string fieldType = llvmFieldType(fieldTypeName);
            const bool dynamicField = usesZeusValueABI(fieldTypeName);
            const std::string initializerReturnType =
                usesZeusValueABI(initializer.returnType)
                    ? "void" : llvmType(initializer.returnType);
            const bool byReferenceReceiver =
                !initializer.parameterIsReference.empty() &&
                initializer.parameterIsReference.front();
            if (initializer.captureCount != 0) {
                return failAt(ins,
                    "[emit-llvm] field initializer '" + initializerName +
                    "' cannot capture outer locals or upvalues; use this or global variables");
            }
            if (initializer.ownerType != className ||
                initializer.isStaticMethod || initializer.isExternal ||
                !initializer.nativeSymbol.empty() || initializer.isGenericTemplate ||
                initializer.hasVariadicArgs ||
                initializer.hasVariadicKwargs || initializer.hasDefaultParameters() ||
                initializer.parameterTypes.size() != 1 ||
                initializer.parameterTypes.front() != className ||
                byReferenceReceiver ||
                (!initializer.parameterNames.empty() &&
                 (initializer.parameterNames.size() != 1 ||
                  initializer.parameterNames.front() != "this")) ||
                initializer.returnType != fieldTypeName ||
                usesZeusValueABI(initializer.returnType) != dynamicField ||
                initializerReturnType.empty() || fieldType.empty() ||
                (!dynamicField && initializerReturnType != fieldType)) {
                return failAt(ins,
                    "[emit-llvm] incompatible per-instance initializer ABI for '" +
                    className + "." + layout->fieldNames[fieldIndex] + "' via '" +
                    initializerName + "'");
            }
            fieldInitializers[fieldIndex] = &initializer;
        }

        std::string allocationSize;
        if (layout->fieldNames.empty()) {
            allocationSize = "1";
        } else {
            const std::string sizePointer = freshTemporary();
            allocationSize = freshTemporary();
            emit(sizePointer + " = getelementptr " + llvmStructType(className) +
                 ", ptr null, i32 1");
            emit(allocationSize + " = ptrtoint ptr " + sizePointer + " to i64");
        }

        const std::string allocationKey = freshTemporary();
        const std::string allocationStem = allocationKey.size() > 1 &&
            allocationKey.front() == '%' ? allocationKey.substr(1) : allocationKey;
        const std::string object = freshTemporary();
        emit(object + " = call ptr @malloc(i64 " + allocationSize + ")");
        const std::string isNull = freshTemporary();
        const std::string allocationFailed = "new_fail_" + allocationStem;
        const std::string allocationReady = "new_ready_" + allocationStem;
        emit(isNull + " = icmp eq ptr " + object + ", null");
        emit("br i1 " + isNull + ", label %" + allocationFailed +
             ", label %" + allocationReady);
        emitLabel(allocationFailed);
        emit("call void @zeus_oom_abort(ptr " + poolStr("new") +
             ", ptr " + poolStr(className) + ")");
        emit("unreachable");
        emitLabel(allocationReady);

        for (size_t fieldIndex = 0; fieldIndex < layout->fieldNames.size(); ++fieldIndex) {
            const std::string& fieldTypeName = layout->fieldTypes[fieldIndex];
            const std::string fieldType = llvmFieldType(fieldTypeName);
            const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
            const bool isDynamicField = usesZeusValueABI(fieldTypeName);
            const bool isNativeViewField = ZeusBuiltins::isNativePointerABI(fieldABI);
            if (fieldType.empty() || fieldType == "void" || (!isDynamicField &&
                (fieldTypeName.empty() ||
                 (!isIntegerLLVMType(fieldType) && fieldType != "double" &&
                  fieldType != "float" &&
                  fieldType != "i1" && !isNativeViewField &&
                  fieldTypeName != "nil" && !isClassType(fieldTypeName))))) {
                return failAt(ins,
                    "[emit-llvm] no native field storage ABI for '" +
                    className + "." + layout->fieldNames[fieldIndex] + "'");
            }

            IRType::ClassFieldDefault fieldDefault;
            if (fieldIndex < layout->fieldDefaults.size()) {
                fieldDefault = layout->fieldDefaults[fieldIndex];
            }
            const bool hasInitializer = fieldInitializers[fieldIndex] != nullptr;
            if (!hasInitializer &&
                fieldDefault.kind == IRType::ClassFieldDefaultKind::Unsupported) {
                return failAt(ins,
                    "[emit-llvm] field '" + className + "." +
                    layout->fieldNames[fieldIndex] +
                    "' has a non-constant default but no per-instance initializer metadata");
            }

            const std::string fieldAddress = freshTemporary();
            emit(fieldAddress + " = getelementptr inbounds " + llvmStructType(className) +
                 ", ptr " + object + ", i32 0, i32 " + std::to_string(fieldIndex));

            if (hasInitializer) {
                if (isDynamicField) {
                    emit("call void @zeus_value_init_nil(ptr " + fieldAddress + ")");
                } else if (fieldType == "double" || fieldType == "float") {
                    emit("store " + fieldType + " 0.000000e+00, ptr " + fieldAddress);
                } else if (isIntegerLLVMType(fieldType)) {
                    emit("store " + fieldType + " 0, ptr " + fieldAddress);
                } else if (fieldType == "i1") {
                    emit("store i1 0, ptr " + fieldAddress);
                } else if (fieldType == "ptr") {
                    emit("store ptr null, ptr " + fieldAddress);
                } else {
                    return failAt(ins,
                        "[emit-llvm] unsupported safe zero initialization for field '" +
                        className + "." + layout->fieldNames[fieldIndex] + "'");
                }
                continue;
            }

            if (isDynamicField) {
                switch (fieldDefault.kind) {
                    case IRType::ClassFieldDefaultKind::Zero:
                    case IRType::ClassFieldDefaultKind::Null:
                        emit("call void @zeus_value_init_nil(ptr " + fieldAddress + ")");
                        break;
                    case IRType::ClassFieldDefaultKind::Integer:
                        emit("call void @zeus_value_init_i64(ptr " + fieldAddress +
                             ", i64 " + std::to_string(fieldDefault.integerValue) + ")");
                        break;
                    case IRType::ClassFieldDefaultKind::Boolean:
                        emit("call void @zeus_value_init_bool(ptr " + fieldAddress +
                             ", i32 " + (fieldDefault.booleanValue ? "1" : "0") + ")");
                        break;
                    case IRType::ClassFieldDefaultKind::String: {
                        const std::string symbol = poolStr(fieldDefault.stringValue);
                        const std::string stringPointer = freshTemporary();
                        emit(stringPointer + " = getelementptr inbounds [" +
                             std::to_string(fieldDefault.stringValue.size() + 1) +
                             " x i8], ptr " + symbol + ", i64 0, i64 0");
                        emit("call void @zeus_value_from_string_data(ptr " + fieldAddress +
                             ", ptr " + stringPointer + ", i64 " +
                             std::to_string(fieldDefault.stringValue.size()) + ")");
                        break;
                    }
                    case IRType::ClassFieldDefaultKind::Bytes: {
                        const std::string symbol = poolStr(fieldDefault.bytesValue);
                        const std::string bytesPointer = freshTemporary();
                        emit(bytesPointer + " = getelementptr inbounds [" +
                             std::to_string(fieldDefault.bytesValue.size() + 1) +
                             " x i8], ptr " + symbol + ", i64 0, i64 0");
                        emit("call void @zeus_value_from_bytes(ptr " + fieldAddress +
                             ", ptr " + bytesPointer + ", i64 " +
                             std::to_string(fieldDefault.bytesValue.size()) + ")");
                        break;
                    }
                    case IRType::ClassFieldDefaultKind::EmptyArray:
                        emit("call void @zeus_value_array_build(ptr " + fieldAddress +
                             ", i64 0, ptr null)");
                        break;
                    case IRType::ClassFieldDefaultKind::EmptyDict:
                        emit("call void @zeus_value_dict_build(ptr " + fieldAddress +
                             ", i64 0, ptr null)");
                        break;
                    case IRType::ClassFieldDefaultKind::Unsupported:
                        return failAt(ins,
                            "[emit-llvm] unsupported field default initializer for '" +
                            className + "." + layout->fieldNames[fieldIndex] + "'");
                }
                continue;
            }

            std::string initializer;
            if (fieldType == "double" || fieldType == "float") {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero) {
                    initializer = "0.000000e+00";
                } else if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Integer) {
                    initializer = emitNativeFloatingConversion(
                        std::to_string(fieldDefault.integerValue), "i64", fieldType,
                        false, emit, freshTemporary);
                } else {
                    return failAt(ins, "[emit-llvm] incompatible floating field default");
                }
            } else if (isIntegerLLVMType(fieldType)) {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Integer) {
                    initializer = emitNativeIntegerConversion(
                        std::to_string(fieldDefault.integerValue), "i64", fieldType,
                        false, fieldABI && fieldABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::UnsignedInteger,
                        emit, freshTemporary);
                } else if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero) {
                    initializer = "0";
                } else {
                    return failAt(ins,
                        "[emit-llvm] int field default has an incompatible value for '" +
                        className + "." + layout->fieldNames[fieldIndex] + "'");
                }
            } else if (fieldType == "i1") {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Boolean) {
                    initializer = fieldDefault.booleanValue ? "1" : "0";
                } else if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero) {
                    initializer = "0";
                } else {
                    return failAt(ins,
                        "[emit-llvm] bool field default has an incompatible value for '" +
                        className + "." + layout->fieldNames[fieldIndex] + "'");
                }
            } else if (fieldType == "ptr") {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero ||
                    fieldDefault.kind == IRType::ClassFieldDefaultKind::Null) {
                    initializer = "null";
                } else if ((fieldDefault.kind == IRType::ClassFieldDefaultKind::String &&
                            fieldABI != nullptr &&
                            fieldABI->valueABI == ZeusBuiltins::NativeValueABI::NativeString) ||
                           (fieldDefault.kind == IRType::ClassFieldDefaultKind::Bytes &&
                            fieldABI != nullptr &&
                            fieldABI->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes)) {
                    const std::string& contents =
                        fieldDefault.kind == IRType::ClassFieldDefaultKind::Bytes
                            ? fieldDefault.bytesValue : fieldDefault.stringValue;
                    const std::string symbol = poolStr(contents);
                    const std::string dataPointer = freshTemporary();
                    emit(dataPointer + " = getelementptr inbounds [" +
                         std::to_string(contents.size() + 1) +
                         " x i8], ptr " + symbol + ", i64 0, i64 0");
                    initializer = freshTemporary();
                    emit(initializer + " = call ptr @" +
                         std::string(ZeusBuiltins::nativePointerFromDataHelper(fieldABI)) +
                         "(ptr " + dataPointer + ", i64 " +
                         std::to_string(contents.size()) + ")");
                } else {
                    return failAt(ins,
                        "[emit-llvm] pointer field default has an incompatible value for '" +
                        className + "." + layout->fieldNames[fieldIndex] + "'");
                }
            } else {
                return failAt(ins,
                    "[emit-llvm] unsupported LLVM field storage for '" +
                    className + "." + layout->fieldNames[fieldIndex] + "'");
            }

            emit("store " + fieldType + " " + initializer + ", ptr " + fieldAddress);
        }

        emit("call void @zeus_class_ref_register(ptr " + object + ", ptr " +
             classLifetimeFinalizer(className) + ", ptr " + poolStr(className) + ")");
        emit("call void @zeus_class_gc_register(ptr " + object + ", ptr " +
             poolStr(className) + ")");

        for (size_t fieldIndex = 0; fieldIndex < fieldInitializers.size(); ++fieldIndex) {
            const FunctionSignature* initializer = fieldInitializers[fieldIndex];
            if (initializer == nullptr) continue;

            const std::string& fieldTypeName = layout->fieldTypes[fieldIndex];
            const std::string fieldType = llvmFieldType(fieldTypeName);
            const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
            const std::string fieldAddress = freshTemporary();
            emit(fieldAddress + " = getelementptr inbounds " + llvmStructType(className) +
                 ", ptr " + object + ", i32 0, i32 " + std::to_string(fieldIndex));

            if (usesZeusValueABI(fieldTypeName)) {
                emit("call void " + llvmCallTarget(*initializer) +
                     "(ptr " + fieldAddress + ", ptr " + object + ")");
                continue;
            }

            const std::string resultValue = freshTemporary();
            emit(resultValue + " = call " + llvmType(initializer->returnType) + " " +
                 llvmCallTarget(*initializer) + "(ptr " + object + ")");
            if (isClassType(fieldTypeName)) {
                emit("call void @zeus_class_ref_retain_edge(ptr " + resultValue + ")");
                emit("store ptr " + resultValue + ", ptr " + fieldAddress);
                emit("call void @zeus_class_ref_release(ptr " + resultValue + ")");
            } else if (fieldABI != nullptr && fieldABI->valueABI ==
                       ZeusBuiltins::NativeValueABI::NativeBytes) {
                emit("store ptr " + resultValue + ", ptr " + fieldAddress);
            } else if (fieldABI != nullptr && fieldABI->valueABI ==
                       ZeusBuiltins::NativeValueABI::NativeString) {
                // A native string initializer returns one owned reference;
                // move that reference directly into the field slot.
                emit("store ptr " + resultValue + ", ptr " + fieldAddress);
            } else if (ZeusBuiltins::isNativePointerABI(fieldABI)) {
                const std::string stableView = freshTemporary();
                emit(stableView + " = call ptr @" +
                     std::string(ZeusBuiltins::nativePointerInternHelper(fieldABI)) +
                     "(ptr " + resultValue + ")");
                emit("store ptr " + stableView + ", ptr " + fieldAddress);
            } else {
                emit("store " + fieldType + " " + resultValue + ", ptr " + fieldAddress);
            }
        }

        std::set<std::string> releasedDynamicArguments;
        std::vector<std::string> nativeBytesArguments;
        if (constructor != nullptr) {
            const bool constructorReturnsDynamic = usesZeusValueABI(constructor->returnType);
            const std::string constructorReturnType = constructorReturnsDynamic
                ? "void"
                : llvmType(constructor->returnType);
            if (constructorReturnType.empty()) {
                return failAt(ins, "[emit-llvm] init 构造函数返回类型没有 LLVM ABI 表示");
            }

            std::string constructorResult;
            std::string callText;
            if (constructorReturnsDynamic) {
                constructorResult = freshTemporary();
                emit(constructorResult + " = alloca %ZeusValue, align 8");
                callText = "call void " + llvmCallTarget(constructor->name) +
                           "(ptr " + constructorResult + ", ptr " + object;
            } else {
                callText = "call " + constructorReturnType + " " +
                           llvmCallTarget(constructor->name) + "(ptr " + object;
            }

            std::vector<std::string> dynamicArguments;
            std::vector<bool> ownsDynamicArguments;
            std::vector<std::string> nativeStringArguments;
            for (size_t i = 0; i + 1 < constructor->parameterTypes.size(); ++i) {
                std::string argument;
                bool ownsDynamic = false;
                bool ownsNativeString = false;
                const std::string& declaredType = constructor->parameterTypes[i + 1];
                if (i < arguments.size()) {
                    if (!convertArgument(arguments[i], declaredType, argument,
                                          ownsDynamic, ownsNativeString)) {
                        return false;
                    }
                } else {
                    NativeValue placeholder;
                    if (!emitOptionalArgumentPlaceholder(
                            declaredType, placeholder, emit, freshTemporary)) {
                        return failAt(ins,
                            "[emit-llvm] init constructor has no ABI placeholder for an omitted argument");
                    }
                    argument = placeholder.value;
                }
                callText += ", " + functionParameterABIType(declaredType) + " " + argument;
                if (usesZeusValueABI(declaredType) && i < arguments.size()) {
                    dynamicArguments.push_back(argument);
                    ownsDynamicArguments.push_back(ownsDynamic);
                }
                if (declaredType == "bytes" && i < arguments.size() &&
                    arguments[i].dynamic) {
                    nativeBytesArguments.push_back(argument);
                }
                if (ownsNativeString) nativeStringArguments.push_back(argument);
            }
            if (constructor->hasDefaultParameters()) {
                callText += ", i64 " + std::to_string(providedPrefixMask(arguments.size() + 1));
            }
            callText += ")";
            emit(callText);
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i] &&
                    releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                    emit("call void @zeus_value_release(ptr " + dynamicArguments[i] + ")");
                }
            }
            for (const auto& argument : nativeBytesArguments) {
                emit("call void @zeus_bytes_release(ptr " + argument + ")");
            }
            for (const auto& argument : nativeStringArguments) {
                emit("call void @zeus_string_release(ptr " + argument + ")");
            }
            if (constructorReturnsDynamic) {
                emit("call void @zeus_value_release(ptr " + constructorResult + ")");
            }
        }

        for (const auto& argument : arguments) {
            if (argument.dynamic && argument.ownsDynamic &&
                releasedDynamicArguments.insert(argument.value).second) {
                emit("call void @zeus_value_release(ptr " + argument.value + ")");
            }
            if (argument.ownsNativeBytes) {
                emit("call void @zeus_bytes_release(ptr " + argument.value + ")");
            }
            if (argument.ownsNativeString) {
                emit("call void @zeus_string_release(ptr " + argument.value + ")");
            }
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        instance = NativeValue{ object, "ptr", false, false, className };
        instance.ownsClass = true;
        return true;
    }

    template <typename EmitLine, typename FreshTemporary, typename Fail>
    bool emitConstructedValueAsDynamic(
        const IRType::IRInstr& ins,
        const NativeValue& value,
        const std::string& dynamicResult,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        Fail&& failAt
    ) {
        if (value.dynamic && value.type == "ptr") {
            emit("call void @zeus_value_clone(ptr " + dynamicResult +
                 ", ptr " + value.value + ")");
            if (value.ownsDynamic) {
                emit("call void @zeus_value_release(ptr " + value.value + ")");
            }
            return true;
        }

        if (value.type == "double") {
            emit("call void @zeus_value_init_double(ptr " + dynamicResult +
                 ", double " + value.value + ")");
            return true;
        }
        if (value.type == "i64") {
            emit("call void @zeus_value_init_i64(ptr " + dynamicResult +
                 ", i64 " + value.value + ")");
            return true;
        }
        if (value.type == "i1") {
            const std::string booleanValue = freshTemporary();
            emit(booleanValue + " = zext i1 " + value.value + " to i32");
            emit("call void @zeus_value_init_bool(ptr " + dynamicResult +
                 ", i32 " + booleanValue + ")");
            return true;
        }

        if (value.type == "ptr" && isClassType(value.sourceType)) {
            emit("call void @zeus_value_from_class_ptr(ptr " + dynamicResult +
                 ", ptr " + value.value + ", ptr " + poolStr(value.sourceType) + ")");
            if (value.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
            }
            return true;
        }

        if (value.type == "ptr") {
            const auto* abi = ZeusBuiltins::findNativeTypeABI(value.sourceType);
            if (ZeusBuiltins::isNativePointerABI(abi)) {
                    const std::string helper(
                        ZeusBuiltins::nativePointerToDynamicHelper(abi));
                if (!helper.empty()) {
                    emit("call void @" + helper + "(ptr " + dynamicResult +
                         ", ptr " + value.value + ")");
                    if (value.ownsNativeBytes) {
                        emit("call void @zeus_bytes_release(ptr " + value.value + ")");
                    }
                    return true;
                }
            }
        }

        return failAt(ins,
            "[emit-llvm] dynamic new cannot box factory result type '" +
            value.sourceType + "'");
    }

    template <typename EmitLine, typename EmitLabel, typename FreshTemporary,
              typename ConvertArgument, typename Fail>
    bool emitDynamicClassConstruction(
        const IRType::IRInstr& ins,
        const NativeValue& classValue,
        const std::vector<NativeValue>& arguments,
        NativeValue& instance,
        EmitLine&& emit,
        EmitLabel&& emitLabel,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        Fail&& failAt
    ) {
        if (!classValue.dynamic || classValue.type != "ptr") {
            return failAt(ins, "[emit-llvm] dynamic new requires a ZeusValue class handle");
        }
        if (classLayouts.empty() || classLayouts.size() > static_cast<size_t>(
                std::numeric_limits<int32_t>::max())) {
            return failAt(ins, "[emit-llvm] dynamic new has no valid AOT class table");
        }

        const std::string dynamicResult = freshTemporary();
        emit(dynamicResult + " = alloca %ZeusValue, align 8");
        emit("call void @zeus_value_init_nil(ptr " + dynamicResult + ")");
        const std::string classId = freshTemporary();
        emit(classId + " = call i32 @zeus_value_require_class_handle(ptr " +
             classValue.value + ", i32 " +
             std::to_string(classLayouts.size()) + ")");
        if (classValue.ownsDynamic) {
            emit("call void @zeus_value_release(ptr " + classValue.value + ")");
        }

        const std::string labelSeed = freshTemporary();
        const std::string labelStem = labelSeed.size() > 1 && labelSeed.front() == '%'
            ? labelSeed.substr(1) : labelSeed;
        const std::string endLabel = "dynamic_new_end_" + labelStem;
        int32_t index = 0;
        for (const auto& entry : classLayouts) {
            const std::string caseLabel =
                "dynamic_new_case_" + labelStem + "_" + std::to_string(index);
            const std::string nextLabel =
                "dynamic_new_next_" + labelStem + "_" + std::to_string(index);
            const std::string matches = freshTemporary();
            emit(matches + " = icmp eq i32 " + classId + ", " +
                 std::to_string(index));
            emit("br i1 " + matches + ", label %" + caseLabel +
                 ", label %" + nextLabel);
            emitLabel(caseLabel);

            NativeValue constructed;
            if (!emitClassConstruction(
                    ins, entry.first, arguments, constructed,
                    emit, emitLabel, freshTemporary, convertArgument, failAt)) {
                return false;
            }
            if (!emitConstructedValueAsDynamic(
                    ins, constructed, dynamicResult, emit, freshTemporary, failAt)) {
                return false;
            }
            emit("br label %" + endLabel);
            emitLabel(nextLabel);
            ++index;
        }

        emit("unreachable");
        emitLabel(endLabel);
        instance = NativeValue{
            dynamicResult, "ptr", false, true, std::string(), true
        };
        return true;
    }

    std::string emitClassLifetimeFinalizers() const {
        std::string generated;
        for (const auto& entry : classLayouts) {
            // See classNeedsRuntimeMachinery: every live instance needs a
            // finalizer, and every referenced finalizer must be emitted.
            if (!classNeedsRuntimeMachinery(entry.first)) {
                continue;
            }
            const std::string& className = entry.first;
            const ClassLayout& layout = entry.second;
            const FunctionSignature* destructor = nullptr;
            for (const auto& candidate : functionSignatures) {
                if (candidate.second.ownerType == className &&
                    candidate.second.operatorSymbol == "~") {
                    destructor = &candidate.second;
                    break;
                }
            }
            if (destructor != nullptr && !destructor->isExternal &&
                reachableFunctionChunks.find(destructor->chunkIndex) ==
                    reachableFunctionChunks.end()) {
                destructor = nullptr;
            }

            generated += "define void " + classLifetimeFinalizer(className) +
                "(ptr %object) personality ptr @__CxxFrameHandler3 {\nentry:\n";
            int temporaryIndex = 0;
            auto fresh = [&]() {
                return "%life" + std::to_string(temporaryIndex++);
            };

            if (destructor != nullptr) {
                const bool returnsDynamic = usesZeusValueABI(destructor->returnType);
                if (returnsDynamic) {
                    const std::string result = fresh();
                    generated += "  " + result + " = alloca %ZeusValue, align 8\n";
                    generated += "  call void " + llvmCallTarget(destructor->name) +
                        "(ptr " + result + ", ptr %object)\n";
                    generated += "  call void @zeus_value_release(ptr " + result + ")\n";
                } else {
                    const std::string returnType = llvmType(destructor->returnType);
                    if (returnType.empty() || returnType == "void") {
                        generated += "  call void " + llvmCallTarget(destructor->name) +
                            "(ptr %object)\n";
                    } else {
                        generated += "  call " + returnType + " " +
                            llvmCallTarget(destructor->name) + "(ptr %object)\n";
                    }
                }
            }

            for (size_t fieldIndex = 0; fieldIndex < layout.fieldTypes.size(); ++fieldIndex) {
                const std::string& fieldType = layout.fieldTypes[fieldIndex];
                const bool classField = isClassType(fieldType);
                const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldType);
                const bool nativeBytesField = fieldABI != nullptr &&
                    fieldABI->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes;
                const bool nativeStringField = fieldABI != nullptr &&
                    fieldABI->valueABI == ZeusBuiltins::NativeValueABI::NativeString;
                if (!usesZeusValueABI(fieldType) && !classField &&
                    !nativeBytesField && !nativeStringField) continue;

                const std::string address = fresh();
                generated += "  " + address + " = getelementptr inbounds " +
                    llvmStructType(className) + ", ptr %object, i32 0, i32 " +
                    std::to_string(fieldIndex) + "\n";
                if (classField) {
                    const std::string fieldValue = fresh();
                    generated += "  " + fieldValue + " = load ptr, ptr " + address + "\n";
                    generated += "  call void @zeus_class_ref_release_edge(ptr " +
                        fieldValue + ")\n";
                } else if (nativeBytesField) {
                    const std::string fieldValue = fresh();
                    generated += "  " + fieldValue + " = load ptr, ptr " + address + "\n";
                    generated += "  call void @zeus_bytes_release(ptr " + fieldValue + ")\n";
                } else if (nativeStringField) {
                    const std::string fieldValue = fresh();
                    generated += "  " + fieldValue + " = load ptr, ptr " + address + "\n";
                    generated += "  call void @zeus_string_release(ptr " + fieldValue + ")\n";
                } else {
                    generated += "  call void @zeus_value_slot_unregister_gc_edge(ptr " +
                        address + ")\n";
                    generated += "  call void @zeus_value_release(ptr " + address + ")\n";
                }
            }

            generated += "  ret void\n}\n\n";
        }
        return generated;
    }

    std::string emitClassEmbeddedSlotHelpers() {
        std::string generated;
        for (const auto& classEntry : embeddedSlotTypes) {
            const std::string& className = classEntry;
            const std::string functionName = classEmbeddedSlotStore(className);
            const std::string typeName = poolStr(className);
            generated += "define internal void " + functionName +
                "(ptr %destination, ptr %source) personality ptr @__CxxFrameHandler3 {\n";
            const std::string storageSize = llvmValueStorageSize(className);
            generated += "entry:\n";
            generated += "  call void @zeus_class_ref_embedded_slot_assign(ptr %destination, ptr %source, ptr " +
                classLifetimeFinalizer(className) + ", ptr " + typeName + ", i64 " +
                storageSize + ")\n";
            generated += "  ret void\n}\n\n";
        }
        return generated;
    }

    template <typename EmitLine, typename EmitLabel, typename FreshTemporary, typename Fail>
    bool emitClassDelete(
        const IRType::IRInstr& ins,
        const NativeValue& object,
        bool releaseManagedOwner,
        EmitLine&& emit,
        EmitLabel&& emitLabel,
        FreshTemporary&& freshTemporary,
        Fail&& failAt
    ) {
        if (object.type != "ptr" || !isClassType(object.sourceType)) {
            return failAt(ins, "[emit-llvm] delete 目标不是 class 实例");
        }

        const FunctionSignature* destructor = nullptr;
        for (const auto& entry : functionSignatures) {
            const auto& candidate = entry.second;
            if (candidate.ownerType == object.sourceType &&
                candidate.operatorSymbol == "~") {
                destructor = &candidate;
                break;
            }
        }

        bool destructorReturnsDynamic = false;
        std::string destructorReturnType;
        if (destructor != nullptr) {
            if (destructor->parameterTypes.size() != 1 ||
                destructor->parameterTypes.front() != object.sourceType) {
                return failAt(ins, "[emit-llvm] operator[~] 必须只接受 class 实例");
            }
            destructorReturnsDynamic = usesZeusValueABI(destructor->returnType);
            destructorReturnType = destructorReturnsDynamic
                ? "void"
                : llvmType(destructor->returnType);
            if (destructorReturnType.empty()) {
                return failAt(ins, "[emit-llvm] operator[~] 必须声明可表示的返回类型");
            }
        }

        const std::string isNull = freshTemporary();
        const std::string deleteDone = freshTemporary();
        const std::string deleteLive = freshTemporary();
        emit(isNull + " = icmp eq ptr " + object.value + ", null");
        emit("br i1 " + isNull + ", label " + deleteDone +
             ", label " + deleteLive);
        emitLabel(deleteLive);

        const std::string managedResult = freshTemporary();
        const std::string isManaged = freshTemporary();
        const std::string managedLabel = freshTemporary();
        const std::string legacyLabel = freshTemporary();
        emit(managedResult + " = call i32 @zeus_class_ref_destroy_now(ptr " +
             object.value + ")");
        emit(isManaged + " = icmp ne i32 " + managedResult + ", 0");
        emit("br i1 " + isManaged + ", label " + managedLabel +
             ", label " + legacyLabel);
        emitLabel(managedLabel);
        // A retained local remains a dead-object owner until its normal cleanup.
        // Clearing a global slot, in contrast, relinquishes that slot's owner.
        if (releaseManagedOwner) {
            emit("call void @zeus_class_ref_release(ptr " + object.value + ")");
        }
        emit("br label " + deleteDone);
        emitLabel(legacyLabel);

        if (destructor != nullptr) {
            if (destructorReturnsDynamic) {
                const std::string destructorResult = freshTemporary();
                emit(destructorResult + " = alloca %ZeusValue, align 8");
                emit(
                    "call void " + llvmCallTarget(destructor->name) +
                    "(ptr " + destructorResult + ", ptr " + object.value + ")"
                );
                emit("call void @zeus_value_release(ptr " + destructorResult + ")");
            } else {
                emit(
                    "call " + destructorReturnType + " " +
                    llvmCallTarget(destructor->name) + "(ptr " + object.value + ")"
                );
            }
        }

        const ClassLayout* layout = findClassLayout(object.sourceType);
        if (layout != nullptr) {
            for (size_t fieldIndex = 0; fieldIndex < layout->fieldTypes.size(); ++fieldIndex) {
                const std::string& fieldType = layout->fieldTypes[fieldIndex];
                const bool classField = isClassType(fieldType);
                if (!usesZeusValueABI(fieldType) && !classField) continue;
                const std::string fieldAddress = freshTemporary();
                emit(fieldAddress + " = getelementptr inbounds " +
                     llvmStructType(object.sourceType) + ", ptr " + object.value +
                     ", i32 0, i32 " + std::to_string(fieldIndex));
                if (classField) {
                    const std::string fieldValue = freshTemporary();
                    emit(fieldValue + " = load ptr, ptr " + fieldAddress);
                    emit("call void @zeus_class_ref_release_edge(ptr " + fieldValue + ")");
                } else {
                    emit("call void @zeus_value_release(ptr " + fieldAddress + ")");
                }
            }
        }

        emit("call void @zeus_class_pointer_invalidate(ptr " + object.value + ")");
        emit("call void @free(ptr " + object.value + ")");
        emit("br label " + deleteDone);
        emitLabel(deleteDone);
        return true;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitLLVMAddressFromStorage(
        const std::string& nativeAddress,
        const std::string& storageABI,
        const std::string& sourceType,
        bool revealView,
        bool writable,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        std::string& error
    ) {
        if (nativeAddress.empty() ||
            (storageABI != "native" && storageABI != "zeusvalue" && storageABI != "raw")) {
            error = "@addr could not resolve a stable variable storage slot";
            return false;
        }
        std::string valueAddress = nativeAddress;
        if (!revealView && storageABI != "raw") {
            const std::string size = storageABI == "zeusvalue" ? "16" :
                (sourceType.empty() ? "8" : llvmTypeStorageSize(llvmFieldType(sourceType)));
            valueAddress = freshTemporary();
            emit(valueAddress + " = call ptr @zeus_lvalue_register(ptr " + nativeAddress +
                 ", ptr " + poolStr(sourceType) + ", i32 " +
                 (storageABI == "zeusvalue" ? "1" : "0") +
                 ", i64 " + size + ", i32 " + (writable ? "1" : "0") + ")");
        } else if (revealView && storageABI == "zeusvalue") {
            valueAddress = freshTemporary();
            emit(valueAddress + " = call ptr @zeus_value_native_address(ptr " +
                 nativeAddress + ", i32 1)");
            emit("call void @zeus_lvalue_register_revealed(ptr " + nativeAddress +
                 ", ptr " + nativeAddress + ", ptr " + valueAddress + ")");
        } else if (revealView && storageABI == "native") {
            const auto* abi = ZeusBuiltins::findNativeTypeABI(sourceType);
            if (ZeusBuiltins::isNativePointerABI(abi) || isClassType(sourceType)) {
                valueAddress = freshTemporary();
                emit(valueAddress + " = load ptr, ptr " + nativeAddress + ", align 8");
            }
            if (ZeusBuiltins::isNativePointerABI(abi)) {
                emit("call void @zeus_lvalue_register_native_buffer(ptr " + nativeAddress +
                     ", ptr " + valueAddress + ", ptr " + poolStr(sourceType) + ")");
            } else {
                const std::string size =
                (isClassType(sourceType) ? llvmValueStorageSize(sourceType) : llvmTypeStorageSize(llvmFieldType(sourceType)));
                emit("call void @zeus_lvalue_register_raw_owned(ptr " + nativeAddress +
                     ", ptr " + valueAddress + ", i64 " + size + ", i32 0)");
            }
        }
        const std::string integerAddress = freshTemporary();
        emit(integerAddress + " = ptrtoint ptr " + valueAddress + " to i64");
        resultValue = NativeValue{integerAddress, "i64", false, false, "int"};
        resultValue.addressStorageABI = revealView ? "raw" : storageABI;
        return true;
    }

    bool validateAddressableNativeFunction(
        const FunctionSignature* signature,
        const std::string& functionName,
        std::string& error
    ) const {
        if (signature == nullptr) {
            error = "@addr function '" + functionName + "' could not be resolved";
            return false;
        }
        if (signature->nativeSymbol.empty()) {
            error = "@addr on a function requires @native linkage";
            return false;
        }
        if (signature->isGenericTemplate) {
            error = "@addr cannot take the address of a generic function template";
            return false;
        }
        if (signature->captureCount != 0) {
            error = "@addr cannot take the address of a capturing closure";
            return false;
        }
        if (signature->hasDefaultParameters() ||
            signature->hasVariadicArgs ||
            signature->hasVariadicKwargs) {
            error = "@addr requires a fixed-arity @native function without default or variadic parameters";
            return false;
        }
        const bool returnsVoid = signature->returnType.empty() ||
            signature->returnType == "nil" || signature->returnType == "void";
        if (!returnsVoid && usesZeusValueABI(signature->returnType)) {
            error = "@addr requires a @native function with a C ABI return type or void";
            return false;
        }
        return true;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitLLVMFunctionAddress(
        const std::string& functionName,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        std::string& error
    ) const {
        const FunctionSignature* signature =
            resolveAddressableFunctionSignature(functionName);
        if (!validateAddressableNativeFunction(signature, functionName, error)) {
            return false;
        }
        const std::string targetSymbol = llvmCallTarget(*signature);
        const std::string integerAddress = freshTemporary();
        emit(integerAddress + " = ptrtoint ptr " + targetSymbol + " to i64");
        resultValue = NativeValue{integerAddress, "i64", false, false, "int"};
        return true;
    }

    bool llvmFaddrAccessWidth(
        const IRType::IRInstr& instruction,
        int64_t& width,
        std::string& error
    ) const {
        width = 0;
        if (instruction.args.size() <= 1) return true;
        if (std::holds_alternative<std::string>(instruction.args[1])) {
            const std::string& mode = std::get<std::string>(instruction.args[1]);
            if (mode == "length" || mode == "raw_range" ||
                mode == "slot" || mode == "slot_clear") {
                return true;
            }
        }
        if (!std::holds_alternative<LexerType::Value>(instruction.args[1])) {
            error = "invalid @faddr size metadata";
            return false;
        }
        const auto& literal = std::get<LexerType::Value>(instruction.args[1]);
        const auto* integer = std::get_if<ZeusType::BigInteger>(&literal.data);
        if (!integer || !integer->tryToInt64(width) ||
            (width != 1 && width != 2 && width != 4 && width != 8)) {
            error = "@faddr integer width must be a literal 1, 2, 4, or 8";
            return false;
        }
        return true;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitLLVMFetchAddress(
        const NativeValue& address,
        const std::string& targetType,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        std::string& error,
        const NativeValue* stringByteLength = nullptr,
        int64_t integerByteWidth = 0,
        bool nativeSlot = false,
        bool nativeSlotClear = false,
        bool rawBytesRange = false,
        bool uncheckedRawFaddr = false
    ) {
        if (address.functionRef || address.dynamic || address.type != "i64") {
            error = std::string("@faddr requires an int address produced inside using LLVM (actual type: ") +
                (address.dynamic ? "dynamic " : "") + address.type +
                (address.sourceType.empty() ? std::string() : ", source: " + address.sourceType) + ")";
            return false;
        }

        const bool completeTypedIntSlot = integerByteWidth == 8 &&
            targetType == "int" && !address.addressStorageABI.empty() &&
            address.addressStorageABI != "raw";
        if (uncheckedRawFaddr &&
            (targetType != "int" || integerByteWidth <= 0 ||
             nativeSlot || nativeSlotClear || rawBytesRange ||
             stringByteLength != nullptr)) {
            error = "@no_safepoint @faddr supports only raw integer-width reads";
            return false;
        }

        if (!uncheckedRawFaddr && !nativeSlot && !nativeSlotClear && stringByteLength == nullptr &&
            (integerByteWidth == 0 || completeTypedIntSlot)) {
            return emitLValueFetch(address, targetType, resultValue, emit, freshTemporary, error);
        }

        const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
        const std::string loadedType = llvmType(targetType);
        if (loadedType.empty() ||
            (abi == nullptr && !isClassType(targetType) &&
             structTypes.find(targetType) == structTypes.end())) {
            error = "@faddr has no native storage mapping for type '" + targetType + "'";
            return false;
        }
        const bool scalarSlot = abi != nullptr &&
            (abi->valueABI == ZeusBuiltins::NativeValueABI::Int64 ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::SignedInteger ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::Bool1);
        const bool rawRangeSlot = abi != nullptr &&
            abi->valueABI == ZeusBuiltins::NativeValueABI::RawBytesRange;
        if (nativeSlot && !scalarSlot && !ZeusBuiltins::isNativePointerABI(abi) &&
            !rawRangeSlot && !isClassType(targetType)) {
            error = "@faddr slot requires a native scalar, view, or class type";
            return false;
        }

        const std::string nativeAddress = freshTemporary();
        emit(nativeAddress + " = inttoptr i64 " + address.value + " to ptr");

        if (stringByteLength == nullptr && !uncheckedRawFaddr) {
            const std::string accessSize = integerByteWidth > 0 ? std::to_string(integerByteWidth) :
                (isClassType(targetType) && nativeSlot ? llvmValueStorageSize(targetType) :
                 llvmTypeStorageSize(llvmFieldType(targetType)));
            emit("call void @zeus_lvalue_require_raw(ptr " + nativeAddress +
                 ", i64 " + accessSize + ", i32 " + (nativeSlotClear ? "1" : "0") + ")");
        }

        if (nativeSlotClear) {
            if (rawRangeSlot) {
                emit("store %ZeusRawBytesRange zeroinitializer, ptr " +
                     nativeAddress + ", align 8");
                resultValue = NativeValue{
                    "zeroinitializer", "%ZeusRawBytesRange", false, false,
                    targetType
                };
                return true;
            }
            if (isClassType(targetType)) {
                embeddedSlotTypes.insert(targetType);
                emit("call void " + classEmbeddedSlotStore(targetType) +
                     "(ptr " + nativeAddress + ", ptr null)");
                resultValue = NativeValue{"null", "ptr", false, false, targetType};
                return true;
            }
            if (abi != nullptr &&
                abi->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint) {
                const std::string floatType(abi->llvmType);
                emit("store " + floatType + " 0.000000e+00, ptr " + nativeAddress + ", align 1");
                resultValue = NativeValue{
                    "0.000000e+00", floatType, false, false, targetType};
                return true;
            }
            if (abi != nullptr &&
                abi->valueABI == ZeusBuiltins::NativeValueABI::Int64) {
                emit("store i64 0, ptr " + nativeAddress + ", align 1");
                resultValue = NativeValue{"0", "i64", false, false, targetType};
                return true;
            }
            if (abi != nullptr &&
                abi->valueABI == ZeusBuiltins::NativeValueABI::Bool1) {
                emit("store i1 0, ptr " + nativeAddress + ", align 1");
                resultValue = NativeValue{"0", "i1", false, false, targetType};
                return true;
            }
            if (ZeusBuiltins::isNativePointerABI(abi)) {
                emit("store ptr null, ptr " + nativeAddress + ", align 8");
                resultValue = NativeValue{"null", "ptr", false, false, targetType};
                return true;
            }
            error = "@faddr slot_clear does not support type '" + targetType + "'";
            return false;
        }

        if (nativeSlot && isClassType(targetType)) {
            embeddedSlotTypes.insert(targetType);
            const std::string storageSize = llvmValueStorageSize(targetType);
            const std::string slotObject = freshTemporary();
            emit(slotObject + " = call ptr @zeus_class_ref_embedded_slot_object(ptr " +
                 nativeAddress + ", ptr " + poolStr(targetType) + ", i64 " +
                 storageSize + ")");
            emit("call void @zeus_class_ref_retain(ptr " + slotObject + ")");
            resultValue = NativeValue{
                slotObject, "ptr", false, false, targetType
            };
            resultValue.ownsClass = true;
            return true;
        }

        if (rawBytesRange) {
            if (targetType != "&bytes" || stringByteLength == nullptr) {
                error = "@faddr raw range requires the &bytes type and a length";
                return false;
            }
            std::string lengthValue;
            if (stringByteLength->functionRef) {
                error = "@faddr raw range length must be an int";
                return false;
            }
            if (stringByteLength->dynamic) {
                lengthValue = freshTemporary();
                emit(lengthValue + " = call i64 @zeus_value_require_i64(ptr " +
                     stringByteLength->value + ")");
            } else if (stringByteLength->type == "i64") {
                lengthValue = stringByteLength->value;
            } else {
                error = "@faddr raw range length must be an int";
                return false;
            }
            emit("call void @zeus_raw_bytes_range_validate(i64 " +
                 address.value + ", i64 " + lengthValue + ")");
            const std::string withAddress = freshTemporary();
            const std::string withLength = freshTemporary();
            resultValue = NativeValue{
                withLength, "%ZeusRawBytesRange", false, false, "&bytes"
            };
            emit(withAddress + " = insertvalue %ZeusRawBytesRange zeroinitializer, ptr " +
                 nativeAddress + ", 0");
            emit(withLength + " = insertvalue %ZeusRawBytesRange " + withAddress +
                 ", i64 " + lengthValue + ", 1");
            if (stringByteLength->dynamic && stringByteLength->ownsDynamic) {
                emit("call void @zeus_value_release(ptr " +
                     stringByteLength->value + ")");
            }
            return true;
        }

        if (ZeusBuiltins::isNativePointerABI(abi)) {
            if (stringByteLength != nullptr) {
                if (abi->valueABI != ZeusBuiltins::NativeValueABI::NativeString &&
                    abi->valueABI != ZeusBuiltins::NativeValueABI::NativeBytes) {
                    error = "@faddr byte length requires string or bytes";
                    return false;
                }
                std::string lengthValue;
                if (stringByteLength->functionRef) {
                    error = "@faddr string length must be an int";
                    return false;
                }
                if (stringByteLength->dynamic) {
                    lengthValue = freshTemporary();
                    emit(lengthValue + " = call i64 @zeus_value_require_i64(ptr " +
                         stringByteLength->value + ")");
                } else if (stringByteLength->type == "i64") {
                    lengthValue = stringByteLength->value;
                } else {
                    error = "@faddr string length must be an int";
                    return false;
                }
                const std::string copiedView = freshTemporary();
                emit("call void @zeus_lvalue_require_raw(ptr " + nativeAddress +
                     ", i64 " + lengthValue + ", i32 0)");
                if (abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                    emit(copiedView + " = call ptr @zeus_string_copy_from_data(ptr " +
                         nativeAddress + ", i64 " + lengthValue + ")");
                } else {
                    emit(copiedView + " = call ptr @zeus_bytes_copy_from_address(i64 " +
                         address.value + ", i64 " + lengthValue + ")");
                }
                if (stringByteLength->dynamic && stringByteLength->ownsDynamic) {
                    emit("call void @zeus_value_release(ptr " +
                         stringByteLength->value + ")");
                }
                resultValue = NativeValue{
                    copiedView, "ptr", false, false, targetType
                };
                resultValue.ownsNativeBytes =
                    abi->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes;
                resultValue.ownsNativeString =
                    abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString;
                return true;
            }
            if (nativeSlot) {
                const std::string loadedView = freshTemporary();
                emit(loadedView + " = load ptr, ptr " + nativeAddress +
                     ", align 8");
                if (abi->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes) {
                    const std::string copied = freshTemporary();
                    emit(copied + " = call ptr @zeus_bytes_clone(ptr " +
                         loadedView + ")");
                    resultValue = NativeValue{
                        copied, "ptr", false, false, targetType
                    };
                    resultValue.ownsNativeBytes = true;
                    return true;
                }
                resultValue = NativeValue{
                    loadedView, "ptr", false, false, targetType
                };
                return true;
            }
            resultValue = NativeValue{
                nativeAddress, "ptr", false, false, targetType
            };
            return true;
        }

        if (abi != nullptr &&
            abi->valueABI == ZeusBuiltins::NativeValueABI::Int64 &&
            integerByteWidth > 0) {
            std::string loadType;
            switch (integerByteWidth) {
                case 1: loadType = "i8"; break;
                case 2: loadType = "i16"; break;
                case 4: loadType = "i32"; break;
                case 8: loadType = "i64"; break;
                default:
                    error = "@faddr int width must be 1, 2, 4, or 8 bytes";
                    return false;
            }
            const std::string rawValue = freshTemporary();
            emit(rawValue + " = load " + loadType + ", ptr " + nativeAddress +
                 ", align 1");
            std::string integerValue = rawValue;
            if (integerByteWidth < 8) {
                integerValue = freshTemporary();
                emit(integerValue + " = zext " + loadType + " " + rawValue + " to i64");
            }
            resultValue = NativeValue{
                integerValue, "i64", false, false, targetType
            };
            return true;
        }

        if (abi != nullptr &&
            abi->valueABI == ZeusBuiltins::NativeValueABI::ZeusValue) {
            const std::string loadedValue = freshTemporary();
            emit(loadedValue + " = alloca %ZeusValue, align 8");
            emit("call void @llvm.memmove.p0.p0.i64(ptr " + loadedValue +
                 ", ptr " + nativeAddress + ", i64 16, i1 false)");
            resultValue = NativeValue{
                loadedValue, "ptr", false, true, targetType, false
            };
            return true;
        }

        const std::string loadedValue = freshTemporary();
        emit(loadedValue + " = load " + loadedType + ", ptr " + nativeAddress +
             ", align 1");
        resultValue = NativeValue{ loadedValue, loadedType, false, false, targetType };
        return true;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitLLVMRawBytesRangeParts(
        const NativeValue& value,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        std::string& address,
        std::string& length,
        std::string& error
    ) {
        NativeValue range = value;
        if (range.dynamic) {
            if (range.sourceType != "&bytes") {
                error = "raw bytes range value has no &bytes type";
                return false;
            }
            const std::string storage = freshTemporary();
            const std::string nativeRange = freshTemporary();
            emit(storage + " = alloca %ZeusRawBytesRange, align 8");
            emit("call void @zeus_value_require_raw_bytes_range(ptr " + storage +
                 ", ptr " + range.value + ")");
            emit(nativeRange + " = load %ZeusRawBytesRange, ptr " + storage +
                 ", align 8");
            range.value = nativeRange;
            range.type = "%ZeusRawBytesRange";
            range.dynamic = false;
        }
        if (range.type != "%ZeusRawBytesRange" || range.sourceType != "&bytes") {
            error = "raw bytes range value has a mismatched native type";
            return false;
        }
        const std::string pointer = freshTemporary();
        address = freshTemporary();
        length = freshTemporary();
        emit(pointer + " = extractvalue %ZeusRawBytesRange " + range.value + ", 0");
        emit(address + " = ptrtoint ptr " + pointer + " to i64");
        emit(length + " = extractvalue %ZeusRawBytesRange " + range.value + ", 1");
        return true;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitLLVMByteRangeStore(
        const NativeValue& address,
        const NativeValue& length,
        const std::string& targetType,
        const NativeValue& source,
        bool rawRange,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        std::string& error
    ) {
        if (address.functionRef || address.dynamic || address.type != "i64") {
            error = "@faddr byte range requires an int address";
            return false;
        }
        if (length.functionRef) {
            error = "@faddr byte range length must be an int";
            return false;
        }
        std::string lengthValue;
        if (length.dynamic) {
            lengthValue = freshTemporary();
            emit(lengthValue + " = call i64 @zeus_value_require_i64(ptr " +
                 length.value + ")");
        } else if (length.type == "i64") {
            lengthValue = length.value;
        } else {
            error = "@faddr byte range length must be an int";
            return false;
        }

        bool releaseSourceBytes = false;
        bool releaseSourceValue = false;
        std::string bytesValue;
        if (!rawRange) {
            if (targetType != "bytes") {
                error = "@faddr length writes require bytes";
                return false;
            }
            if (source.dynamic) {
                bytesValue = freshTemporary();
                emit(bytesValue + " = call ptr @zeus_value_copy_bytes(ptr " +
                     source.value + ")");
                releaseSourceBytes = true;
                releaseSourceValue = source.ownsDynamic;
            } else if (source.type == "ptr" && source.sourceType == "bytes") {
                bytesValue = source.value;
                releaseSourceBytes = source.ownsNativeBytes;
            } else {
                error = "@faddr bytes write requires a bytes value";
                return false;
            }
            emit("call void @zeus_bytes_copy_to_address(ptr " + bytesValue +
                 ", i64 " + address.value + ", i64 " + lengthValue + ")");
        } else {
            if (targetType != "&bytes") {
                error = "@faddr raw range writes require &bytes";
                return false;
            }
            if (source.dynamic) {
                if (source.sourceType == "bytes") {
                    bytesValue = freshTemporary();
                    emit(bytesValue + " = call ptr @zeus_value_copy_bytes(ptr " +
                         source.value + ")");
                    emit("call void @zeus_bytes_store_raw_range(ptr " + bytesValue +
                         ", i64 " + address.value + ", i64 " + lengthValue + ")");
                    releaseSourceBytes = true;
                    releaseSourceValue = source.ownsDynamic;
                } else if (source.sourceType == "&bytes") {
                    const std::string storage = freshTemporary();
                    const std::string range = freshTemporary();
                    const std::string sourceAddress = freshTemporary();
                    const std::string sourceLength = freshTemporary();
                    const std::string sourceAddressInteger = freshTemporary();
                    emit(storage + " = alloca %ZeusRawBytesRange, align 8");
                    emit("call void @zeus_value_require_raw_bytes_range(ptr " +
                         storage + ", ptr " + source.value + ")");
                    emit(range + " = load %ZeusRawBytesRange, ptr " + storage +
                         ", align 8");
                    emit(sourceAddress +
                         " = extractvalue %ZeusRawBytesRange " + range + ", 0");
                    emit(sourceAddressInteger + " = ptrtoint ptr " + sourceAddress +
                         " to i64");
                    emit(sourceLength +
                         " = extractvalue %ZeusRawBytesRange " + range + ", 1");
                    emit("call void @zeus_raw_bytes_range_store(i64 " +
                         address.value + ", i64 " + lengthValue + ", i64 " +
                         sourceAddressInteger + ", i64 " + sourceLength + ")");
                    releaseSourceValue = source.ownsDynamic;
                } else {
                    error = "@faddr raw range write requires bytes or &bytes";
                    return false;
                }
            } else if (source.type == "ptr" && source.sourceType == "bytes") {
                bytesValue = source.value;
                emit("call void @zeus_bytes_store_raw_range(ptr " + bytesValue +
                     ", i64 " + address.value + ", i64 " + lengthValue + ")");
                releaseSourceBytes = source.ownsNativeBytes;
            } else if (source.type == "%ZeusRawBytesRange" &&
                       source.sourceType == "&bytes") {
                const std::string sourceAddress = freshTemporary();
                const std::string sourceLength = freshTemporary();
                const std::string sourceAddressInteger = freshTemporary();
                emit(sourceAddress + " = extractvalue %ZeusRawBytesRange " +
                     source.value + ", 0");
                emit(sourceAddressInteger + " = ptrtoint ptr " + sourceAddress +
                     " to i64");
                emit(sourceLength + " = extractvalue %ZeusRawBytesRange " +
                     source.value + ", 1");
                emit("call void @zeus_raw_bytes_range_store(i64 " + address.value +
                     ", i64 " + lengthValue + ", i64 " + sourceAddressInteger +
                     ", i64 " + sourceLength + ")");
            } else {
                error = "@faddr raw range write requires bytes or &bytes";
                return false;
            }
        }

        if (releaseSourceBytes) {
            emit("call void @zeus_bytes_release(ptr " + bytesValue + ")");
        }
        if (releaseSourceValue) {
            emit("call void @zeus_value_release(ptr " + source.value + ")");
        }
        if (length.dynamic && length.ownsDynamic) {
            emit("call void @zeus_value_release(ptr " + length.value + ")");
        }
        return true;
    }

    template <typename EmitLine, typename FreshTemporary,
              typename ConvertNativeValue, typename BoxDynamicValue>
    bool emitLLVMStoreAddress(
        const NativeValue& address,
        const std::string& targetType,
        const NativeValue& value,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ConvertNativeValue&& convertNativeValue,
        BoxDynamicValue&& boxDynamicValue,
        std::string& error,
        int64_t integerByteWidth = 0,
        bool nativeSlot = false,
        bool uncheckedRawFaddr = false
    ) {
        if (address.functionRef || address.dynamic || address.type != "i64") {
            error = std::string("@faddr assignment requires an int address produced inside using LLVM (actual type: ") +
                (address.dynamic ? "dynamic " : "") + address.type +
                (address.sourceType.empty() ? std::string() : ", source: " + address.sourceType) + ")";
            return false;
        }
        const bool completeTypedIntSlot = integerByteWidth == 8 &&
            targetType == "int" && !address.addressStorageABI.empty() &&
            address.addressStorageABI != "raw";
        if (uncheckedRawFaddr &&
            (targetType != "int" || integerByteWidth <= 0 || nativeSlot)) {
            error = "@no_safepoint @faddr supports only raw integer-width writes";
            return false;
        }
        if (!uncheckedRawFaddr && !nativeSlot &&
            (integerByteWidth == 0 || completeTypedIntSlot)) {
            return emitLValueStore(address, targetType, value, emit, freshTemporary, boxDynamicValue, error);
        }
        const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
        const std::string nativeType = llvmType(targetType);
        if (nativeType.empty() ||
            (abi == nullptr && !isClassType(targetType) &&
             structTypes.find(targetType) == structTypes.end())) {
            error = "@faddr has no native storage mapping for type '" + targetType + "'";
            return false;
        }
        const bool scalarSlot = abi != nullptr &&
            (abi->valueABI == ZeusBuiltins::NativeValueABI::Int64 ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::SignedInteger ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::Bool1);
        if (nativeSlot && !scalarSlot && !ZeusBuiltins::isNativePointerABI(abi) &&
            !isClassType(targetType)) {
            error = "@faddr slot requires a native scalar, view, or class type";
            return false;
        }
        if (value.functionRef &&
            (abi == nullptr ||
             abi->valueABI != ZeusBuiltins::NativeValueABI::ZeusValue)) {
            error = "@faddr can store a function reference only in a ZeusValue slot";
            return false;
        }
        const std::string nativeAddress = freshTemporary();
        emit(nativeAddress + " = inttoptr i64 " + address.value + " to ptr");

        const bool copiesNativeView = ZeusBuiltins::isNativePointerABI(abi) &&
            !nativeSlot && integerByteWidth == 0;
        if (!copiesNativeView && !uncheckedRawFaddr) {
            const std::string accessSize = integerByteWidth > 0
                ? std::to_string(integerByteWidth)
                : (isClassType(targetType) && nativeSlot
                    ? llvmValueStorageSize(targetType)
                    : llvmTypeStorageSize(llvmFieldType(targetType)));
            emit("call void @zeus_lvalue_require_raw(ptr " + nativeAddress +
                 ", i64 " + accessSize + ", i32 1)");
        }

        if (nativeSlot && isClassType(targetType)) {
            embeddedSlotTypes.insert(targetType);
            std::string sourceObject;
            bool sourceConsumed = false;
            bool ownsConvertedNativeString = false;
            if (!convertNativeValue(
                    value, targetType, "ptr", sourceObject, sourceConsumed,
                    ownsConvertedNativeString, error)) {
                return false;
            }
            emit("call void " + classEmbeddedSlotStore(targetType) +
                 "(ptr " + nativeAddress + ", ptr " + sourceObject + ")");
            if (value.dynamic && value.ownsDynamic && !sourceConsumed) {
                emit("call void @zeus_value_release(ptr " + value.value + ")");
            }
            if (value.ownsClass && isManagedClassValue(value)) {
                emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
            }
            return true;
        }

        if (abi != nullptr &&
            abi->valueABI == ZeusBuiltins::NativeValueABI::ZeusValue) {
            std::string boxedValue;
            bool ownsBoxedValue = false;
            if (!boxDynamicValue(value, boxedValue, ownsBoxedValue, error)) {
                return false;
            }
            const std::string copiedValue = freshTemporary();
            emit(copiedValue + " = alloca %ZeusValue, align 8");
            emit("call void @zeus_value_init_nil(ptr " + copiedValue + ")");
            emit("call void @zeus_value_clone(ptr " + copiedValue +
                 ", ptr " + boxedValue + ")");
            if (ownsBoxedValue) {
                emit("call void @zeus_value_release(ptr " + boxedValue + ")");
            }
            emit("call void @llvm.memmove.p0.p0.i64(ptr " + nativeAddress +
                 ", ptr " + copiedValue + ", i64 16, i1 false)");
            if (value.ownsClass && isManagedClassValue(value)) {
                emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
            }
            return true;
        }

        if (ZeusBuiltins::isNativePointerABI(abi)) {
            std::string sourceView;
            bool sourceConsumed = false;
            bool ownsConvertedNativeString = false;
            if (!convertNativeValue(value, targetType, "ptr", sourceView,
                                    sourceConsumed, ownsConvertedNativeString, error)) {
                return false;
            }
            if (nativeSlot) {
                if (abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                    emit("call void @zeus_string_store_slot(ptr " + nativeAddress +
                         ", ptr " + sourceView + ")");
                    if (ownsConvertedNativeString) {
                        emit("call void @zeus_string_release(ptr " + sourceView + ")");
                    }
                    if (value.ownsNativeString) {
                        emit("call void @zeus_string_release(ptr " + value.value + ")");
                    }
                } else {
                    emit("store ptr " + sourceView + ", ptr " + nativeAddress +
                         ", align 8");
                }
                if (value.dynamic && value.ownsDynamic && !sourceConsumed) {
                    emit("call void @zeus_value_release(ptr " + value.value + ")");
                }
                if (value.ownsClass && isManagedClassValue(value)) {
                    emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
                }
                return true;
            }
            std::string sourceData = sourceView;
            std::string lengthField;
            if (abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                lengthField = freshTemporary();
                emit(lengthField + " = getelementptr i8, ptr " + sourceView +
                     ", i64 -8");
            } else {
                const std::string dataField = freshTemporary();
                emit(dataField + " = getelementptr i8, ptr " + sourceView +
                     ", i64 0");
                sourceData = freshTemporary();
                emit(sourceData + " = load ptr, ptr " + dataField + ", align 8");
                lengthField = freshTemporary();
                emit(lengthField + " = getelementptr i8, ptr " + sourceView +
                     ", i64 8");
            }
            const std::string length = freshTemporary();
            emit(length + " = load i64, ptr " + lengthField + ", align 8");
            emit("call void @zeus_lvalue_require_raw(ptr " + nativeAddress +
                 ", i64 " + length + ", i32 1)");
            emit("call void @llvm.memmove.p0.p0.i64(ptr " + nativeAddress +
                 ", ptr " + sourceData + ", i64 " + length + ", i1 false)");
            if (ownsConvertedNativeString) {
                emit("call void @zeus_string_release(ptr " + sourceView + ")");
            }
            if (value.ownsNativeString) {
                emit("call void @zeus_string_release(ptr " + value.value + ")");
            }
            if (value.dynamic && value.ownsDynamic && !sourceConsumed) {
                emit("call void @zeus_value_release(ptr " + value.value + ")");
            }
            if (value.ownsClass && isManagedClassValue(value)) {
                emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
            }
            return true;
        }

        std::string storedValue;
        bool sourceConsumed = false;
        bool ownsConvertedNativeString = false;
        if (!convertNativeValue(
                value, targetType, nativeType, storedValue, sourceConsumed,
                ownsConvertedNativeString, error)) {
            return false;
        }

        if (abi != nullptr &&
            abi->valueABI == ZeusBuiltins::NativeValueABI::Int64 &&
            integerByteWidth > 0 && integerByteWidth < 8) {
            const std::string storeType = integerByteWidth == 1 ? "i8" :
                integerByteWidth == 2 ? "i16" : "i32";
            const std::string truncatedValue = freshTemporary();
            emit(truncatedValue + " = trunc i64 " + storedValue + " to " + storeType);
            emit("store " + storeType + " " + truncatedValue + ", ptr " +
                 nativeAddress + ", align 1");
        } else {
            if (integerByteWidth != 0 && integerByteWidth != 8) {
                error = "@faddr int width must be 1, 2, 4, or 8 bytes";
                return false;
            }
            emit("store " + nativeType + " " + storedValue + ", ptr " +
                 nativeAddress + ", align 1");
        }

        if (value.dynamic && value.ownsDynamic && !sourceConsumed) {
            emit("call void @zeus_value_release(ptr " + value.value + ")");
        }
        if (value.ownsClass && isManagedClassValue(value)) {
            emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
        }
        if (ownsConvertedNativeString) {
            emit("call void @zeus_string_release(ptr " + storedValue + ")");
        }
        return true;
    }

    template <typename EmitLine, typename FreshTemporary, typename ConvertArgument,
              typename ReleaseDynamic, typename Fail>
    bool emitKnownClassMethodCall(
        const IRType::IRInstr& ins,
        const std::string& methodName,
        const std::vector<NativeValue>& arguments,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        ReleaseDynamic&& releaseDynamic,
        Fail&& failAt,
        std::function<void(bool, const std::vector<NativeValue>&,
                           const std::vector<std::string>&)> exceptionCallScope = {},
        std::function<bool(size_t, const FunctionSignature&, std::string&)>
            emitReferenceArgument = {}
    ) {
        if (arguments.empty()) {
            return failAt(ins, "[emit-llvm] class method call is missing its receiver");
        }

        NativeValue receiver = arguments.front();
        if (receiver.sourceType.empty() && !receiver.dynamic) {
            if (receiver.type == "i64") receiver.sourceType = "int";
            else if (receiver.type == "i1") receiver.sourceType = "bool";
        }
        // Boxed native receivers retain their static source type in call
        // metadata even when the value-producing chunk used the dynamic ABI.
        if (receiver.dynamic && !ins.metadata.callArgumentTypes.empty()) {
            const std::string& inferredReceiverType =
                ins.metadata.callArgumentTypes.front();
            const auto* inferredABI = ZeusBuiltins::findNativeTypeABI(
                inferredReceiverType);
            std::string targetName;
            if (ins.metadata.readDirectCall(targetName)) {
                const auto target = functionSignatures.find(targetName);
                if (target != functionSignatures.end() &&
                    !target->second.isStaticMethod &&
                    target->second.ownerType == inferredReceiverType) {
                    if (isClassType(inferredReceiverType)) {
                        receiver.sourceType = inferredReceiverType;
                    } else if (inferredABI != nullptr &&
                               target->second.ownerType == inferredABI->canonicalName) {
                        receiver.sourceType = inferredABI->canonicalName;
                    }
                }
            }
        }
        const bool classReceiver = !receiver.sourceType.empty() &&
            isClassType(receiver.sourceType);
        const bool staticClassReceiver = classReceiver &&
            receiver.type == "classref" && !receiver.dynamic;
        const auto* nativeReceiverABI = receiver.sourceType.empty()
            ? nullptr
            : ZeusBuiltins::findNativeTypeABI(receiver.sourceType);
        const bool nativeReceiver = nativeReceiverABI != nullptr;
        const bool receiverTypeMatches = classReceiver
            ? (receiver.type == "ptr" || staticClassReceiver)
            : nativeReceiver && (receiver.dynamic
                ? receiver.type == "ptr"
                : receiver.type == llvmType(receiver.sourceType));
        if (receiver.functionRef || receiver.sourceType.empty() ||
            !receiverTypeMatches) {
            std::string receiverDirectTarget;
            ins.metadata.readDirectCall(receiverDirectTarget);
            std::string receiverArgumentType;
            if (!ins.metadata.callArgumentTypes.empty()) {
                receiverArgumentType = ins.metadata.callArgumentTypes.front();
            }
            return failAt(
                ins,
                "[emit-llvm] typed method call requires a matching class or native receiver ABI"
                " (receiver LLVM type=" + receiver.type +
                ", source type=" + receiver.sourceType +
                ", dynamic=" + (receiver.dynamic ? "true" : "false") +
                ", function reference=" + (receiver.functionRef ? "true" : "false") +
                ", call argument type=" + receiverArgumentType +
                ", direct target=" + receiverDirectTarget + ")"
            );
        }

        const std::string originalName = receiver.sourceType + "::" + methodName;
        std::string qualifiedName = originalName;
        std::string resolvedTarget;
        if (ins.metadata.readDirectCall(resolvedTarget)) {
            const auto specialized = functionSignatures.find(resolvedTarget);
            if (specialized != functionSignatures.end() &&
                specialized->second.genericOrigin == originalName) {
                qualifiedName = resolvedTarget;
            }
        }
        const auto function = functionSignatures.find(qualifiedName);
        const size_t suppliedCount = arguments.size() - (staticClassReceiver ? 1 : 0);
        if (function == functionSignatures.end() ||
            function->second.ownerType != receiver.sourceType ||
            suppliedCount < static_cast<size_t>(function->second.minRequiredParams) ||
            suppliedCount > function->second.parameterTypes.size() ||
            (staticClassReceiver && !function->second.isStaticMethod) ||
            (!staticClassReceiver && (function->second.isStaticMethod ||
                function->second.parameterTypes.empty() ||
                function->second.parameterTypes.front() != receiver.sourceType))) {
            return failAt(
                ins,
                "[emit-llvm] no matching typed method LLVM signature: " + qualifiedName
            );
        }

        const FunctionSignature& called = function->second;
        std::vector<NativeValue> suppliedArguments(arguments.begin() +
            (staticClassReceiver ? 1 : 0), arguments.end());
        std::vector<NativeValue> orderedArguments;
        std::vector<bool> providedArguments;
        std::vector<NativeValue> variadicArguments;
        std::vector<std::pair<std::string, NativeValue>> keywordArguments;
        std::string bindingError;
        if (!bindCallArguments(called, suppliedArguments, orderedArguments,
                providedArguments, variadicArguments, keywordArguments, bindingError)) {
            return failAt(ins, "[emit-llvm] " + bindingError);
        }
        const bool returnsDynamic = usesZeusValueABI(called.returnType);
        const std::string returnType = returnsDynamic
            ? "void"
            : llvmType(called.returnType);
        if (returnType.empty()) {
            return failAt(ins, "[emit-llvm] class method return type has no LLVM ABI mapping");
        }

        if (exceptionCallScope) exceptionCallScope(true, arguments, {});
        if (!staticClassReceiver) {
            emitRequireLiveIfNeeded(receiver, emit);
        }

        std::vector<std::string> nativeArguments;
        std::vector<std::string> cleanupValues;
        nativeArguments.reserve(called.parameterTypes.size());
        for (size_t i = 0; i < called.parameterTypes.size(); ++i) {
            const std::string& parameterType = called.parameterTypes[i];
            const bool byReference = i < called.parameterIsReference.size() &&
                called.parameterIsReference[i];
            if (functionParameterABIType(parameterType, byReference).empty()) {
                return failAt(ins, "[emit-llvm] class method parameter has no LLVM ABI mapping");
            }

            std::string nativeArgument;
            if (providedArguments[i]) {
                if (byReference) {
                    if (!emitReferenceArgument ||
                        !emitReferenceArgument(i, called, nativeArgument)) {
                        if (!emitReferenceArgument) {
                            return failAt(ins,
                                "[emit-llvm] reference method argument has no symbol lowering");
                        }
                        return false;
                    }
                } else {
                    std::string cleanupValue;
                    if (!convertArgument(
                            orderedArguments[i],
                            parameterType, nativeArgument, cleanupValue)) {
                        return false;
                    }
                    if (!cleanupValue.empty()) {
                        cleanupValues.push_back(std::move(cleanupValue));
                        if (exceptionCallScope) {
                            exceptionCallScope(true, arguments, cleanupValues);
                        }
                    }
                }
            } else {
                if (byReference) {
                    return failAt(ins,
                        "[emit-llvm] a reference parameter cannot be omitted");
                }
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        parameterType, placeholder, emit, freshTemporary)) {
                    return failAt(ins,
                        "[emit-llvm] class method has no ABI placeholder for an omitted argument");
                }
                nativeArgument = placeholder.value;
            }
            nativeArguments.push_back(std::move(nativeArgument));
        }

        const std::string callResult = freshTemporary();
        std::string callText;
        if (returnsDynamic) {
            emit(callResult + " = alloca %ZeusValue, align 8");
            callText = "call void " + llvmCallTarget(called.name) +
                       "(ptr " + callResult;
        } else {
            callText = callResult + " = call " + returnType + " " +
                       llvmCallTarget(called.name) + "(";
        }
        for (size_t i = 0; i < nativeArguments.size(); ++i) {
            if (i > 0 || returnsDynamic) callText += ", ";
            const bool byReference = i < called.parameterIsReference.size() &&
                called.parameterIsReference[i];
            callText += functionParameterABIType(called.parameterTypes[i], byReference) +
                        " " + nativeArguments[i];
        }
        if (called.hasDefaultParameters()) {
            if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
            callText += "i64 " + std::to_string(providedArgumentMask(providedArguments));
        }
        callText += ")";
        emit(callText);
        if (exceptionCallScope) exceptionCallScope(false, {}, {});

        std::set<std::string> releasedDynamicArguments;
        for (const auto& cleanupValue : cleanupValues) {
            if (releasedDynamicArguments.insert(cleanupValue).second) {
                if (detail::isNativeBytesCleanupToken(cleanupValue)) {
                    emit("call void @zeus_bytes_release(ptr " +
                         detail::nativeBytesCleanupValue(cleanupValue) + ")");
                } else if (detail::isNativeStringCleanupToken(cleanupValue)) {
                    emit("call void @zeus_string_release(ptr " +
                         detail::nativeStringCleanupValue(cleanupValue) + ")");
                } else {
                    releaseDynamic(cleanupValue);
                }
            }
        }
        for (const auto& argument : arguments) {
            if (argument.dynamic && argument.ownsDynamic &&
                releasedDynamicArguments.insert(argument.value).second) {
                releaseDynamic(argument.value);
            }
        }
        for (const auto& argument : arguments) {
            if (argument.ownsNativeBytes) {
                emit("call void @zeus_bytes_release(ptr " + argument.value + ")");
            }
            if (argument.ownsNativeString) {
                emit("call void @zeus_string_release(ptr " + argument.value + ")");
            }
        }
        for (const auto& argument : arguments) {
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        if (returnsDynamic) {
            resultValue = NativeValue{
                callResult, "ptr", false, true, called.returnType, true
            };
            return true;
        }

        const auto* returnABI = ZeusBuiltins::findNativeTypeABI(called.returnType);
        if (returnABI != nullptr && returnABI->valueABI ==
                ZeusBuiltins::NativeValueABI::NativeBytes) {
            resultValue = NativeValue{
                callResult, returnType, false, false, called.returnType, false
            };
            resultValue.ownsNativeBytes = true;
        } else if (ZeusBuiltins::isNativePointerABI(returnABI)) {
            const std::string stableView = freshTemporary();
            emit(stableView + " = call ptr @" +
                 std::string(ZeusBuiltins::nativePointerInternHelper(returnABI)) + "(ptr " +
                 callResult + ")");
            resultValue = NativeValue{
                stableView, returnType, false, false, called.returnType, false
            };
        } else {
            resultValue = NativeValue{
                callResult, returnType, false, false, called.returnType, false
            };
        }
        // &Class is a sharing rule for the signature. The returned pointer
        // still carries the concrete class layout and owns its retained value.
        const std::string referenceTarget = referencedTypeName(called.returnType);
        if (!referenceTarget.empty() && isClassType(referenceTarget)) {
            resultValue.sourceType = referenceTarget;
        }
        resultValue.ownsClass = !returnsDynamic && isClassType(resultValue.sourceType);
        return true;
    }

    template <typename EmitLine, typename FreshTemporary, typename ToDynamic,
              typename ReleaseDynamic, typename Fail>
    bool emitDynamicMemberCall(
        const IRType::IRInstr& ins,
        const std::string& methodName,
        const std::vector<NativeValue>& arguments,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ToDynamic&& toDynamic,
        ReleaseDynamic&& releaseDynamic,
        Fail&& failAt,
        std::function<void(bool, const std::vector<NativeValue>&,
                           const std::vector<std::string>&)> exceptionCallScope = {},
        const char* callSymbol = "zeus_value_dynamic_member_call"
    ) {
        if (arguments.empty()) {
            return failAt(ins, "dynamic member call is missing its receiver");
        }

        std::vector<std::string> ownedDynamicArguments;
        if (exceptionCallScope) {
            exceptionCallScope(true, arguments, ownedDynamicArguments);
        }
        std::vector<std::string> dynamicValues(arguments.size());
        std::vector<bool> ownsDynamicValues(arguments.size(), false);
        for (size_t i = 0; i < arguments.size(); ++i) {
            bool ownsDynamic = false;
            if (arguments[i].functionRef ||
                !toDynamic(arguments[i], dynamicValues[i], ownsDynamic)) {
                return failAt(ins, "dynamic member argument cannot be boxed as ZeusValue");
            }
            ownsDynamicValues[i] = ownsDynamic;
            if (ownsDynamic) {
                ownedDynamicArguments.push_back(dynamicValues[i]);
                if (exceptionCallScope) {
                    exceptionCallScope(true, arguments, ownedDynamicArguments);
                }
            }
        }

        std::string argumentArray = "null";
        if (arguments.size() > 1) {
            const size_t argumentCount = arguments.size() - 1;
            argumentArray = freshTemporary();
            emit(argumentArray + " = alloca ptr, i64 " +
                 std::to_string(argumentCount) + ", align 8");
            for (size_t i = 0; i < argumentCount; ++i) {
                const std::string slot = freshTemporary();
                emit(slot + " = getelementptr inbounds ptr, ptr " + argumentArray +
                     ", i64 " + std::to_string(i));
                emit("store ptr " + dynamicValues[i + 1] + ", ptr " + slot + ", align 8");
            }
        }

        const std::string methodSymbol = poolStr(methodName);
        const std::string methodPointer = freshTemporary();
        emit(methodPointer + " = getelementptr inbounds [" +
             std::to_string(methodName.size() + 1) + " x i8], ptr " + methodSymbol +
             ", i64 0, i64 0");

        const std::string dynamicResult = freshTemporary();
        emit(dynamicResult + " = alloca %ZeusValue, align 8");
        emit("call void @" + std::string(callSymbol) + "(ptr " + dynamicResult +
             ", ptr " + dynamicValues[0] + ", ptr " + methodPointer + ", i32 " +
             std::to_string(arguments.size() - 1) + ", ptr " + argumentArray + ")");
        if (exceptionCallScope) exceptionCallScope(false, {}, {});

        for (size_t i = 0; i < dynamicValues.size(); ++i) {
            if (ownsDynamicValues[i]) releaseDynamic(dynamicValues[i]);
        }
        for (const auto& argument : arguments) {
            if (argument.ownsNativeBytes) {
                emit("call void @zeus_bytes_release(ptr " + argument.value + ")");
            }
            if (argument.ownsNativeString) {
                emit("call void @zeus_string_release(ptr " + argument.value + ")");
            }
        }
        for (const auto& argument : arguments) {
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        resultValue = NativeValue{
            dynamicResult, "ptr", false, true, std::string(), true
        };
        return true;
    }

