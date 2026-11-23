// Emitter member definitions: emitNativeFunction; included inside class Emitter.
    bool emitNativeFunction(
        const std::vector<IRType::IRInstr>& chunk,
        size_t ci
    ) {
        auto signatureIt = functionSignaturesByChunk.find(ci);
        if (signatureIt == functionSignaturesByChunk.end()) {
            result.error = "[emit-llvm] 函数 chunk 缺少签名标注";
            return false;
        }

        const FunctionSignature& signature = signatureIt->second;
        const bool emitFunctionSafepoints = !signature.isNoSafepoint;
        LivenessScope livenessScope(
            verifiedLiveParameters, blockVerifiedLiveValues);

        std::vector<int64_t> exceptionTarget(chunk.size(), -1);
        // Number of try regions whose TRY_END has not executed yet when
        // instruction pc starts running. TRY_END itself counts its own region
        // (the exit call runs as part of that instruction); TRY_BEGIN counts
        // zero (the push happens when the instruction runs).
        std::vector<int> tryLevel(chunk.size(), 0);
        std::vector<size_t> exceptionStack;
        std::vector<size_t> finExitHandlerStack;
        bool finExitHandlerStackActive = false;
        std::vector<size_t> catchBodyStack;
        std::map<size_t, size_t> catchBeginForEnd;
        std::map<size_t, size_t> catchEndForBegin;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& instruction = chunk[pc];
            if (instruction.op == VMOperator::TRY_BEGIN) {
                int64_t offset = 0;
                if (!tryArgInt(instruction, 0, offset) || offset <= 0 ||
                    static_cast<uint64_t>(offset) >= chunk.size() - pc ||
                    chunk[pc + static_cast<size_t>(offset)].op != VMOperator::CATCH_BEGIN) {
                    result.error = "[emit-llvm] invalid TRY_BEGIN catch target";
                    result.errorLine = instruction.line;
                    return false;
                }
                exceptionStack.push_back(pc + static_cast<size_t>(offset));
            } else if (instruction.op == VMOperator::TRY_END) {
                if (instruction.metadata.tag == "fin_exit_handler" &&
                    !finExitHandlerStackActive) {
                    finExitHandlerStack = exceptionStack;
                    finExitHandlerStackActive = true;
                }
                if (exceptionStack.empty()) {
                    result.error = "[emit-llvm] TRY_END without TRY_BEGIN";
                    result.errorLine = instruction.line;
                    return false;
                }
                tryLevel[pc] = static_cast<int>(exceptionStack.size());
                exceptionStack.pop_back();
            } else if (!exceptionStack.empty()) {
                exceptionTarget[pc] = static_cast<int64_t>(exceptionStack.back());
                tryLevel[pc] = static_cast<int>(exceptionStack.size());
            }
            if (instruction.op == VMOperator::CATCH_BEGIN) {
                catchBodyStack.push_back(pc);
            } else if (instruction.op == VMOperator::CATCH_END) {
                if (catchBodyStack.empty()) {
                    result.error = "[emit-llvm] CATCH_END without CATCH_BEGIN";
                    return false;
                }
                catchBeginForEnd[pc] = catchBodyStack.back();
                catchEndForBegin[catchBodyStack.back()] = pc;
                catchBodyStack.pop_back();
            }
            if (instruction.metadata.tag == "fin_exit_terminator" &&
                finExitHandlerStackActive) {
                exceptionStack = std::move(finExitHandlerStack);
                finExitHandlerStack.clear();
                finExitHandlerStackActive = false;
            }
        }
        if (!exceptionStack.empty() || !catchBodyStack.empty() ||
            finExitHandlerStackActive) {
            result.error = "[emit-llvm] unterminated TRY_BEGIN";
            return false;
        }
        const bool hasNativeTry = std::find_if(chunk.begin(), chunk.end(),
            [](const IRType::IRInstr& instruction) {
                return instruction.op == VMOperator::TRY_BEGIN;
            }) != chunk.end();
        needsRuntimeErrorFactory = needsRuntimeErrorFactory || hasNativeTry;

        std::set<int> referenceCapturedLocals;
        if (programChunks) {
            for (const auto& instruction : chunk) {
                if (instruction.op != VMOperator::MAKE_FUNCTION ||
                    instruction.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(instruction.args[0])) {
                    continue;
                }
                const int childChunk = VMHelpers::getIntFromValue(
                    std::get<LexerType::Value>(instruction.args[0]));
                if (childChunk < 0) continue;
                for (const auto& capture : captureDescriptors(static_cast<size_t>(childChunk))) {
                    if (capture.sourceKind == "local" && capture.byReference &&
                        capture.sourceIndex >= 0) {
                        referenceCapturedLocals.insert(capture.sourceIndex);
                    }
                }
            }
        }

        struct StaticReferenceValue {
            enum class Kind { Unknown, Function, Class, Namespace } kind = Kind::Unknown;
            std::string name;
        };
        auto staticFunctionReference = [&](const FunctionSignature* function) {
            if (function == nullptr || function->isExternal ||
                function->captureCount != 0 ||
                !hasCompleteFunctionAnnotations(*function)) {
                return StaticReferenceValue{};
            }
            return StaticReferenceValue{
                StaticReferenceValue::Kind::Function, function->name
            };
        };
        auto resolveStaticIdentifier = [&](const std::string& name) {
            if (const auto* function = resolveFunctionSignature(name)) {
                const auto reference = staticFunctionReference(function);
                if (reference.kind != StaticReferenceValue::Kind::Unknown) {
                    return reference;
                }
            }
            if (classLayouts.find(name) != classLayouts.end()) {
                return StaticReferenceValue{
                    StaticReferenceValue::Kind::Class, name
                };
            }
            return StaticReferenceValue{};
        };
        auto resolveStaticGlobal = [&](int64_t index) {
            if (index < 0 || index > std::numeric_limits<int>::max()) {
                return StaticReferenceValue{};
            }
            const int globalIndex = static_cast<int>(index);
            const auto namespaceValue = namespacePackagesByGlobal.find(globalIndex);
            if (namespaceValue != namespacePackagesByGlobal.end()) {
                return StaticReferenceValue{
                    StaticReferenceValue::Kind::Namespace,
                    namespaceValue->second
                };
            }
            const auto classValue = classGlobals.find(globalIndex);
            if (classValue != classGlobals.end() &&
                classLayouts.find(classValue->second) != classLayouts.end()) {
                return StaticReferenceValue{
                    StaticReferenceValue::Kind::Class, classValue->second
                };
            }
            return StaticReferenceValue{};
        };
        auto resolveStaticAttribute = [&](const StaticReferenceValue& receiver,
                                          const std::string& member) {
            if (receiver.kind == StaticReferenceValue::Kind::Class) {
                return staticFunctionReference(
                    findStaticClassFunction(receiver.name, member));
            }
            if (receiver.kind == StaticReferenceValue::Kind::Namespace) {
                const auto exports = namespaceExportsByPackage.find(receiver.name);
                if (exports != namespaceExportsByPackage.end() &&
                    exports->second.find(member) != exports->second.end() &&
                    classLayouts.find(member) != classLayouts.end()) {
                    return StaticReferenceValue{
                        StaticReferenceValue::Kind::Class, member
                    };
                }
                return staticFunctionReference(
                    findNamespaceFunction(receiver.name, member));
            }
            return StaticReferenceValue{};
        };

        // Track captureless function values through straight-line local stores.
        // This lets the addressability pass use their real parameter signature
        // instead of treating every indirect-looking call as dynamically typed.
        std::map<size_t, std::string> inferredStaticCallTargets;
        std::vector<StaticReferenceValue> staticValueStack;
        std::map<int, StaticReferenceValue> staticLocalValues;
        auto popStaticValue = [&]() {
            if (staticValueStack.empty()) return StaticReferenceValue{};
            StaticReferenceValue value = std::move(staticValueStack.back());
            staticValueStack.pop_back();
            return value;
        };
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& instruction = chunk[pc];
            switch (instruction.op) {
                case VMOperator::FUNC_BEGIN:
                case VMOperator::IR_ANNOTATION:
                case VMOperator::HOTSPOT:
                case VMOperator::PUSH_NAMED:
                    break;

                case VMOperator::LOAD_VALUE: {
                    StaticReferenceValue value;
                    if (instruction.args.size() == 1 &&
                        std::holds_alternative<std::string>(instruction.args[0])) {
                        value = resolveStaticIdentifier(
                            std::get<std::string>(instruction.args[0]));
                    } else if (instruction.args.size() >= 2 &&
                               std::holds_alternative<std::string>(instruction.args[0])) {
                        int64_t index = -1;
                        const std::string& kind =
                            std::get<std::string>(instruction.args[0]);
                        if (tryArgInt(instruction, 1, index)) {
                            if (kind == "local" && index >= 0 &&
                                index <= std::numeric_limits<int>::max()) {
                                const auto local = staticLocalValues.find(
                                    static_cast<int>(index));
                                if (local != staticLocalValues.end()) {
                                    value = local->second;
                                }
                            } else if (kind == "global") {
                                value = resolveStaticGlobal(index);
                            }
                        }
                    }
                    staticValueStack.push_back(std::move(value));
                    break;
                }

                case VMOperator::OBJ_LOAD_ATTR:
                case VMOperator::OBJ_LOAD_SAFE_ATTR: {
                    const StaticReferenceValue receiver = popStaticValue();
                    if (!instruction.args.empty() &&
                        std::holds_alternative<std::string>(instruction.args[0])) {
                        staticValueStack.push_back(resolveStaticAttribute(
                            receiver, std::get<std::string>(instruction.args[0])));
                    } else {
                        staticValueStack.emplace_back();
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t index = -1;
                    StaticReferenceValue value = popStaticValue();
                    if (tryArgInt(instruction, 0, index) && index >= 0 &&
                        index <= std::numeric_limits<int>::max()) {
                        staticLocalValues[static_cast<int>(index)] =
                            std::move(value);
                    } else {
                        staticLocalValues.clear();
                    }
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    int64_t argc = -1;
                    if (!tryArgInt(instruction, 0, argc) || argc < 0 ||
                        static_cast<uint64_t>(argc) >= staticValueStack.size()) {
                        staticValueStack.clear();
                        staticLocalValues.clear();
                        break;
                    }
                    const size_t calleeIndex = staticValueStack.size() -
                        static_cast<size_t>(argc) - 1;
                    const StaticReferenceValue& callee =
                        staticValueStack[calleeIndex];
                    if (callee.kind == StaticReferenceValue::Kind::Function) {
                        inferredStaticCallTargets[pc] = callee.name;
                    }
                    staticValueStack.resize(calleeIndex);
                    staticValueStack.emplace_back();
                    // Calls may mutate locals passed by reference or through an
                    // alias, so require a fresh straight-line store to infer
                    // another indirect call target.
                    staticLocalValues.clear();
                    break;
                }

                case VMOperator::POP:
                    (void)popStaticValue();
                    break;

                case VMOperator::CLEAR:
                    staticValueStack.clear();
                    break;

                case VMOperator::SWAP:
                    if (staticValueStack.size() >= 2) {
                        std::iter_swap(staticValueStack.end() - 1,
                                       staticValueStack.end() - 2);
                    } else {
                        staticValueStack.clear();
                    }
                    break;

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
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR:
                    (void)popStaticValue();
                    (void)popStaticValue();
                    staticValueStack.emplace_back();
                    break;

                case VMOperator::LOGICAL_NOT:
                    (void)popStaticValue();
                    staticValueStack.emplace_back();
                    break;

                case VMOperator::JUMP:
                case VMOperator::JUMP_IF_TRUE:
                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_PARAM_PROVIDED:
                case VMOperator::LOOP_BREAK:
                case VMOperator::TRY_BEGIN:
                case VMOperator::TRY_END:
                case VMOperator::CATCH_BEGIN:
                case VMOperator::CATCH_END:
                case VMOperator::RETURN:
                case VMOperator::FUNC_END:
                    pc = chunk.size();
                    break;

                default:
                    staticValueStack.clear();
                    staticLocalValues.clear();
                    break;
            }
        }

        std::set<int> addressableLocalIndices;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& instruction = chunk[pc];
            if (instruction.op != VMOperator::CALL_CLOSURE &&
                instruction.op != VMOperator::OBJ_CALL_METHOD &&
                instruction.op != VMOperator::CLASS_STATIC_METHOD_CALL) continue;

            int receiverOffset = 0;
            if (instruction.op == VMOperator::OBJ_CALL_METHOD) {
                std::string targetName;
                if (instruction.metadata.readDirectCall(targetName)) {
                    const auto target = functionSignatures.find(targetName);
                    if (target != functionSignatures.end() &&
                        !target->second.isStaticMethod) receiverOffset = 1;
                }
            }
            const bool hasStaticReferenceSignature =
                instruction.metadata.kind == IRType::IRMetadataKind::CallTarget;
            if (!hasStaticReferenceSignature) {
                const auto inferredTargetName = inferredStaticCallTargets.find(pc);
                if (inferredTargetName != inferredStaticCallTargets.end()) {
                    const auto inferredTarget = functionSignatures.find(
                        inferredTargetName->second);
                    if (inferredTarget != functionSignatures.end()) {
                        const FunctionSignature& target = inferredTarget->second;
                        for (const auto& argument : instruction.metadata.callArguments) {
                            if (!argument.isAssignable || argument.source.isConst ||
                                argument.source.kind != IRType::SymbolRefKind::Local ||
                                argument.source.index < 0) {
                                continue;
                            }

                            size_t parameterIndex = target.parameterTypes.size();
                            bool parameterKnown = false;
                            if (!argument.namedParameter.empty()) {
                                const auto parameter = std::find(
                                    target.parameterNames.begin(),
                                    target.parameterNames.end(),
                                    argument.namedParameter);
                                if (parameter != target.parameterNames.end()) {
                                    parameterIndex = static_cast<size_t>(
                                        std::distance(target.parameterNames.begin(), parameter));
                                    parameterKnown = true;
                                }
                            } else if (argument.positionalIndex >= 0) {
                                parameterIndex = static_cast<size_t>(
                                    argument.positionalIndex);
                                parameterKnown = true;
                            }

                            bool mayPassByReference = !parameterKnown;
                            if (parameterKnown &&
                                parameterIndex < target.parameterTypes.size()) {
                                mayPassByReference =
                                    parameterIndex >= target.parameterIsReference.size() ||
                                    target.parameterIsReference[parameterIndex];
                            }
                            if (mayPassByReference) {
                                addressableLocalIndices.insert(argument.source.index);
                            }
                        }
                        continue;
                    }
                }

                // An indirect closure has no statically-known signature here.
                // Keep each mutable local argument addressable so the runtime
                // adapter can decide whether it needs the value or its slot.
                for (const auto& argument : instruction.metadata.callArguments) {
                    if (!argument.isAssignable || argument.source.isConst ||
                        argument.source.kind != IRType::SymbolRefKind::Local ||
                        argument.source.index < 0) {
                        continue;
                    }
                    const size_t sourceIndex =
                        static_cast<size_t>(argument.source.index);
                    if (sourceIndex < signature.parameterIsReference.size() &&
                        signature.parameterIsReference[sourceIndex]) {
                        continue;
                    }
                    addressableLocalIndices.insert(argument.source.index);
                }
                continue;
            }

            for (size_t parameterIndex = 0;
                 parameterIndex < instruction.metadata.parameterIsReference.size();
                 ++parameterIndex) {
                if (!instruction.metadata.parameterIsReference[parameterIndex]) continue;
                const std::string parameterName = parameterIndex <
                        instruction.metadata.parameterNames.size()
                    ? instruction.metadata.parameterNames[parameterIndex]
                    : std::string();
                for (const auto& argument : instruction.metadata.callArguments) {
                    const bool matches = argument.namedParameter.empty()
                        ? argument.positionalIndex == static_cast<int>(parameterIndex) - receiverOffset
                        : argument.namedParameter == parameterName;
                    if (!matches || !argument.isAssignable ||
                        argument.source.kind != IRType::SymbolRefKind::Local ||
                        argument.source.index < 0) {
                        continue;
                    }
                    const size_t sourceIndex =
                        static_cast<size_t>(argument.source.index);
                    if (sourceIndex < signature.parameterIsReference.size() &&
                        signature.parameterIsReference[sourceIndex]) {
                        continue;
                    }
                    addressableLocalIndices.insert(argument.source.index);
                }
            }
        }

        // Typechecked native locals can keep their LLVM ABI representation for
        // the whole function. Dynamic locals stay in ZeusValue slots so later
        // assignments may change their runtime type.
        std::map<int, std::string> nativeScalarLocalTypes;
        for (const auto& instruction : chunk) {
            if (instruction.op != VMOperator::STORE_VALUE_LOCAL ||
                instruction.metadata.kind !=
                    IRType::IRMetadataKind::LocalBindingType) {
                continue;
            }
            int64_t localIndex = -1;
            if (!tryArgInt(instruction, 0, localIndex) || localIndex < 0 ||
                localIndex > std::numeric_limits<int>::max()) {
                continue;
            }
            const auto* abi = ZeusBuiltins::findNativeTypeABI(
                instruction.metadata.typeName);
            if (abi == nullptr ||
                (abi->valueABI != ZeusBuiltins::NativeValueABI::Int64 &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::SignedInteger &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::UnsignedInteger &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::FloatingPoint &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::Bool1 &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::NativeString &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::NativeBytes &&
                 abi->valueABI != ZeusBuiltins::NativeValueABI::RawBytesRange)) {
                continue;
            }
            nativeScalarLocalTypes[static_cast<int>(localIndex)] =
                std::string(abi->canonicalName);
        }

        // Locals named by @addr need storage that remains stable for the
        // lifetime of the function. Dynamic values use the existing
        // ZeusValue local slots. Native ABI parameters keep their declared
        // representation in typed slots so string methods can still read
        // NativeString fields without boxing.
        std::set<int> addressableNativeParameterIndices;
        std::set<int> addressedLocalIndices;
        for (const auto& instruction : chunk) {
            if (instruction.op != VMOperator::LLVM_ADDR ||
                instruction.args.size() < 2 ||
                !std::holds_alternative<std::string>(instruction.args[0]) ||
                std::get<std::string>(instruction.args[0]) != "local") {
                continue;
            }
            int64_t index = -1;
            if (!tryArgInt(instruction, 1, index) || index < 0 ||
                index > std::numeric_limits<int>::max()) {
                continue;
            }
            const int localIndex = static_cast<int>(index);
            addressedLocalIndices.insert(localIndex);
            if (referenceCapturedLocals.find(localIndex) !=
                    referenceCapturedLocals.end() ||
                addressableLocalIndices.find(localIndex) !=
                    addressableLocalIndices.end()) {
                continue;
            }
            if (static_cast<size_t>(localIndex) < signature.parameterTypes.size()) {
                const bool byReference = static_cast<size_t>(localIndex) <
                        signature.parameterIsReference.size() &&
                    signature.parameterIsReference[static_cast<size_t>(localIndex)];
                if (byReference) continue;
                const std::string& parameterType =
                    signature.parameterTypes[static_cast<size_t>(localIndex)];
                const bool nativeParameter = !usesZeusValueABI(parameterType) &&
                    !isReferenceTypeName(parameterType) &&
                    !isClassType(parameterType) &&
                    !llvmType(parameterType).empty();
                if (nativeParameter) {
                    addressableNativeParameterIndices.insert(localIndex);
                    continue;
                }
            }
            if (nativeScalarLocalTypes.find(localIndex) !=
                    nativeScalarLocalTypes.end() &&
                addressableLocalIndices.find(localIndex) ==
                    addressableLocalIndices.end()) {
                continue;
            }
            addressableLocalIndices.insert(localIndex);
        }

        // `@native func` 定义一个 C 链接函数：没有 ZeusValue 出参。未标注返回
        // 类型时 IR 里落成 "nil"，对 C 边界来说就是 void。
        const bool definesVoidReturn = !signature.isExternal &&
            !signature.nativeSymbol.empty() &&
            (signature.returnType.empty() || signature.returnType == "nil" ||
             signature.returnType == "void");
        const bool returnsDynamic =
            !definesVoidReturn && usesZeusValueABI(signature.returnType);
        const std::string returnType = (returnsDynamic || definesVoidReturn)
            ? "void"
            : llvmType(signature.returnType);
        if (returnType.empty()) {
            result.error = "[emit-llvm] 函数 '" + signature.name +
                           "' 的返回类型没有 LLVM ABI 映射";
            return false;
        }

        std::vector<std::string> parameterLLVMTypes;
        for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
            const bool byReference = i < signature.parameterIsReference.size() &&
                signature.parameterIsReference[i];
            const std::string type = functionParameterABIType(
                signature.parameterTypes[i], byReference);
            if (type.empty()) {
                result.error = "[emit-llvm] 函数 '" + signature.name +
                               "' 的参数类型没有 LLVM ABI 映射";
                return false;
            }
            parameterLLVMTypes.push_back(type);
        }

        std::string functionText;
        functionText += "define " + returnType + " " + llvmCallTarget(signature) + "(";
        bool hasFunctionParameter = false;
        if (signature.captureCount != 0) {
            functionText += "ptr %closure.env";
            hasFunctionParameter = true;
        }
        if (returnsDynamic) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "ptr %return.out";
            hasFunctionParameter = true;
        }
        for (size_t i = 0; i < parameterLLVMTypes.size(); ++i) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += parameterLLVMTypes[i] + " %arg" + std::to_string(i);
            hasFunctionParameter = true;
        }
        if (signature.hasVariadicArgs) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "ptr %varargs";
            hasFunctionParameter = true;
        }
        if (signature.hasVariadicKwargs) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "ptr %kwargs";
            hasFunctionParameter = true;
        }
        if (signature.hasDefaultParameters()) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "i64 %provided.mask";
        }
        functionText += ")";
        if (signature.isInline) functionText += " alwaysinline";
        functionText += isMCUMode() ? " {\nentry:\n" : " personality ptr @__CxxFrameHandler3 {\nentry:\n";
        const size_t nativeEntryBody = functionText.size();

        int nativeTmpCount = 0;
        auto freshNativeTmp = [&]() {
            return std::string("%n") + std::to_string(nativeTmpCount++);
        };
        std::map<int, std::string> addressableLocalSlots;
        std::map<int, NativeSlot> addressableNativeLocalSlots;
        std::string addressableLocalSlotPrologue;
        for (const int localIndex : addressableLocalIndices) {
            const std::string slot = freshNativeTmp();
            addressableLocalSlots.emplace(localIndex, slot);
            addressableLocalSlotPrologue += "  " + slot +
                " = alloca %ZeusValue, align 8\n";
            addressableLocalSlotPrologue +=
                "  call void @zeus_value_init_nil(ptr " + slot + ")\n";
        }
        for (const int localIndex : addressableNativeParameterIndices) {
            const std::string& parameterType =
                signature.parameterTypes[static_cast<size_t>(localIndex)];
            NativeSlot slot;
            slot.address = freshNativeTmp();
            slot.type = llvmType(parameterType);
            slot.sourceType = parameterType;
            slot.addressStorageABI = "native";
            const bool borrowedBytesReceiver = localIndex == 0 &&
                parameterType == "bytes" && signature.ownerType == "bytes" &&
                !signature.isStaticMethod &&
                detail::isReadOnlyBytesMethodName(methodNameForSignature(signature));
            slot.ownsNativeBytes = parameterType == "bytes" &&
                !borrowedBytesReceiver;
            slot.ownsNativeString = parameterType == "string";
            slot.needsLValueUnregister = true;
            addressableNativeLocalSlots.emplace(localIndex, slot);
            addressableLocalSlotPrologue += "  " + slot.address + " = alloca " +
                slot.type + ", align " +
                (slot.type == "i1" ? "1" : "8") + "\n";
            if (parameterType == "bytes") {
                addressableLocalSlotPrologue += "  store ptr null, ptr " +
                    slot.address + "\n";
            } else if (parameterType == "string") {
                addressableLocalSlotPrologue += "  store ptr null, ptr " +
                    slot.address + "\n";
            } else if (parameterType == "&bytes") {
                addressableLocalSlotPrologue +=
                    "  store %ZeusRawBytesRange zeroinitializer, ptr " +
                    slot.address + ", align 8\n";
            }
        }
        for (const auto& localType : nativeScalarLocalTypes) {
            const int localIndex = localType.first;
            if (static_cast<size_t>(localIndex) < signature.parameterTypes.size() ||
                referenceCapturedLocals.find(localIndex) !=
                    referenceCapturedLocals.end() ||
                addressableLocalIndices.find(localIndex) !=
                    addressableLocalIndices.end() ||
                addressableNativeLocalSlots.find(localIndex) !=
                    addressableNativeLocalSlots.end()) {
                continue;
            }
            NativeSlot slot;
            slot.address = freshNativeTmp();
            slot.type = llvmType(localType.second);
            slot.sourceType = localType.second;
            slot.addressStorageABI = "native";
            slot.needsLValueUnregister =
                addressedLocalIndices.find(localIndex) != addressedLocalIndices.end();
            addressableNativeLocalSlots.emplace(localIndex, slot);
            addressableLocalSlotPrologue += "  " + slot.address + " = alloca " +
                slot.type + ", align " +
                (slot.type == "i1" ? "1" : "8") + "\n";
            if (localType.second == "bytes") {
                addressableLocalSlotPrologue += "  store ptr null, ptr " +
                    slot.address + "\n";
            } else if (localType.second == "string") {
                addressableLocalSlotPrologue += "  store ptr null, ptr " +
                    slot.address + "\n";
            } else if (localType.second == "&bytes") {
                addressableLocalSlotPrologue +=
                    "  store %ZeusRawBytesRange zeroinitializer, ptr " +
                    slot.address + ", align 8\n";
            }
        }
        if (!addressableLocalSlotPrologue.empty()) {
            functionText.insert(nativeEntryBody, addressableLocalSlotPrologue);
        }
        struct NativeExceptionPad {
            std::string switchLabel;
            std::string padLabel;
            std::string resumeLabel;
            std::string valueSlot;
            size_t catchPc = 0;
            bool cleanupOnly = false;
            std::vector<std::string> cleanupCalls;
        };
        std::vector<NativeExceptionPad> exceptionPads;
        size_t exceptionEdgeCount = 0;
        std::map<size_t, std::string> exceptionValueSlots;
        int64_t activeExceptionTarget = -1;
        size_t activeExceptionPc = 0;
        bool suppressExceptionWrapping = false;
        std::string currentNativeBlock = "%entry";
        std::function<void(size_t, const std::string&)> recordExceptionEdge;
        std::function<void(const std::string&)> enterExceptionContinuation;
        std::function<std::vector<std::string>(bool)> exceptionCleanupSnapshot;
        std::vector<std::string>* exceptionEdgeNormalPathReleases = nullptr;
        auto isNonThrowingRuntimeCall = [](const std::string& line) -> bool {
            static constexpr const char* kNoUnwindSymbols[] = {
                "@zeus_class_ref_retain",
                "@zeus_class_ref_release",
                "@zeus_value_release",
                "@zeus_capture_cell_release",
                "@zeus_string_retain",
                "@zeus_string_release",
                "@zeus_bytes_release",
                "@zeus_value_init_nil",
                "@zeus_value_init_i64",
                "@zeus_value_init_bool",
                "@zeus_string_literal_from_data",
                "@zeus_gc_safepoint",
            };
            for (const char* symbol : kNoUnwindSymbols) {
                if (line.find(symbol) != std::string::npos) return true;
            }
            return false;
        };
        auto preservesClassLiveness = [](const std::string& line) -> bool {
            static constexpr const char* kLivenessSafeSymbols[] = {
                "@zeus_class_ref_require_live",
                "@zeus_bytes_get_fast",
                "@zeus_bytes_len_fast",
                "@zeus_string_literal_from_data",
                "@zeus_string_retain",
                "@zeus_value_init_nil",
                "@zeus_value_init_i64",
                "@zeus_value_init_bool",
            };
            for (const char* symbol : kLivenessSafeSymbols) {
                if (line.find(symbol) != std::string::npos) return true;
            }
            return false;
        };
        auto emitNativeLine = [&](const std::string& line) {
            const size_t callAt = line.rfind("call ", 0) == 0 ? 0 : line.find(" = call ");
            if (!elideParameterLivenessChecks && callAt != std::string::npos &&
                !preservesClassLiveness(line)) {
                blockVerifiedLiveValues.clear();
            }
            if (suppressExceptionWrapping || isMCUMode() || callAt == std::string::npos ||
                line.find("@llvm.", callAt) != std::string::npos ||
                isNonThrowingRuntimeCall(line)) {
                functionText += "  " + line + "\n";
                return;
            }
            const bool hasLocalCatch = activeExceptionTarget >= 0 &&
                                       recordExceptionEdge;
            std::vector<std::string> cleanupCalls;
            if (exceptionCleanupSnapshot) {
                cleanupCalls = exceptionCleanupSnapshot(hasLocalCatch);
            }
            if (!hasLocalCatch && cleanupCalls.empty()) {
                functionText += "  " + line + "\n";
                return;
            }

            const std::string continuation = "%eh_continue_" +
                std::to_string(exceptionEdgeCount++);
            std::string unwindLabel;
            std::vector<std::string> normalPathExceptionEdgeReleases;
            if (!hasLocalCatch) {
                const auto sharedCleanup = std::find_if(
                    exceptionPads.begin(), exceptionPads.end(),
                    [&](const NativeExceptionPad& existing) {
                        return existing.cleanupOnly &&
                            existing.cleanupCalls == cleanupCalls;
                    });
                if (sharedCleanup != exceptionPads.end()) {
                    unwindLabel = sharedCleanup->switchLabel;
                }
            }
            if (unwindLabel.empty()) {
                const std::string stem = std::to_string(exceptionPads.size());
                NativeExceptionPad pad;
                pad.switchLabel = "%eh_switch_" + stem;
                pad.padLabel = "%eh_pad_" + stem;
                pad.resumeLabel = "%eh_resume_" + stem;
                pad.cleanupOnly = !hasLocalCatch;
                pad.cleanupCalls = std::move(cleanupCalls);
                if (hasLocalCatch) {
                    // An exception thrown inside the try body leaves the region
                    // without executing TRY_END; decrement the dynamic try depth
                    // before the catch body starts running.
                    pad.cleanupCalls.insert(pad.cleanupCalls.begin(),
                        "call void @zeus_try_exit()");
                }
                if (hasLocalCatch) {
                    const int savedTarget = static_cast<int>(activeExceptionTarget);
                    activeExceptionTarget = -1;
                    pad.catchPc = static_cast<size_t>(savedTarget);
                    auto slot = exceptionValueSlots.find(pad.catchPc);
                    if (slot == exceptionValueSlots.end()) {
                        pad.valueSlot = freshNativeTmp();
                        functionText += "  " + pad.valueSlot + " = alloca %ZeusValue, align 8\n";
                        slot = exceptionValueSlots.emplace(pad.catchPc, pad.valueSlot).first;
                    } else {
                        pad.valueSlot = slot->second;
                    }
                    const bool previousSuppression = suppressExceptionWrapping;
                    auto* const previousExceptionEdgeReleases =
                        exceptionEdgeNormalPathReleases;
                    exceptionEdgeNormalPathReleases =
                        &normalPathExceptionEdgeReleases;
                    suppressExceptionWrapping = true;
                    recordExceptionEdge(pad.catchPc, pad.resumeLabel);
                    suppressExceptionWrapping = previousSuppression;
                    exceptionEdgeNormalPathReleases =
                        previousExceptionEdgeReleases;
                    activeExceptionTarget = savedTarget;
                }
                exceptionPads.push_back(std::move(pad));
                unwindLabel = exceptionPads.back().switchLabel;
            }
            std::string invoke = line;
            invoke.replace(callAt == 0 ? 0 : callAt + 3, 4, "invoke");
            functionText += "  " + invoke + " to label " + continuation +
                " unwind label " + unwindLabel + "\n";
            functionText += continuation.substr(1) + ":\n";
            if (enterExceptionContinuation) {
                enterExceptionContinuation(continuation);
            } else {
                currentNativeBlock = continuation;
            }
            for (const auto& value : normalPathExceptionEdgeReleases) {
                functionText += "  call void @zeus_value_release(ptr " + value + ")\n";
            }
        };
        // Emits the given number of dynamic try-depth exits. Raw address
        // checks are gated on the thread-local depth, so control-flow edges
        // that leave a try region without executing TRY_END (and every
        // implicit return) must decrement it. These lines are appended
        // directly: they cannot throw and must never be wrapped in an invoke.
        auto emitTryDepthExits = [&](int count) {
            for (int index = 0; index < count; ++index) {
                functionText += "  call void @zeus_try_exit()\n";
            }
        };
        auto tryLevelBefore = [&](size_t pc) -> int {
            return pc < tryLevel.size() ? tryLevel[pc] : 0;
        };
        auto releaseNativeValueWithContext = [&](const NativeValue& value) {
            if (value.dynamic && value.ownsDynamic && !value.value.empty()) {
                emitNativeLine("call void @zeus_value_release(ptr " +
                               value.value + ")");
            } else if (value.ownsClass && !value.value.empty()) {
                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                               value.value + ")");
            } else if (value.ownsNativeBytes && !value.value.empty()) {
                emitNativeLine("call void @zeus_bytes_release(ptr " +
                               value.value + ")");
            } else if (value.ownsNativeString && !value.value.empty()) {
                emitNativeLine("call void @zeus_string_release(ptr " +
                               value.value + ")");
            }
        };

        std::vector<NativeValue> valueStack;
        std::vector<NativeValue> withValues;
        std::vector<NativeValue> exceptionCallArguments;
        std::vector<std::string> exceptionCallDynamicValues;
        std::map<int, NativeValue> locals;
        std::map<std::string, std::string> nativeGlobalValueOrigins;
        std::map<int, std::string> closureLocalCells;
        std::set<int> referenceParameterLocals;
        auto exceptionCallScope = [&](bool begin,
                                      const std::vector<NativeValue>& arguments,
                                      const std::vector<std::string>& dynamicValues) {
            if (begin) {
                exceptionCallArguments = arguments;
                exceptionCallDynamicValues = dynamicValues;
            } else {
                exceptionCallArguments.clear();
                exceptionCallDynamicValues.clear();
            }
        };
        exceptionCleanupSnapshot = [&](bool hasLocalCatch) {
            std::vector<std::string> cleanupCalls;
            auto appendValueCleanup = [&](const NativeValue& value) {
                if (value.dynamic && value.ownsDynamic) {
                    cleanupCalls.push_back("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                } else if (value.ownsClass && isManagedClassValue(value)) {
                    cleanupCalls.push_back("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                } else if (value.ownsNativeBytes) {
                    cleanupCalls.push_back("call void @zeus_bytes_release(ptr " +
                                           value.value + ")");
                } else if (value.ownsNativeString) {
                    cleanupCalls.push_back("call void @zeus_string_release(ptr " +
                                           value.value + ")");
                }
            };
            if (!hasLocalCatch) {
                for (const auto& entry : addressableLocalSlots) {
                    cleanupCalls.push_back("call void @zeus_lvalue_unregister(ptr " + entry.second + ")");
                }
                for (const auto& entry : addressableNativeLocalSlots) {
                    if (entry.second.needsLValueUnregister) {
                        cleanupCalls.push_back("call void @zeus_lvalue_unregister(ptr " + entry.second.address + ")");
                    }
                    if (entry.second.type == "ptr" &&
                        entry.second.sourceType == "bytes" &&
                        entry.second.ownsNativeBytes) {
                        const std::string bytesValue = freshNativeTmp();
                        cleanupCalls.push_back(bytesValue + " = load ptr, ptr " +
                                               entry.second.address);
                        cleanupCalls.push_back("call void @zeus_bytes_release(ptr " +
                                               bytesValue + ")");
                    } else if (entry.second.type == "ptr" &&
                               entry.second.sourceType == "string" &&
                               entry.second.ownsNativeString) {
                        const std::string stringValue = freshNativeTmp();
                        cleanupCalls.push_back(stringValue + " = load ptr, ptr " +
                                               entry.second.address);
                        cleanupCalls.push_back("call void @zeus_string_release(ptr " +
                                               stringValue + ")");
                    }
                }
                for (const auto& entry : locals) appendValueCleanup(entry.second);
                for (const auto& entry : closureLocalCells) {
                    cleanupCalls.push_back("call void @zeus_capture_cell_release(ptr " +
                                           entry.second + ")");
                }
                for (const auto& value : withValues) appendValueCleanup(value);
            }
            for (const auto& value : exceptionCallArguments) {
                appendValueCleanup(value);
            }
            for (const auto& value : exceptionCallDynamicValues) {
                cleanupCalls.push_back(detail::callArgumentCleanup(value));
            }
            for (const auto& value : valueStack) appendValueCleanup(value);
            return cleanupCalls;
        };
        for (size_t i = 0; i < parameterLLVMTypes.size(); ++i) {
            const bool byReference = i < signature.parameterIsReference.size() &&
                signature.parameterIsReference[i];
            if (byReference) {
                referenceParameterLocals.insert(static_cast<int>(i));
                locals[static_cast<int>(i)] = NativeValue{
                    "%arg" + std::to_string(i),
                    "ptr",
                    false,
                    false,
                    signature.parameterTypes[i],
                    false
                };
                locals[static_cast<int>(i)].isReference = true;
                continue;
            }
            const auto nativeAddressableParameter =
                addressableNativeLocalSlots.find(static_cast<int>(i));
            if (nativeAddressableParameter != addressableNativeLocalSlots.end()) {
                const NativeSlot& slot = nativeAddressableParameter->second;
                if (slot.type == "ptr" && slot.sourceType == "bytes") {
                    if (slot.ownsNativeBytes) {
                        emitNativeLine("call void @zeus_bytes_store_slot(ptr " +
                            slot.address + ", ptr %arg" + std::to_string(i) +
                            ", i32 0)");
                    } else {
                        emitNativeLine("store ptr %arg" + std::to_string(i) +
                            ", ptr " + slot.address + ", align 8");
                    }
                } else if (slot.type == "ptr" && slot.sourceType == "string") {
                    emitNativeLine("call void @zeus_string_store_slot(ptr " +
                        slot.address + ", ptr %arg" + std::to_string(i) + ")");
                } else {
                    emitNativeLine("store " + slot.type + " %arg" +
                        std::to_string(i) + ", ptr " + slot.address +
                        (slot.type == "i1" ? ", align 1" : ", align 8"));
                }
                locals[static_cast<int>(i)] = NativeValue{
                    "%arg" + std::to_string(i), slot.type, false, false,
                    slot.sourceType
                };
                continue;
            }
            const auto addressableParameter =
                addressableLocalSlots.find(static_cast<int>(i));
            if (addressableParameter != addressableLocalSlots.end()) {
                const std::string& slot = addressableParameter->second;
                const std::string& parameterType = signature.parameterTypes[i];
                if (usesZeusValueABI(parameterType)) {
                    emitNativeLine("call void @zeus_value_clone(ptr " + slot +
                                   ", ptr %arg" + std::to_string(i) + ")");
                } else if (isIntegerLLVMType(llvmType(parameterType))) {
                    const std::string nativeType = llvmType(parameterType);
                    if (const auto* unsignedABI =
                            ZeusBuiltins::findNativeUnsignedIntegerABI(parameterType)) {
                        std::string integerValue = "%arg" + std::to_string(i);
                        if (nativeType != "i64") {
                            integerValue = freshNativeTmp();
                            emitNativeLine(integerValue + " = zext " + nativeType +
                                " %arg" + std::to_string(i) + " to i64");
                        }
                        emitNativeLine("call void @zeus_value_init_unsigned_integer(ptr " +
                            slot + ", i64 " + integerValue + ", i32 " +
                            std::to_string(unsignedABI->integerBits) + ")");
                    } else {
                        const std::string integerValue = emitNativeIntegerValueConversion(
                            "%arg" + std::to_string(i), nativeType, parameterType,
                            "i64", "int", emitNativeLine, freshNativeTmp);
                        emitNativeLine("call void @zeus_value_init_i64(ptr " + slot +
                                       ", i64 " + integerValue + ")");
                    }
                } else if (llvmType(parameterType) == "float") {
                    emitNativeLine("call void @zeus_value_init_float(ptr " + slot +
                        ", float %arg" + std::to_string(i) + ")");
                } else if (llvmType(parameterType) == "double") {
                    emitNativeLine("call void @zeus_value_init_double(ptr " + slot +
                        ", double %arg" + std::to_string(i) + ")");
                } else if (llvmType(parameterType) == "i1") {
                    const std::string raw = freshNativeTmp();
                    emitNativeLine(raw + " = zext i1 %arg" + std::to_string(i) + " to i32");
                    emitNativeLine("call void @zeus_value_init_bool(ptr " + slot +
                                   ", i32 " + raw + ")");
                } else if (isClassType(parameterType)) {
                    emitNativeLine("call void @zeus_value_from_class_ptr(ptr " + slot +
                                   ", ptr %arg" + std::to_string(i) + ", ptr " +
                                   poolStr(parameterType) + ")");
                } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(parameterType);
                           ZeusBuiltins::isNativePointerABI(abi)) {
                    emitNativeLine("call void @" +
                        std::string(ZeusBuiltins::nativePointerToDynamicHelper(abi)) +
                        "(ptr " + slot + ", ptr %arg" + std::to_string(i) + ")");
                } else {
                    result.error = "[emit-llvm] cannot materialize reference slot for parameter type '" +
                        parameterType + "'";
                    return false;
                }
                locals[static_cast<int>(i)] = NativeValue{
                    slot, "ptr", false, true, parameterType, true
                };
                continue;
            }
            const bool dynamicParameter = usesZeusValueABI(signature.parameterTypes[i]);
            std::string parameterValue = "%arg" + std::to_string(i);
            bool ownsNativeBytes = false;
            const auto* parameterABI = ZeusBuiltins::findNativeTypeABI(
                signature.parameterTypes[i]);
            const bool borrowedBytesReceiver = i == 0 &&
                signature.ownerType == "bytes" && !signature.isStaticMethod &&
                detail::isReadOnlyBytesMethodName(methodNameForSignature(signature));
            if (parameterABI != nullptr && parameterABI->valueABI ==
                    ZeusBuiltins::NativeValueABI::NativeBytes &&
                !borrowedBytesReceiver) {
                parameterValue = freshNativeTmp();
                emitNativeLine(parameterValue + " = call ptr @zeus_bytes_clone(ptr %arg" +
                               std::to_string(i) + ")");
                ownsNativeBytes = true;
            }
            locals[static_cast<int>(i)] = NativeValue{
                parameterValue,
                parameterLLVMTypes[i],
                false,
                dynamicParameter,
                signature.parameterTypes[i],
                false
            };
            locals[static_cast<int>(i)].ownsNativeBytes = ownsNativeBytes;
            if (isClassType(signature.parameterTypes[i]) && !byReference) {
                const bool destructorReceiver = signature.operatorSymbol == "~" && i == 0;
                locals[static_cast<int>(i)].ownsClass = !destructorReceiver;
                if (!destructorReceiver) {
                    emitNativeLine("call void @zeus_class_ref_retain(ptr %arg" +
                                   std::to_string(i) + ")");
                    verifyParameterLiveOnce(
                        "%arg" + std::to_string(i), emitNativeLine);
                }
            }
        }
        const int variadicBase = static_cast<int>(parameterLLVMTypes.size());
        if (signature.hasVariadicArgs) {
            locals[variadicBase] = NativeValue{
                "%varargs", "ptr", false, true, "Array", false
            };
        }
        if (signature.hasVariadicKwargs) {
            locals[variadicBase + (signature.hasVariadicArgs ? 1 : 0)] = NativeValue{
                "%kwargs", "ptr", false, true, "Object", false
            };
        }

        auto failNative = [&](const IRType::IRInstr& ins, const std::string& message) {
            result.error = "[emit-llvm] 函数 '" + signature.name + "': " +
                           message + " (" + ins.toString() + ")";
            return false;
        };

        auto popNative = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (valueStack.empty()) {
                result.error = "[emit-llvm] 原生函数栈下溢(" + ins.toString() + ")";
                return false;
            }

            value = valueStack.back();
            valueStack.pop_back();
            return true;
        };

        auto parseNativeValue = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (!ins.args.empty() &&
                std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                const std::string nilValue = freshNativeTmp();
                emitNativeLine(nilValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_init_nil(ptr " + nilValue + ")");
                value = NativeValue{ nilValue, "ptr", false, true, "nil", true };
                return true;
            }
            if (!ins.args.empty() &&
                std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                const auto& bytes = std::get<ZeusType::Bytes>(ins.args[0]);
                const std::string contents(bytes.data.begin(), bytes.data.end());
                const std::string symbol = poolStr(contents);
                const std::string dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_bytes(ptr " + dynamicValue +
                               ", ptr " + symbol + ", i64 " +
                               std::to_string(bytes.data.size()) + ")");
                value = NativeValue{
                    dynamicValue, "ptr", false, true, "bytes", true
                };
                return true;
            }
            if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                return failNative(ins, "[emit-llvm] 原生函数 PUSH 参数不是 Value");
            }

            const auto& literal = std::get<LexerType::Value>(ins.args[0]);
            if (std::holds_alternative<std::string>(literal.data)) {
                const std::string& text = std::get<std::string>(literal.data);
                const std::string symbol = poolStr(text);
                const std::string nativeValue = freshNativeTmp();
                emitNativeLine(nativeValue + " = call ptr @zeus_string_literal_from_data(ptr " +
                               symbol + ", i64 " + std::to_string(text.size()) + ")");
                value = NativeValue{nativeValue, "ptr", false, false, "string"};
                return true;
            }

            if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) {
                int64_t integerValue = 0;
                if (!std::get<ZeusType::BigInteger>(literal.data).tryToInt64(integerValue)) {
                    const std::string symbol = poolStr(
                        std::get<ZeusType::BigInteger>(literal.data).toString());
                    const std::string dynamicValue = freshNativeTmp();
                    emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                    emitNativeLine("call void @zeus_value_from_bigint_decimal(ptr " +
                                   dynamicValue + ", ptr " + symbol + ")");
                    value = NativeValue{
                        dynamicValue, "ptr", false, true, "bigint", true
                    };
                    return true;
                }

                value = NativeValue{ std::to_string(integerValue), "i64", false };
                return true;
            }

            if (std::holds_alternative<ZeusType::BigDecimal>(literal.data)) {
                const std::string symbol = poolStr(
                    std::get<ZeusType::BigDecimal>(literal.data).toString());
                const std::string dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_bigdecimal_decimal(ptr " +
                               dynamicValue + ", ptr " + symbol + ")");
                value = NativeValue{
                    dynamicValue, "ptr", false, true, "float", true
                };
                return true;
            }

            if (std::holds_alternative<bool>(literal.data)) {
                value = NativeValue{
                    std::get<bool>(literal.data) ? "1" : "0",
                    "i1",
                    false
                };
                return true;
            }

            return failNative(ins, "[emit-llvm] 原生函数只支持 int/bool 字面量");
        };

        auto emitNativeDynamicValue = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            std::string& dynamicValue,
            bool& ownsDynamic
        ) {
            if (value.functionRef) {
                std::string error;
                if (!emitFunctionReferenceAsDynamic(
                        value,
                        emitNativeLine,
                        freshNativeTmp,
                        dynamicValue,
                        error)) {
                    return failNative(ins, error);
                }
                ownsDynamic = true;
                return true;
            }

            if (value.dynamic && value.type == "ptr") {
                dynamicValue = value.value;
                ownsDynamic = value.ownsDynamic;
                return true;
            }

            if (value.type == "classref") {
                const int32_t classId = classHandleIndex(value.sourceType);
                if (classId < 0) {
                    return failNative(ins,
                        "[emit-llvm] class value has no AOT class layout");
                }
                dynamicValue = freshNativeTmp();
                const std::string className = poolStr(value.sourceType);
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_named_class_handle(ptr " +
                               dynamicValue + ", i32 " +
                               std::to_string(classId) + ", ptr " + className + ")");
                ownsDynamic = true;
                return true;
            }

            if (value.type == "i8" && value.sourceType == "char") {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                const std::string widenedByte = freshNativeTmp();
                emitNativeLine(widenedByte + " = zext i8 " + value.value + " to i32");
                emitNativeLine("call void @zeus_value_from_char(ptr " + dynamicValue +
                               ", i32 " + widenedByte + ")");
                ownsDynamic = true;
                return true;
            }

            if (value.type == "float") {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_init_float(ptr " + dynamicValue +
                               ", float " + value.value + ")");
                ownsDynamic = true;
                return true;
            }
            if (value.type == "double") {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_init_double(ptr " + dynamicValue +
                               ", double " + value.value + ")");
                ownsDynamic = true;
                return true;
            }
            if (isIntegerLLVMType(value.type) || value.type == "i1") {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                if (isIntegerLLVMType(value.type)) {
                    if (const auto* unsignedABI =
                            ZeusBuiltins::findNativeUnsignedIntegerABI(value.sourceType)) {
                        std::string integerValue = value.value;
                        if (value.type != "i64") {
                            integerValue = freshNativeTmp();
                            emitNativeLine(integerValue + " = zext " + value.type + " " +
                                           value.value + " to i64");
                        }
                        emitNativeLine("call void @zeus_value_init_unsigned_integer(ptr " +
                            dynamicValue + ", i64 " + integerValue + ", i32 " +
                            std::to_string(unsignedABI->integerBits) + ")");
                    } else {
                        const std::string integerValue = emitNativeIntegerValueConversion(
                            value.value, value.type, value.sourceType, "i64", "int",
                            emitNativeLine, freshNativeTmp);
                    emitNativeLine("call void @zeus_value_init_i64(ptr " + dynamicValue +
                                   ", i64 " + integerValue + ")");
                    }
                } else {
                    const std::string booleanValue = freshNativeTmp();
                    emitNativeLine(booleanValue + " = zext i1 " + value.value + " to i32");
                    emitNativeLine("call void @zeus_value_init_bool(ptr " + dynamicValue +
                                   ", i32 " + booleanValue + ")");
                }
                ownsDynamic = true;
                return true;
            }

            if (value.type == "ptr" && isReferenceTypeName(value.sourceType)) {
                const std::string targetType = referencedTypeName(value.sourceType);
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                if (targetType == "int" ||
                    targetType == "int64" || targetType == "bool") {
                    emitNativeLine(std::string("call void @zeus_value_from_") +
                                   (targetType == "bool" ? "bool" : "i64") +
                                   "_ptr(ptr " + dynamicValue + ", ptr " + value.value + ")");
                } else if (isClassType(targetType)) {
                    const std::string typeName = poolStr(targetType);
                    emitNativeLine("call void @zeus_value_from_class_ptr(ptr " +
                                   dynamicValue + ", ptr " + value.value + ", ptr " +
                                   typeName + ")");
                } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                           ZeusBuiltins::isNativePointerABI(abi)) {
                    emitNativeLine("call void @" +
                                   std::string(ZeusBuiltins::nativePointerToDynamicHelper(abi)) +
                                   "(ptr " +
                                   dynamicValue + ", ptr " + value.value + ")");
                } else {
                    return failNative(ins,
                        "[emit-llvm] 该引用类型不能装箱为 ZeusValue: &" + targetType);
                }
                ownsDynamic = true;
                return true;
            }

            if (value.type == "ptr" && isClassType(value.sourceType)) {
                dynamicValue = freshNativeTmp();
                const std::string typeName = poolStr(value.sourceType);
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_class_ptr(ptr " + dynamicValue +
                               ", ptr " + value.value + ", ptr " + typeName + ")");
                ownsDynamic = true;
                return true;
            }

            const auto* sourceABI = ZeusBuiltins::findNativeTypeABI(value.sourceType);
            if (value.type == "ptr" && ZeusBuiltins::isNativePointerABI(sourceABI)) {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @" +
                               std::string(ZeusBuiltins::nativePointerToDynamicHelper(sourceABI)) +
                               "(ptr " + dynamicValue +
                               ", ptr " + value.value + ")");
                ownsDynamic = true;
                return true;
            }

            return failNative(ins, "[emit-llvm] 原生函数参数无法映射为 ZeusValue");
        };

        auto emitReferenceForSymbol = [&] (
            const IRType::SymbolRef& symbol,
            const IRType::IRInstr& ins,
            std::string& reference
        ) {
            if (symbol.isConst) {
                return failNative(ins,
                    "[emit-llvm] a const variable cannot be passed to a mutable reference parameter");
            }

            if (symbol.kind == IRType::SymbolRefKind::Global) {
                reference = freshNativeTmp();
                emitNativeLine(reference + " = call ptr @zeus_value_global_slot(i32 " +
                    std::to_string(symbol.index) + ")");
                return true;
            }

            if (symbol.kind == IRType::SymbolRefKind::Upvalue) {
                reference = freshNativeTmp();
                emitNativeLine(reference + " = call ptr @zeus_closure_env_value_slot(ptr %closure.env, i32 " +
                    std::to_string(symbol.index) + ")");
                return true;
            }

            if (symbol.kind == IRType::SymbolRefKind::Local) {
                if (referenceParameterLocals.find(symbol.index) !=
                    referenceParameterLocals.end()) {
                    reference = "%arg" + std::to_string(symbol.index);
                    return true;
                }
                const auto captured = closureLocalCells.find(symbol.index);
                if (captured != closureLocalCells.end()) {
                    reference = freshNativeTmp();
                    emitNativeLine(reference + " = call ptr @zeus_capture_cell_value_slot(ptr " +
                        captured->second + ")");
                    return true;
                }
                const auto slot = addressableLocalSlots.find(symbol.index);
                if (slot == addressableLocalSlots.end()) {
                    return failNative(ins,
                        "[emit-llvm] local reference argument has no stable storage slot");
                }
                auto local = locals.find(symbol.index);
                if (local == locals.end()) {
                    return failNative(ins,
                        "[emit-llvm] local reference argument is not initialized");
                }
                local->second.sourceType.clear();
                reference = slot->second;
                return true;
            }

            return failNative(ins,
                "[emit-llvm] reference parameter requires a local, global, or captured variable");
        };

        auto cloneNativeDynamicValue = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            NativeValue& clone
        ) {
            std::string source;
            bool ownsSource = false;
            if (!emitNativeDynamicValue(ins, value, source, ownsSource)) return false;

            const std::string destination = freshNativeTmp();
            emitNativeLine(destination + " = alloca %ZeusValue, align 8");
            emitNativeLine("call void @zeus_value_clone(ptr " + destination +
                           ", ptr " + source + ")");
            if (exceptionEdgeNormalPathReleases) {
                exceptionEdgeNormalPathReleases->push_back(destination);
            }
            // Cloning a dynamic local must not consume the local itself. If
            // conversion created a temporary ZeusValue for a native source,
            // that temporary is the only object we own and can release here.
            if (ownsSource && !value.dynamic) {
                emitNativeLine("call void @zeus_value_release(ptr " + source + ")");
            }
            clone = NativeValue{
                destination, "ptr", false, true, value.sourceType, true
            };
            return true;
        };

        auto emitFunctionABIArgumentWithContext = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            const std::string& declaredType,
            std::string& argument,
            bool& ownsDynamicArgument,
            bool deferOwnedSourceRelease,
            const FunctionSignature* argumentSignature,
            size_t parameterIndex,
            bool* ownsNativeStringArgument = nullptr
        ) {
            ownsDynamicArgument = false;
            if (ownsNativeStringArgument != nullptr) {
                *ownsNativeStringArgument = false;
            }
            if (value.functionRef) {
                // 裸函数名作为实参：物化成一个可调用的动态值（闭包）。
                // 只有动态参数（未标注、可空、ZeusValue ABI）能接收它；
                // 原生签名位（int/ptr 等）没有可放闭包的位置，仍然报错。
                const bool dynamicParameter = declaredType.empty() ||
                    declaredType.back() == '?' || usesZeusValueABI(declaredType);
                if (!dynamicParameter) {
                    return failNative(ins,
                        "[emit-llvm] 函数引用只能传给动态（未标注具体类型）的参数");
                }
                return emitNativeDynamicValue(
                    ins, value, argument, ownsDynamicArgument);
            }

            // Nullable (`?`) parameters stay dynamic in phase 1: nil has no
            // native representation, so the box (nil included) passes through
            // instead of converting to native.
            if (!declaredType.empty() && declaredType.back() == '?') {
                return emitNativeDynamicValue(
                    ins, value, argument, ownsDynamicArgument);
            }

            if (usesZeusValueABI(declaredType)) {
                return emitNativeDynamicValue(
                    ins, value, argument, ownsDynamicArgument);
            }

            const std::string expectedType = llvmType(declaredType);
            if (value.dynamic) {
                if (expectedType == "double" || expectedType == "float") {
                    argument = emitNativeFloatingValueConversion(
                        value.value, "ptr", {}, expectedType, emitNativeLine, freshNativeTmp);
                } else if (isIntegerLLVMType(expectedType) && (expectedType != "i64" ||
                    ZeusBuiltins::findNativeIntegerABI(declaredType))) {
                    argument = emitNativeIntegerValueConversion(value.value, "ptr", {},
                        expectedType, declaredType, emitNativeLine, freshNativeTmp);
                } else if (expectedType == "i64") {
                    argument = freshNativeTmp();
                    if (argumentSignature && parameterIndex <
                            argumentSignature->parameterTypes.size()) {
                        const size_t receiverOffset =
                            !argumentSignature->ownerType.empty() &&
                            !argumentSignature->isStaticMethod ? 1 : 0;
                        const size_t userArgumentIndex = parameterIndex > receiverOffset
                            ? parameterIndex - receiverOffset
                            : 0;
                        emitNativeLine(argument +
                            " = call i64 @zeus_value_require_argument_i64(ptr " +
                            value.value + ", ptr " + poolStr(argumentSignature->name) +
                            ", i32 " + std::to_string(userArgumentIndex) + ", ptr " +
                            poolStr(declaredType) + ")");
                    } else {
                        emitNativeLine(argument +
                            " = call i64 @zeus_value_require_i64(ptr " +
                            value.value + ")");
                    }
                } else if (expectedType == "i1") {
                    const std::string raw = freshNativeTmp();
                    if (argumentSignature && parameterIndex <
                            argumentSignature->parameterTypes.size()) {
                        const size_t receiverOffset =
                            !argumentSignature->ownerType.empty() &&
                            !argumentSignature->isStaticMethod ? 1 : 0;
                        const size_t userArgumentIndex = parameterIndex > receiverOffset
                            ? parameterIndex - receiverOffset
                            : 0;
                        emitNativeLine(raw +
                            " = call i32 @zeus_value_require_argument_bool(ptr " +
                            value.value + ", ptr " + poolStr(argumentSignature->name) +
                            ", i32 " + std::to_string(userArgumentIndex) + ", ptr " +
                            poolStr(declaredType) + ")");
                    } else {
                        emitNativeLine(raw +
                            " = call i32 @zeus_value_require_bool(ptr " +
                            value.value + ")");
                    }
                    argument = freshNativeTmp();
                    emitNativeLine(argument + " = icmp ne i32 " + raw + ", 0");
                } else if (expectedType == "ptr" && isClassType(declaredType)) {
                    const std::string typeName = poolStr(declaredType);
                    argument = freshNativeTmp();
                    // Class pointers are nullable. Flow inference can refine a
                    // nil-initialized local after a later assignment, so this
                    // conversion must accept nil while still checking its type.
                    emitNativeLine(argument +
                                   " = call ptr @zeus_value_try_class_ptr(ptr " +
                                   value.value + ", ptr " + typeName + ")");
                } else if (expectedType == "ptr") {
                    const auto* abi = ZeusBuiltins::findNativeTypeABI(declaredType);
                    if (!ZeusBuiltins::isNativePointerABI(abi)) {
                        return failNative(ins,
                            "[emit-llvm] ZeusValue 不能直接转换为原生 class/struct 指针");
                    }
                    argument = freshNativeTmp();
                    emitNativeLine(argument + " = call ptr @" +
                                   std::string(ZeusBuiltins::nativePointerFromDynamicHelper(abi)) +
                                   "(ptr " +
                                   value.value + ")");
                    if (ownsNativeStringArgument != nullptr && abi != nullptr &&
                        abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                        *ownsNativeStringArgument = true;
                    }
                } else if (expectedType == "%ZeusRawBytesRange" &&
                           declaredType == "&bytes") {
                    const std::string storage = freshNativeTmp();
                    emitNativeLine(storage + " = alloca %ZeusRawBytesRange, align 8");
                    emitNativeLine("call void @zeus_value_require_raw_bytes_range(ptr " +
                                   storage + ", ptr " + value.value + ")");
                    argument = freshNativeTmp();
                    emitNativeLine(argument +
                                   " = load %ZeusRawBytesRange, ptr " + storage +
                                   ", align 8");
                } else {
                    return failNative(ins,
                        "[emit-llvm] ZeusValue 不能直接转换为原生 class/struct 指针");
                }
                // A class pointer extracted from an owned ZeusValue must stay
                // alive until the call site has invoked the callee. The callee
                // retains its typed class parameters on entry; releasing the
                // source box here could otherwise destroy the object first.
                const bool deferredClassSource =
                    expectedType == "ptr" && isClassType(declaredType);
                if (value.ownsDynamic && !deferredClassSource &&
                    !deferOwnedSourceRelease) {
                    emitNativeLine("call void @zeus_value_release(ptr " + value.value + ")");
                }
                return true;
            }

            const auto* declaredABI = ZeusBuiltins::findNativeTypeABI(declaredType);
            if (value.type == "ptr" && isClassType(value.sourceType) &&
                declaredABI != nullptr &&
                declaredABI->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                if (value.sourceType != "String") {
                    return failNative(ins,
                        "[emit-llvm] 只有 String 实例可以转换为原生 string ABI");
                }
                const std::string boxedString = freshNativeTmp();
                const std::string typeName = poolStr("String");
                emitNativeLine(boxedString + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_class_ptr(ptr " + boxedString +
                               ", ptr " + value.value + ", ptr " + typeName + ")");
                argument = freshNativeTmp();
                emitNativeLine(argument +
                               " = call ptr @zeus_value_to_native_string(ptr " +
                               boxedString + ")");
                emitNativeLine("call void @zeus_value_release(ptr " + boxedString + ")");
                if (ownsNativeStringArgument != nullptr) {
                    *ownsNativeStringArgument = true;
                }
                return true;
            }

            if ((expectedType == "double" || expectedType == "float") &&
                value.sourceType != "char") {
                argument = emitNativeFloatingValueConversion(value.value, value.type,
                    value.sourceType, expectedType, emitNativeLine, freshNativeTmp);
                if (!argument.empty()) return true;
            }
            if (isIntegerLLVMType(value.type) &&
                isIntegerLLVMType(expectedType) && value.sourceType != "char") {
                argument = emitNativeIntegerValueConversion(value.value, value.type,
                    value.sourceType, expectedType, declaredType,
                    emitNativeLine, freshNativeTmp);
                return true;
            }
            if (value.type != expectedType ||
                (isClassType(declaredType) && value.sourceType != declaredType)) {
                return failNative(
                    ins,
                    "[emit-llvm] native ABI type mismatch: expected '" +
                        expectedType + "' from Zeus type '" + declaredType +
                        "', got '" + value.type + "' with source type '" +
                        value.sourceType + "' (dynamic=" +
                        (value.dynamic ? "true" : "false") + ")"
                );
            }
            argument = value.value;
            return true;
        };

        auto emitFunctionABIArgument = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            const std::string& declaredType,
            std::string& argument,
            bool& ownsDynamicArgument,
            bool deferOwnedSourceRelease,
            bool* ownsNativeStringArgument = nullptr
        ) {
            return emitFunctionABIArgumentWithContext(
                ins, value, declaredType, argument, ownsDynamicArgument,
                deferOwnedSourceRelease, nullptr, 0, ownsNativeStringArgument);
        };

        auto emitClosureInvoke = [&] (
            const IRType::IRInstr& ins,
            NativeValue& callee,
            const std::vector<NativeValue>& arguments,
            NativeValue& callResult,
            const FunctionSignature* knownSignature
        ) {
            if (!callee.dynamic || callee.type != "ptr") {
                return failNative(ins, "[emit-llvm] indirect closure call requires a ZeusValue callee");
            }
            exceptionCallArguments = arguments;
            if (callee.ownsDynamic) exceptionCallArguments.push_back(callee);
            exceptionCallDynamicValues.clear();
            std::vector<std::string> dynamicArguments(arguments.size());
            std::vector<bool> ownsDynamicArguments(arguments.size(), false);
            for (size_t i = 0; i < arguments.size(); ++i) {
                bool ownsDynamic = false;
                if (!emitNativeDynamicValue(
                        ins,
                        arguments[i],
                        dynamicArguments[i],
                        ownsDynamic)) {
                    return false;
                }
                ownsDynamicArguments[i] = ownsDynamic;
                if (ownsDynamic) {
                    exceptionCallDynamicValues.push_back(dynamicArguments[i]);
                    exceptionCallScope(
                        true, exceptionCallArguments,
                        exceptionCallDynamicValues);
                }
            }

            std::vector<std::string> referenceArguments(arguments.size(), "null");
            const size_t sourceCount = std::min(
                arguments.size(), ins.metadata.callArguments.size());
            for (size_t i = 0; i < sourceCount; ++i) {
                const auto& source = ins.metadata.callArguments[i];
                bool expectsReference = knownSignature == nullptr;
                if (knownSignature != nullptr) {
                    size_t parameterIndex = knownSignature->parameterTypes.size();
                    if (!source.namedParameter.empty()) {
                        const auto parameter = std::find(
                            knownSignature->parameterNames.begin(),
                            knownSignature->parameterNames.end(),
                            source.namedParameter);
                        if (parameter != knownSignature->parameterNames.end()) {
                            parameterIndex = static_cast<size_t>(
                                parameter - knownSignature->parameterNames.begin());
                        }
                    } else if (source.positionalIndex >= 0) {
                        parameterIndex = static_cast<size_t>(source.positionalIndex);
                    }
                    expectsReference = parameterIndex <
                            knownSignature->parameterIsReference.size() &&
                        knownSignature->parameterIsReference[parameterIndex];
                }
                if (!expectsReference) continue;
                if (!source.isAssignable || source.source.isConst) continue;
                if (source.source.kind != IRType::SymbolRefKind::Local &&
                    source.source.kind != IRType::SymbolRefKind::Global &&
                    source.source.kind != IRType::SymbolRefKind::Upvalue) {
                    continue;
                }
                if (!emitReferenceForSymbol(source.source, ins, referenceArguments[i])) {
                    return false;
                }
            }

            std::string argumentArray = "null";
            std::string argumentReferenceArray = "null";
            std::string argumentNameArray = "null";
            if (!dynamicArguments.empty()) {
                argumentArray = freshNativeTmp();
                argumentReferenceArray = freshNativeTmp();
                argumentNameArray = freshNativeTmp();
                emitNativeLine(argumentArray + " = alloca ptr, i64 " +
                               std::to_string(dynamicArguments.size()) + ", align 8");
                emitNativeLine(argumentReferenceArray + " = alloca ptr, i64 " +
                               std::to_string(dynamicArguments.size()) + ", align 8");
                emitNativeLine(argumentNameArray + " = alloca ptr, i64 " +
                               std::to_string(dynamicArguments.size()) + ", align 8");
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    const std::string slot = freshNativeTmp();
                    const std::string referenceSlot = freshNativeTmp();
                    const std::string nameSlot = freshNativeTmp();
                    emitNativeLine(slot + " = getelementptr inbounds ptr, ptr " +
                                   argumentArray + ", i64 " + std::to_string(i));
                    emitNativeLine("store ptr " + dynamicArguments[i] + ", ptr " + slot +
                                   ", align 8");
                    emitNativeLine(referenceSlot +
                                   " = getelementptr inbounds ptr, ptr " +
                                   argumentReferenceArray + ", i64 " +
                                   std::to_string(i));
                    emitNativeLine("store ptr " + referenceArguments[i] +
                                   ", ptr " + referenceSlot + ", align 8");
                    emitNativeLine(nameSlot + " = getelementptr inbounds ptr, ptr " +
                                   argumentNameArray + ", i64 " + std::to_string(i));
                    if (arguments[i].namedArgument.empty()) {
                        emitNativeLine("store ptr null, ptr " + nameSlot + ", align 8");
                    } else {
                        const std::string nameSymbol = poolStr(arguments[i].namedArgument);
                        const std::string namePointer = freshNativeTmp();
                        emitNativeLine(namePointer + " = getelementptr inbounds [" +
                            std::to_string(arguments[i].namedArgument.size() + 1) +
                            " x i8], ptr " + nameSymbol + ", i64 0, i64 0");
                        emitNativeLine("store ptr " + namePointer + ", ptr " + nameSlot +
                                       ", align 8");
                    }
                }
            }

            const std::string output = freshNativeTmp();
            emitNativeLine(output + " = alloca %ZeusValue, align 8");
            emitNativeLine("call void @zeus_closure_call_named(ptr " + output + ", ptr " +
                           callee.value + ", i32 " +
                           std::to_string(dynamicArguments.size()) + ", ptr " +
                           argumentArray + ", ptr " + argumentReferenceArray +
                           ", ptr " + argumentNameArray + ")");
            exceptionCallScope(false, {}, {});
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   dynamicArguments[i] + ")");
                }
            }
            for (const auto& argument : arguments) {
                if (argument.ownsNativeBytes) {
                    emitNativeLine("call void @zeus_bytes_release(ptr " +
                                   argument.value + ")");
                }
                if (argument.ownsNativeString) {
                    emitNativeLine("call void @zeus_string_release(ptr " +
                                   argument.value + ")");
                }
                if (isManagedClassValue(argument) && argument.ownsClass) {
                    emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                   argument.value + ")");
                }
            }
            if (callee.ownsDynamic) {
                emitNativeLine("call void @zeus_value_release(ptr " + callee.value + ")");
            }
            callResult = NativeValue{
                output, "ptr", false, true, std::string(), true
            };
            return true;
        };

        auto releaseOwnedFunctionLocals = [&]() {
            for (const auto& entry : addressableLocalSlots) {
                emitNativeLine("call void @zeus_lvalue_unregister(ptr " + entry.second + ")");
            }
            for (const auto& entry : addressableNativeLocalSlots) {
                if (entry.second.needsLValueUnregister) {
                    emitNativeLine("call void @zeus_lvalue_unregister(ptr " + entry.second.address + ")");
                }
                if (entry.second.type == "ptr" &&
                    entry.second.sourceType == "bytes" &&
                    entry.second.ownsNativeBytes) {
                    const std::string bytesValue = freshNativeTmp();
                    emitNativeLine(bytesValue + " = load ptr, ptr " +
                                   entry.second.address);
                    emitNativeLine("call void @zeus_bytes_release(ptr " +
                                   bytesValue + ")");
                } else if (entry.second.type == "ptr" &&
                           entry.second.sourceType == "string" &&
                           entry.second.ownsNativeString) {
                    const std::string stringValue = freshNativeTmp();
                    emitNativeLine(stringValue + " = load ptr, ptr " +
                                   entry.second.address);
                    emitNativeLine("call void @zeus_string_release(ptr " +
                                   stringValue + ")");
                }
            }
            for (const auto& entry : locals) {
                if (entry.second.dynamic && entry.second.ownsDynamic) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   entry.second.value + ")");
                } else if (entry.second.ownsClass &&
                           isManagedClassValue(entry.second)) {
                    emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                   entry.second.value + ")");
                } else if (entry.second.ownsNativeBytes) {
                    emitNativeLine("call void @zeus_bytes_release(ptr " +
                                   entry.second.value + ")");
                } else if (entry.second.ownsNativeString) {
                    emitNativeLine("call void @zeus_string_release(ptr " +
                                   entry.second.value + ")");
                }
            }
            for (const auto& entry : closureLocalCells) {
                emitNativeLine("call void @zeus_capture_cell_release(ptr " +
                               entry.second + ")");
            }
        };

        std::string pendingDirectCall;

        struct NativeIncomingState {
            // The block the edge really leaves from. It is not always the block
            // the IR pc belongs to: a null check for new/delete splits a block in
            // two, and the branch leaves from the tail label.
            std::string predecessor;
            // Pc of the instruction that produced the edge, so a loop backedge
            // can be matched up with its phi placeholder later on.
            size_t sourcePc = 0;
            std::map<int, NativeValue> locals;
        };

        size_t functionExitPc = chunk.size();
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            if (chunk[pc].op == VMOperator::FUNC_END) {
                functionExitPc = pc;
                break;
            }
        }

        std::set<size_t> reachableNativePcs;
        std::set<size_t> nativeBlockLeaders{ 0 };
        std::map<size_t, size_t> nativeBranchTargets;
        std::map<size_t, std::set<size_t>> nativePredecessors;
        std::vector<size_t> pendingNativePcs{ 0 };
        bool nativeExitReachable = false;

        auto normalizedNativePc = [&](long long target) -> size_t {
            return target == static_cast<long long>(chunk.size())
                ? functionExitPc
                : static_cast<size_t>(target);
        };

        while (!pendingNativePcs.empty()) {
            const size_t pc = pendingNativePcs.back();
            pendingNativePcs.pop_back();
            if (pc > functionExitPc || !reachableNativePcs.insert(pc).second) {
                continue;
            }
            if (pc == functionExitPc) {
                nativeExitReachable = true;
                continue;
            }

            const auto& ins = chunk[pc];
            if (ins.op == VMOperator::FUNC_END) {
                nativeExitReachable = true;
                continue;
            }
            if (ins.op == VMOperator::RETURN) continue;

            if (exceptionTarget[pc] >= 0 &&
                ins.op != VMOperator::TRY_BEGIN &&
                ins.op != VMOperator::TRY_END &&
                ins.op != VMOperator::IR_ANNOTATION &&
                ins.op != VMOperator::JUMP &&
                ins.op != VMOperator::LOOP_BREAK) {
                const size_t catchPc = static_cast<size_t>(exceptionTarget[pc]);
                nativeBlockLeaders.insert(catchPc);
                nativePredecessors[catchPc].insert(pc);
                pendingNativePcs.push_back(catchPc);
            }
            if (ins.op == VMOperator::THROW) continue;

            const bool unconditionalJump =
                ins.op == VMOperator::JUMP || ins.op == VMOperator::LOOP_BREAK;
            const bool conditionalJump =
                ins.op == VMOperator::JUMP_IF_FALSE ||
                ins.op == VMOperator::JUMP_IF_TRUE ||
                ins.op == VMOperator::JUMP_IF_PARAM_PROVIDED;
            if (unconditionalJump || conditionalJump) {
                int64_t offset = 0;
                const size_t offsetIndex =
                    ins.op == VMOperator::JUMP_IF_PARAM_PROVIDED ? 1 : 0;
                if (!tryArgInt(ins, offsetIndex, offset)) {
                    return failNative(ins, "LLVM 函数分支缺少有效的跳转偏移");
                }
                const long long rawTarget =
                    static_cast<long long>(pc) + 1 + offset;
                if (rawTarget < 0 ||
                    rawTarget > static_cast<long long>(chunk.size())) {
                    return failNative(ins, "LLVM 函数分支目标超出函数范围");
                }
                const size_t target = normalizedNativePc(rawTarget);
                if (target > functionExitPc) {
                    return failNative(ins, "LLVM 函数分支目标落在函数结束标记之后");
                }
                nativeBranchTargets[pc] = target;
                pendingNativePcs.push_back(target);
                if (conditionalJump) {
                    const size_t fallthrough = pc + 1 > functionExitPc
                        ? functionExitPc
                        : pc + 1;
                    pendingNativePcs.push_back(fallthrough);
                }
                continue;
            }

            const size_t fallthrough = pc + 1 > functionExitPc
                ? functionExitPc
                : pc + 1;
            pendingNativePcs.push_back(fallthrough);
        }

        for (const auto& branch : nativeBranchTargets) {
            if (reachableNativePcs.find(branch.first) == reachableNativePcs.end()) {
                continue;
            }
            nativeBlockLeaders.insert(branch.second);
            nativePredecessors[branch.second].insert(branch.first);
            if (chunk[branch.first].op == VMOperator::JUMP_IF_FALSE ||
                chunk[branch.first].op == VMOperator::JUMP_IF_TRUE ||
                chunk[branch.first].op == VMOperator::JUMP_IF_PARAM_PROVIDED) {
                const size_t fallthrough = branch.first + 1 > functionExitPc
                    ? functionExitPc
                    : branch.first + 1;
                nativeBlockLeaders.insert(fallthrough);
                nativePredecessors[fallthrough].insert(branch.first);
            }
        }
        if (nativeExitReachable) nativeBlockLeaders.insert(functionExitPc);

        for (size_t pc : reachableNativePcs) {
            if (pc >= functionExitPc ||
                nativeBranchTargets.find(pc) != nativeBranchTargets.end() ||
                chunk[pc].op == VMOperator::RETURN ||
                chunk[pc].op == VMOperator::FUNC_END) {
                continue;
            }
            const size_t fallthrough = pc + 1 > functionExitPc
                ? functionExitPc
                : pc + 1;
            if (nativeBlockLeaders.find(fallthrough) != nativeBlockLeaders.end()) {
                nativePredecessors[fallthrough].insert(pc);
            }
        }

        auto nativeLabelForPc = [&](size_t pc) {
            return pc == functionExitPc ? endLabelFor(ci) : labelFor(ci, pc);
        };

        std::set<size_t> nativeLoopHeaders;
        for (const auto& predecessors : nativePredecessors) {
            for (size_t predecessorPc : predecessors.second) {
                if (predecessorPc >= predecessors.first) {
                    nativeLoopHeaders.insert(predecessors.first);
                    break;
                }
            }
        }

        std::map<size_t, std::string> nativeBlockForPc;
        std::string activeNativeBlock = "%entry";
        for (size_t pc = 0; pc <= functionExitPc; ++pc) {
            if ((pc == 0 && nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) ||
                (pc != 0 && nativeBlockLeaders.find(pc) != nativeBlockLeaders.end())) {
                activeNativeBlock = nativeLabelForPc(pc);
            }
            nativeBlockForPc[pc] = activeNativeBlock;
        }

        std::map<size_t, std::set<std::string>> nativePredecessorBlocks;
        std::map<size_t, std::set<std::string>> nativeLoopBackedgeBlocks;
        // Pc of every loop backedge, kept next to the block labels above. The
        // labels come from the static analysis and go stale as soon as a block
        // gets split, so the phi patch uses these pcs to find the real
        // predecessor instead.
        std::map<size_t, std::set<size_t>> nativeLoopBackedgePcs;
        for (const auto& predecessors : nativePredecessors) {
            for (size_t predecessorPc : predecessors.second) {
                const auto block = nativeBlockForPc.find(predecessorPc);
                if (block == nativeBlockForPc.end()) continue;
                nativePredecessorBlocks[predecessors.first].insert(block->second);
                if (predecessorPc >= predecessors.first) {
                    nativeLoopBackedgeBlocks[predecessors.first].insert(block->second);
                    nativeLoopBackedgePcs[predecessors.first].insert(predecessorPc);
                }
            }
        }

        // Loop members are the blocks reachable from the header that can also
        // reach one of its backedge sources. A PC interval includes exit-only
        // branches (for example, a break arm), which are not loop-carried.
        std::map<std::string, size_t> nativeLeaderPcForBlock;
        for (const auto& block : nativeBlockForPc) {
            nativeLeaderPcForBlock.emplace(block.second, block.first);
        }
        std::map<std::string, std::set<std::string>> nativeSuccessorBlocks;
        for (const auto& target : nativePredecessorBlocks) {
            const auto targetBlock = nativeBlockForPc.find(target.first);
            if (targetBlock == nativeBlockForPc.end()) continue;
            for (const auto& predecessorBlock : target.second) {
                nativeSuccessorBlocks[predecessorBlock].insert(
                    targetBlock->second);
            }
        }
        std::map<size_t, std::set<size_t>> nativeLoopMemberPcs;
        for (const auto& loop : nativeLoopBackedgePcs) {
            const auto headerBlock = nativeBlockForPc.find(loop.first);
            if (headerBlock == nativeBlockForPc.end()) continue;

            std::set<std::string> forwardBlocks{ headerBlock->second };
            std::vector<std::string> pendingForward{ headerBlock->second };
            while (!pendingForward.empty()) {
                const std::string block = pendingForward.back();
                pendingForward.pop_back();
                const auto successors = nativeSuccessorBlocks.find(block);
                if (successors == nativeSuccessorBlocks.end()) continue;
                for (const auto& successor : successors->second) {
                    if (forwardBlocks.insert(successor).second) {
                        pendingForward.push_back(successor);
                    }
                }
            }

            std::set<std::string> backwardBlocks{ headerBlock->second };
            std::vector<std::string> pendingBackward;
            for (const size_t backedgePc : loop.second) {
                const auto backedgeBlock = nativeBlockForPc.find(backedgePc);
                if (backedgeBlock != nativeBlockForPc.end() &&
                    backedgeBlock->second != headerBlock->second &&
                    backwardBlocks.insert(backedgeBlock->second).second) {
                    pendingBackward.push_back(backedgeBlock->second);
                }
            }
            while (!pendingBackward.empty()) {
                const std::string block = pendingBackward.back();
                pendingBackward.pop_back();
                const auto leader = nativeLeaderPcForBlock.find(block);
                if (leader == nativeLeaderPcForBlock.end()) continue;
                const auto predecessors = nativePredecessorBlocks.find(
                    leader->second);
                if (predecessors == nativePredecessorBlocks.end()) continue;
                for (const auto& predecessor : predecessors->second) {
                    if (backwardBlocks.insert(predecessor).second &&
                        predecessor != headerBlock->second) {
                        pendingBackward.push_back(predecessor);
                    }
                }
            }

            for (const auto& block : nativeBlockForPc) {
                if (block.first < chunk.size() &&
                    forwardBlocks.find(block.second) != forwardBlocks.end() &&
                    backwardBlocks.find(block.second) != backwardBlocks.end()) {
                    nativeLoopMemberPcs[loop.first].insert(block.first);
                }
            }
        }

        // A ZeusValue binding written in a loop needs storage whose identity
        // survives every iteration. Keep this per-local so native scalar loops
        // continue to use LLVM SSA values.
        std::set<int> nativeDynamicLoopLocals;
        std::map<size_t, std::set<int>> nativeDynamicLoopHeaderLocals;
        std::map<size_t, std::set<int>> nativeDynamicLoopStoreLocals;
        std::map<size_t, std::set<int>> nativeLoopWrittenLocals;
        std::map<int, std::string> nativeLocalBindingTypes;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            if (reachableNativePcs.find(pc) == reachableNativePcs.end()) continue;
            const auto& instruction = chunk[pc];
            if (instruction.op != VMOperator::STORE_VALUE_LOCAL ||
                instruction.metadata.kind !=
                    IRType::IRMetadataKind::LocalBindingType ||
                instruction.metadata.typeName.empty()) {
                continue;
            }
            int64_t localIndex = -1;
            if (tryArgInt(instruction, 0, localIndex) && localIndex >= 0 &&
                    localIndex <= std::numeric_limits<int>::max()) {
                nativeLocalBindingTypes[static_cast<int>(localIndex)] =
                    instruction.metadata.typeName;
            }
        }
        auto isDefinitelyInitializedBeforeLoopHeader =
            [&](size_t headerPc, int localIndex) {
                if (localIndex < static_cast<int>(signature.parameterTypes.size())) {
                    return true;
                }

                using InitializationState = std::pair<size_t, bool>;
                std::vector<InitializationState> pending{ { 0, false } };
                std::set<InitializationState> visited;
                while (!pending.empty()) {
                    const auto state = pending.back();
                    pending.pop_back();
                    const size_t pc = state.first;
                    const bool initialized = state.second;
                    if (pc > functionExitPc ||
                        reachableNativePcs.find(pc) == reachableNativePcs.end() ||
                        !visited.insert(state).second) {
                        continue;
                    }
                    if (pc == headerPc) {
                        if (!initialized) return false;
                        continue;
                    }
                    if (pc == functionExitPc) continue;

                    const auto& instruction = chunk[pc];
                    if (exceptionTarget[pc] >= 0 &&
                        instruction.op != VMOperator::TRY_BEGIN &&
                        instruction.op != VMOperator::TRY_END &&
                        instruction.op != VMOperator::IR_ANNOTATION &&
                        instruction.op != VMOperator::JUMP &&
                        instruction.op != VMOperator::LOOP_BREAK) {
                        pending.emplace_back(
                            static_cast<size_t>(exceptionTarget[pc]), initialized);
                    }
                    if (instruction.op == VMOperator::THROW ||
                        instruction.op == VMOperator::RETURN ||
                        instruction.op == VMOperator::FUNC_END) {
                        continue;
                    }

                    bool initializedAfterInstruction = initialized;
                    if (instruction.op == VMOperator::STORE_VALUE_LOCAL ||
                        instruction.op == VMOperator::ASSIGN_VALUE_LOCAL) {
                        int64_t storedLocal = -1;
                        if (tryArgInt(instruction, 0, storedLocal) &&
                            storedLocal == localIndex) {
                            initializedAfterInstruction = true;
                        }
                    }

                    const bool unconditionalJump =
                        instruction.op == VMOperator::JUMP ||
                        instruction.op == VMOperator::LOOP_BREAK;
                    const bool conditionalJump =
                        instruction.op == VMOperator::JUMP_IF_FALSE ||
                        instruction.op == VMOperator::JUMP_IF_TRUE ||
                        instruction.op == VMOperator::JUMP_IF_PARAM_PROVIDED;
                    if (unconditionalJump || conditionalJump) {
                        const auto target = nativeBranchTargets.find(pc);
                        if (target != nativeBranchTargets.end()) {
                            pending.emplace_back(
                                target->second, initializedAfterInstruction);
                        }
                        if (conditionalJump) {
                            pending.emplace_back(
                                pc + 1 > functionExitPc ? functionExitPc : pc + 1,
                                initializedAfterInstruction);
                        }
                        continue;
                    }
                    pending.emplace_back(
                        pc + 1 > functionExitPc ? functionExitPc : pc + 1,
                        initializedAfterInstruction);
                }
                return true;
            };

        for (const auto& loop : nativeLoopBackedgePcs) {
            if (loop.second.empty()) continue;
            const size_t headerPc = loop.first;
            const size_t lastBackedgePc = *loop.second.rbegin();
            if (headerPc > lastBackedgePc || lastBackedgePc >= chunk.size()) continue;
            const auto loopMemberPcs = nativeLoopMemberPcs.find(headerPc);
            if (loopMemberPcs == nativeLoopMemberPcs.end() ||
                loopMemberPcs->second.empty()) {
                continue;
            }

            std::set<int> writtenInLoop;
            for (const size_t pc : loopMemberPcs->second) {
                const auto& instruction = chunk[pc];
                if (instruction.op != VMOperator::STORE_VALUE_LOCAL &&
                    instruction.op != VMOperator::ASSIGN_VALUE_LOCAL) {
                    continue;
                }
                int64_t localIndex = -1;
                if (tryArgInt(instruction, 0, localIndex) && localIndex >= 0) {
                    nativeLoopWrittenLocals[headerPc].insert(
                        static_cast<int>(localIndex));
                    writtenInLoop.insert(static_cast<int>(localIndex));
                }
            }
            for (const int localIndex : writtenInLoop) {
                const bool isParameter = localIndex >= 0 &&
                    static_cast<size_t>(localIndex) < signature.parameterTypes.size();
                if (isParameter &&
                    static_cast<size_t>(localIndex) <
                        signature.parameterIsReference.size() &&
                    signature.parameterIsReference[static_cast<size_t>(localIndex)]) {
                    continue;
                }

                const std::string* bindingType = nullptr;
                if (isParameter) {
                    bindingType = &signature.parameterTypes[
                        static_cast<size_t>(localIndex)];
                } else {
                    const auto localBindingType =
                        nativeLocalBindingTypes.find(localIndex);
                    if (localBindingType != nativeLocalBindingTypes.end()) {
                        bindingType = &localBindingType->second;
                    }
                }
                if (bindingType == nullptr || !usesZeusValueABI(*bindingType)) continue;
                if (isDefinitelyInitializedBeforeLoopHeader(headerPc, localIndex)) {
                    nativeDynamicLoopLocals.insert(localIndex);
                    nativeDynamicLoopHeaderLocals[headerPc].insert(localIndex);
                    for (const size_t pc : loopMemberPcs->second) {
                        const auto& instruction = chunk[pc];
                        if (instruction.op != VMOperator::STORE_VALUE_LOCAL &&
                            instruction.op != VMOperator::ASSIGN_VALUE_LOCAL) {
                            continue;
                        }
                        int64_t storedLocal = -1;
                        if (tryArgInt(instruction, 0, storedLocal) &&
                            storedLocal == localIndex) {
                            nativeDynamicLoopStoreLocals[pc].insert(localIndex);
                        }
                    }
                }
            }
        }

        std::map<int, std::string> nativeDynamicLoopLocalSlots;
        std::string nativeDynamicLoopSlotPrologue;
        for (const int localIndex : nativeDynamicLoopLocals) {
            const auto addressableSlot = addressableLocalSlots.find(localIndex);
            if (addressableSlot != addressableLocalSlots.end()) {
                nativeDynamicLoopLocalSlots.emplace(
                    localIndex, addressableSlot->second);
                continue;
            }
            const std::string slot = freshNativeTmp();
            nativeDynamicLoopLocalSlots.emplace(localIndex, slot);
            nativeDynamicLoopSlotPrologue += "  " + slot +
                " = alloca %ZeusValue, align 8\n";
            nativeDynamicLoopSlotPrologue +=
                "  call void @zeus_value_init_nil(ptr " + slot + ")\n";
        }
        if (!nativeDynamicLoopSlotPrologue.empty()) {
            functionText.insert(nativeEntryBody, nativeDynamicLoopSlotPrologue);
        }

        IRType::IRInstr captureInitializationInstruction;
        if (!chunk.empty()) captureInitializationInstruction = chunk.front();
        for (const int localIndex : referenceCapturedLocals) {
            const std::string cell = freshNativeTmp();
            emitNativeLine(cell + " = call ptr @zeus_capture_cell_create(ptr null)");
            closureLocalCells.emplace(localIndex, cell);

            const auto parameter = locals.find(localIndex);
            if (parameter != locals.end()) {
                std::string initialValue;
                bool ownsInitialValue = false;
                if (!emitNativeDynamicValue(
                        captureInitializationInstruction,
                        parameter->second,
                        initialValue,
                        ownsInitialValue)) {
                    return false;
                }
                emitNativeLine("call void @zeus_capture_cell_store(ptr " + cell +
                               ", ptr " + initialValue + ")");
                if (ownsInitialValue) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   initialValue + ")");
                }
            }
        }

        if (nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) {
            nativePredecessorBlocks[0].insert("%entry");
        }

        std::map<size_t, std::vector<NativeIncomingState>> nativeIncomingStates;
        std::map<size_t, std::set<int>> nativeLoopHeaderLocals;
        struct NativeLoopPhiPatch {
            size_t headerPc = 0;
            int localIndex = -1;
            size_t sourcePc = 0;
            std::string placeholder;
            // The backedge leaves from a block that is still being emitted, so
            // its label is only known once the whole function body is done.
            std::string labelPlaceholder;
            std::string type;
            bool dynamic = false;
            std::string sourceType;
        };
        std::vector<NativeLoopPhiPatch> nativeLoopPhiPatches;
        size_t nativeLoopPhiPlaceholderCount = 0;
        bool nativeBlockTerminated = false;

        // A conditional branch that leaves a try region must run the depth
        // exits on the taken path only, so the edge is routed through a
        // pass-through wrapper block emitted right after the terminator.
        auto tryExitEdgeTarget = [&](const std::string& realLabel, int exits,
                                     size_t edgeTargetPc, size_t sourcePc,
                                     std::string& outBlocks) -> std::string {
            if (exits <= 0) return realLabel;
            const std::string wrapper = freshNativeTmp();
            outBlocks += wrapper.substr(1) + ":\n";
            for (int index = 0; index < exits; ++index) {
                outBlocks += "  call void @zeus_try_exit()\n";
            }
            outBlocks += "  br label " + realLabel + "\n";
            // Phi placeholders for this edge now merge from the wrapper block.
            auto incoming = nativeIncomingStates.find(edgeTargetPc);
            if (incoming != nativeIncomingStates.end()) {
                for (auto& edge : incoming->second) {
                    if (edge.sourcePc == sourcePc &&
                        edge.predecessor == currentNativeBlock) {
                        edge.predecessor = wrapper;
                    }
                }
            }
            return wrapper;
        };

        auto recordNativeEdge = [&](size_t target, const IRType::IRInstr& ins,
                                    size_t sourcePc) {
            if (target == functionExitPc && returnsDynamic && !valueStack.empty()) {
                // Falling off a function returns nil. Discard any unused
                // expression result left by a statement such as a subscript
                // assignment before merging into that implicit-return block.
                for (const auto& value : valueStack) {
                    if (value.dynamic && value.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       value.value + ")");
                    }
                }
                valueStack.clear();
            }
            if (!valueStack.empty()) {
                std::string pending = "LLVM 函数分支边要求操作数栈为空; pc=" +
                    std::to_string(sourcePc) + ", depth=" +
                    std::to_string(valueStack.size()) + ", values=";
                for (const auto& value : valueStack) {
                    pending += " [" + value.type + "," + value.sourceType +
                        (value.dynamic ? ",dynamic" : ",native") + "]";
                }
                const size_t historyStart = sourcePc > 24 ? sourcePc - 24 : 0;
                pending += "; preceding instructions=";
                for (size_t previous = historyStart; previous <= sourcePc; ++previous) {
                    pending += " {" + std::to_string(previous) + ":" +
                        chunk[previous].toString() + "}";
                }
                return failNative(ins, pending);
            }

            std::map<int, NativeValue> edgeLocals = locals;

            // Copy preheader values into dedicated storage before entering a
            // dynamic loop. Backedges already carry this stable slot.
            const auto dynamicLoopHeader = nativeDynamicLoopHeaderLocals.find(target);
            if (dynamicLoopHeader != nativeDynamicLoopHeaderLocals.end() &&
                (sourcePc < target || currentNativeBlock == "%entry")) {
                for (const int index : dynamicLoopHeader->second) {
                    const auto slot = nativeDynamicLoopLocalSlots.find(index);
                    const auto edgeValue = edgeLocals.find(index);
                    if (slot == nativeDynamicLoopLocalSlots.end() ||
                        edgeValue == edgeLocals.end()) {
                        return failNative(ins,
                            "dynamic loop local has no preheader value or stable slot");
                    }
                    const NativeValue original = edgeValue->second;
                    if (original.value != slot->second) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, original, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       slot->second + ")");
                        emitNativeLine("call void @zeus_value_clone(ptr " +
                                       slot->second + ", ptr " + dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (original.ownsClass && isManagedClassValue(original)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           original.value + ")");
                        }
                    }
                    edgeValue->second = NativeValue{
                        slot->second, "ptr", false, true, std::string(), true
                    };
                }
            }

            const auto loopHeaderLocals = nativeLoopHeaderLocals.find(target);
            if (loopHeaderLocals != nativeLoopHeaderLocals.end()) {
                for (auto local = edgeLocals.begin(); local != edgeLocals.end();) {
                    if (loopHeaderLocals->second.find(local->first) !=
                        loopHeaderLocals->second.end()) {
                        ++local;
                        continue;
                    }
                    if (local->second.dynamic && local->second.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       local->second.value + ")");
                    } else if (local->second.ownsClass &&
                               isManagedClassValue(local->second)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       local->second.value + ")");
                    }
                    local = edgeLocals.erase(local);
                }
                for (int index : loopHeaderLocals->second) {
                    if (edgeLocals.find(index) == edgeLocals.end()) {
                        return failNative(ins,
                            "循环回边缺少循环头已有的局部变量");
                    }
                }
            }

            const auto predecessors = nativePredecessorBlocks.find(target);
            const bool needsMerge = predecessors != nativePredecessorBlocks.end() &&
                                    predecessors->second.size() > 1;
            if (needsMerge) {
                for (auto& local : edgeLocals) {
                    if (!local.second.dynamic || local.second.ownsDynamic) continue;
                    NativeValue owned;
                    if (!cloneNativeDynamicValue(ins, local.second, owned)) return false;
                    local.second = std::move(owned);
                }
            }

            auto& incoming = nativeIncomingStates[target];
            auto existing = std::find_if(
                incoming.begin(), incoming.end(),
                [&](const NativeIncomingState& edge) {
                    return edge.predecessor == currentNativeBlock;
                }
            );
            if (existing == incoming.end()) {
                incoming.push_back(NativeIncomingState{
                    currentNativeBlock, sourcePc, std::move(edgeLocals)
                });
            }
            return true;
        };

        auto promoteBorrowedDynamicLocalsForConditionalEdges = [&](
            size_t firstTarget, size_t secondTarget, const IRType::IRInstr& ins
        ) {
            auto needsMerge = [&](size_t target) {
                const auto predecessors = nativePredecessorBlocks.find(target);
                return predecessors != nativePredecessorBlocks.end() &&
                       predecessors->second.size() > 1;
            };
            if (!needsMerge(firstTarget) && !needsMerge(secondTarget)) return true;

            // Promote once before the conditional branch. Cloning inside each
            // edge snapshot would create an unused owned copy on the other path.
            for (auto& local : locals) {
                if (!local.second.dynamic || local.second.ownsDynamic) continue;
                NativeValue owned;
                if (!cloneNativeDynamicValue(ins, local.second, owned)) return false;
                local.second = std::move(owned);
            }
            return true;
        };
        bool exceptionEdgeFailed = false;
        recordExceptionEdge = [&](size_t target, const std::string& resumeLabel) {
            const std::string savedBlock = currentNativeBlock;
            std::vector<NativeValue> savedStack = std::move(valueStack);
            valueStack.clear();
            currentNativeBlock = resumeLabel;
            nativePredecessorBlocks[target].insert(resumeLabel);
            if (!recordNativeEdge(target, chunk[activeExceptionPc], activeExceptionPc)) {
                exceptionEdgeFailed = true;
            }
            currentNativeBlock = savedBlock;
            valueStack = std::move(savedStack);
        };
        enterExceptionContinuation = [&](const std::string& continuation) {
            currentNativeBlock = continuation;
        };

        auto enterNativeBlock = [&](size_t pc, const IRType::IRInstr& ins) {
            blockVerifiedLiveValues.clear();
            const std::string label = nativeLabelForPc(pc);
            functionText += label.substr(1) + ":\n";
            currentNativeBlock = label;
            nativeBlockTerminated = false;
            if (!valueStack.empty()) {
                return failNative(ins,
                    "LLVM 函数基本块合流时操作数栈不为空");
            }

            auto incoming = nativeIncomingStates.find(pc);
            if (incoming == nativeIncomingStates.end() || incoming->second.empty()) {
                return failNative(ins,
                    "LLVM 函数基本块没有可用的前驱状态");
            }

            std::set<int> localIndices;
            const auto loopBackedges = nativeLoopBackedgePcs.find(pc);
            const bool hasBackedges = loopBackedges != nativeLoopBackedgePcs.end() &&
                                      !loopBackedges->second.empty();
            bool hasLoopEntry = false;
            if (hasBackedges) {
                // A local first declared in the loop body only exists on
                // backedges. It is recreated on the next iteration and must
                // not become a loop-header phi or a loop-carried value.
                for (const auto& edge : incoming->second) {
                    if (loopBackedges->second.find(edge.sourcePc) !=
                        loopBackedges->second.end()) {
                        continue;
                    }
                    std::set<int> entryLocals;
                    for (const auto& local : edge.locals) {
                        entryLocals.insert(local.first);
                    }
                    if (!hasLoopEntry) {
                        localIndices = std::move(entryLocals);
                        hasLoopEntry = true;
                        continue;
                    }
                    for (auto local = localIndices.begin();
                         local != localIndices.end();) {
                        if (entryLocals.find(*local) == entryLocals.end()) {
                            local = localIndices.erase(local);
                        } else {
                            ++local;
                        }
                    }
                }
            }
            if (!hasBackedges || !hasLoopEntry) {
                for (const auto& edge : incoming->second) {
                    for (const auto& local : edge.locals) {
                        localIndices.insert(local.first);
                    }
                }
            }

            std::vector<std::string> phiLines;
            std::vector<std::string> cleanupLines;
            std::map<int, NativeValue> mergedLocals;
            for (int index : localIndices) {
                std::vector<const NativeValue*> values;
                values.reserve(incoming->second.size());
                bool presentOnEveryEdge = true;
                for (const auto& edge : incoming->second) {
                    const auto local = edge.locals.find(index);
                    if (local == edge.locals.end()) {
                        presentOnEveryEdge = false;
                        values.push_back(nullptr);
                    } else {
                        values.push_back(&local->second);
                    }
                }

                const NativeValue* prototype = nullptr;
                for (const NativeValue* value : values) {
                    if (value != nullptr) {
                        prototype = value;
                        break;
                    }
                }
                if (prototype == nullptr) continue;

                if (hasBackedges && !presentOnEveryEdge) {
                    std::string detail =
                        "loop-carried local is not initialized on every loop entry";
                    detail += " pc=" + std::to_string(pc) +
                        ", local=" + std::to_string(index) + ", edges=";
                    for (size_t edgeIndex = 0;
                         edgeIndex < incoming->second.size(); ++edgeIndex) {
                        const auto& edge = incoming->second[edgeIndex];
                        const bool backedge =
                            loopBackedges->second.find(edge.sourcePc) !=
                            loopBackedges->second.end();
                        detail += " [source=" + std::to_string(edge.sourcePc) +
                            ", block=" + edge.predecessor +
                            (backedge ? ", backedge, " : ", entry, " ) +
                            (values[edgeIndex] == nullptr
                                ? std::string("missing")
                                : std::string("present"));
                        if (values[edgeIndex] != nullptr) {
                            detail += ", type=" + values[edgeIndex]->type +
                                (values[edgeIndex]->dynamic ? ", dynamic" : ", native") +
                                ", sourceType=" + values[edgeIndex]->sourceType;
                        }
                        detail += "]";
                    }
                    return failNative(ins, detail);
                }

                const auto stableLoopLocal = nativeDynamicLoopLocalSlots.find(index);
                const auto dynamicLoopHeader = nativeDynamicLoopHeaderLocals.find(pc);
                if (hasBackedges && stableLoopLocal != nativeDynamicLoopLocalSlots.end() &&
                    dynamicLoopHeader != nativeDynamicLoopHeaderLocals.end() &&
                    dynamicLoopHeader->second.find(index) !=
                        dynamicLoopHeader->second.end()) {
                    mergedLocals[index] = NativeValue{
                        stableLoopLocal->second, "ptr", false, true,
                        std::string(), true
                    };
                    continue;
                }

                auto compatible = [&](const NativeValue& value) {
                    const bool dynamicSourceTypesCompatible = value.dynamic &&
                        prototype->dynamic && value.type == "ptr" &&
                        prototype->type == "ptr";
                    const bool nativeScalarSourceTypesCompatible =
                        !value.dynamic && !prototype->dynamic &&
                        value.type == prototype->type &&
                        (value.type == "i64" || value.type == "i1") &&
                        !value.functionRef && !prototype->functionRef;
                    return value.type == prototype->type &&
                           value.dynamic == prototype->dynamic &&
                           (value.sourceType == prototype->sourceType ||
                            dynamicSourceTypesCompatible ||
                            nativeScalarSourceTypesCompatible) &&
                           value.functionRef == prototype->functionRef;
                };
                for (const NativeValue* value : values) {
                    if (value != nullptr && !compatible(*value)) {
                        const auto declaredLocalType =
                            nativeLocalBindingTypes.find(index);
                        result.error = "[emit-llvm] 函数 '" + signature.name +
                            "' 的局部变量 " + std::to_string(index) +
                            " 在 if/else 合流处类型不一致（pc=" +
                            std::to_string(pc) + ", " + prototype->type + "/" +
                            (prototype->dynamic ? "dynamic" : "native") + "/" +
                            prototype->sourceType + " vs " + value->type + "/" +
                            (value->dynamic ? "dynamic" : "native") + "/" +
                            value->sourceType + ", declared=" +
                            (declaredLocalType == nativeLocalBindingTypes.end()
                                ? std::string("<unknown>")
                                : declaredLocalType->second) + ")";
                        return false;
                    }
                }

                if (!presentOnEveryEdge) {
                    // This local leaves scope at the merge. Build a separate
                    // cleanup phi for each owned resource kind because a dynamic
                    // ZeusValue and a native class pointer have different release
                    // functions even though both travel as LLVM ptr values.
                    auto appendDroppedResourceCleanup = [&](bool dynamicValue) {
                        auto isOwnedResource = [&](const NativeValue* value) {
                            if (value == nullptr || value->type != "ptr") return false;
                            return dynamicValue
                                ? value->dynamic && value->ownsDynamic
                                : value->ownsClass && isManagedClassValue(*value);
                        };
                        const bool hasOwnedResource = std::any_of(
                            values.begin(), values.end(), isOwnedResource);
                        if (!hasOwnedResource) return;

                        const std::string phi = freshNativeTmp();
                        std::string expression = phi + " = phi ptr ";
                        for (size_t i = 0; i < values.size(); ++i) {
                            if (i != 0) expression += ", ";
                            expression += "[ " +
                                (isOwnedResource(values[i])
                                    ? values[i]->value : std::string("null")) +
                                ", " + incoming->second[i].predecessor + " ]";
                        }
                        phiLines.push_back(std::move(expression));
                        cleanupLines.push_back(
                            std::string("call void @") +
                            (dynamicValue ? "zeus_value_release(ptr "
                                          : "zeus_class_ref_release(ptr ") +
                            phi + ")");
                    };
                    appendDroppedResourceCleanup(true);
                    appendDroppedResourceCleanup(false);
                    continue;
                }

                bool sameValue = true;
                bool allOwned = true;
                for (const NativeValue* value : values) {
                    sameValue = sameValue && value->value == prototype->value;
                    allOwned = allOwned && value->ownsDynamic;
                }
                if (sameValue && !hasBackedges) {
                    NativeValue merged = *prototype;
                    if (merged.dynamic) merged.ownsDynamic = allOwned;
                    mergedLocals[index] = std::move(merged);
                    continue;
                }

                if (prototype->functionRef || prototype->type.empty() ||
                    (prototype->dynamic && !allOwned)) {
                    result.error = "[emit-llvm] 函数 '" + signature.name +
                        "' 的局部变量无法在 if/else 合流处合并";
                    return false;
                }

                const std::string phi = freshNativeTmp();
                std::string expression = phi + " = phi " + prototype->type + " ";
                const bool dynamicPointerPhi = hasBackedges &&
                    prototype->dynamic && prototype->type == "ptr";
                const auto loopWrites = nativeLoopWrittenLocals.find(pc);
                const bool loopLocalWritten = loopWrites !=
                        nativeLoopWrittenLocals.end() &&
                    loopWrites->second.find(index) != loopWrites->second.end();
                // A loop-invariant ZeusValue keeps its nominal type hint through
                // the loop phi; reassigned values may change runtime type.
                const bool preserveDynamicSourceType =
                    dynamicPointerPhi && !loopLocalWritten;
                const std::string loopPhiSourceType =
                    dynamicPointerPhi && !preserveDynamicSourceType
                    ? std::string()
                    : prototype->sourceType;
                for (size_t i = 0; i < values.size(); ++i) {
                    if (i != 0) expression += ", ";
                    expression += "[ " + values[i]->value + ", " +
                        incoming->second[i].predecessor + " ]";
                }
                if (hasBackedges) {
                    // A backedge is emitted after the header, so both the value
                    // it carries and the block it leaves from are patched in
                    // once the whole body exists. Keying the patch on the pc
                    // that produced the edge is what makes this survive block
                    // splitting: the label recorded while emitting the branch
                    // is the real predecessor, not the IR block it started in.
                    for (size_t sourcePc : loopBackedges->second) {
                        const std::string placeholder = "%loop_phi_" +
                            std::to_string(nativeLoopPhiPlaceholderCount++);
                        const std::string labelPlaceholder = "%loop_phi_label_" +
                            std::to_string(nativeLoopPhiPlaceholderCount++);
                        expression += ", [ " + placeholder + ", " +
                            labelPlaceholder + " ]";
                        nativeLoopPhiPatches.push_back(NativeLoopPhiPatch{
                            pc,
                            index,
                            sourcePc,
                            placeholder,
                            labelPlaceholder,
                            prototype->type,
                            prototype->dynamic,
                            loopPhiSourceType
                        });
                    }
                }
                phiLines.push_back(std::move(expression));
                propagateVerifiedPhi(
                    phi, values, hasBackedges && loopLocalWritten);

                NativeValue merged = *prototype;
                merged.value = phi;
                if (merged.dynamic) merged.ownsDynamic = true;
                if (dynamicPointerPhi) {
                    if (!preserveDynamicSourceType) {
                        // A rebound dynamic pointer can change its runtime type
                        // on the backedge, so keep the ZeusValue representation
                        // and drop the entry edge's nominal hint for the loop.
                        merged.sourceType.clear();
                    }
                    // If this local is never rebound in the loop, its nominal
                    // type remains valid even if a backedge value lost metadata
                    // while flowing through an unrelated operation.
                } else {
                    for (const NativeValue* value : values) {
                        if (value->sourceType != merged.sourceType) {
                            merged.sourceType.clear();
                            break;
                        }
                    }
                }
                mergedLocals[index] = std::move(merged);
            }

            if (hasBackedges) {
                auto& loopLocals = nativeLoopHeaderLocals[pc];
                for (const auto& local : mergedLocals) {
                    loopLocals.insert(local.first);
                }
            }

            for (const auto& line : phiLines) emitNativeLine(line);
            for (const auto& line : cleanupLines) emitNativeLine(line);
            locals = std::move(mergedLocals);
            return true;
        };

        auto recordNativeFallthrough = [&](size_t pc, const IRType::IRInstr& ins) {
            if (pc >= functionExitPc) return true;
            const size_t target = pc + 1 > functionExitPc
                ? functionExitPc
                : pc + 1;
            if (nativeBlockLeaders.find(target) == nativeBlockLeaders.end() ||
                reachableNativePcs.find(target) == reachableNativePcs.end()) {
                return true;
            }
            if (!recordNativeEdge(target, ins, pc)) return false;
            emitNativeLine("br label " + nativeLabelForPc(target));
            nativeBlockTerminated = true;
            return true;
        };

        auto emitImplicitNilReturn = [&]() {
            if (definesVoidReturn) {
                // C void 函数走到函数末尾：丢弃残留表达式值，直接返回。
                for (const auto& value : valueStack) {
                    if (value.dynamic && value.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       value.value + ")");
                    }
                    if (value.ownsClass && isManagedClassValue(value)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       value.value + ")");
                    }
                    if (value.ownsNativeBytes) {
                        emitNativeLine("call void @zeus_bytes_release(ptr " +
                                       value.value + ")");
                    }
                    if (value.ownsNativeString) {
                        emitNativeLine("call void @zeus_string_release(ptr " +
                                       value.value + ")");
                    }
                }
                valueStack.clear();
                releaseOwnedFunctionLocals();
                if (emitFunctionSafepoints) {
                    emitNativeLine("call void @zeus_gc_safepoint()");
                }
                emitNativeLine("ret void");
                nativeBlockTerminated = true;
                return true;
            }
            if (!returnsDynamic) {
                result.error = "[emit-llvm] 函数 '" + signature.name +
                    "' 的原生返回类型存在可达的隐式 nil 路径";
                return false;
            }
            if (!valueStack.empty()) {
                result.error = "[emit-llvm] 函数 '" + signature.name +
                    "' 在隐式返回 nil 时操作数栈不为空";
                return false;
            }
            emitNativeLine("call void @zeus_value_init_nil(ptr %return.out)");
            releaseOwnedFunctionLocals();
            if (emitFunctionSafepoints) {
                emitNativeLine("call void @zeus_gc_safepoint()");
            }
            emitNativeLine("ret void");
            nativeBlockTerminated = true;
            return true;
        };

        if (nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) {
            if (chunk.empty() || !recordNativeEdge(0, chunk.front(), 0)) return false;
            emitNativeLine("br label " + nativeLabelForPc(0));
            nativeBlockTerminated = true;
        }

        auto emitNativeDirectCall = [&](const IRType::IRInstr& ins) {
            if (pendingDirectCall.empty()) {
                return failNative(ins, "[emit-llvm] 静态运算缺少 direct_call 标注");
            }

            auto target = functionSignatures.find(pendingDirectCall);
            if (target == functionSignatures.end()) {
                return failNative(ins, "[emit-llvm] 找不到静态运算目标'" +
                                  pendingDirectCall + "'");
            }

            const auto& signature = target->second;
            if (signature.parameterTypes.size() != 2) {
                return failNative(ins, "[emit-llvm] 静态二元运算必须有两个参数");
            }

            NativeValue right;
            NativeValue left;
            if (!popNative(ins, right) || !popNative(ins, left) ||
                left.functionRef || right.functionRef) {
                return failNative(ins, "[emit-llvm] 静态运算操作数无效");
            }

            const bool returnsDynamic = usesZeusValueABI(signature.returnType);
            const std::string resultType = returnsDynamic
                ? "void"
                : llvmType(signature.returnType);
            if (resultType.empty()) {
                return failNative(ins, "[emit-llvm] 静态运算返回类型没有 LLVM ABI 映射");
            }

            const std::string temporary = freshNativeTmp();
            if (returnsDynamic) {
                emitNativeLine(temporary + " = alloca %ZeusValue, align 8");
            }
            std::string callText = returnsDynamic
                ? "call void " + llvmCallTarget(signature.name) + "(ptr " + temporary
                : temporary + " = call " + resultType + " " + llvmCallTarget(signature.name) + "(";
            std::vector<std::string> dynamicArguments;
            std::vector<bool> ownsDynamicArguments;
            const NativeValue arguments[] = { left, right };
            exceptionCallArguments = { left, right };
            exceptionCallDynamicValues.clear();
            std::vector<std::string> nativeByteArguments;
            std::vector<std::string> nativeStringArguments;
            for (size_t i = 0; i < 2; ++i) {
                if (i > 0 || returnsDynamic) callText += ", ";
                std::string argument;
                bool ownsDynamic = false;
                bool ownsNativeStringArgument = false;
                if (!emitFunctionABIArgument(
                        ins, arguments[i], signature.parameterTypes[i],
                        argument, ownsDynamic, true,
                        &ownsNativeStringArgument)) {
                    return false;
                }
                if (ownsNativeStringArgument) {
                    nativeStringArguments.push_back(argument);
                    NativeValue cleanupValue{
                        argument, "ptr", false, false, "string"
                    };
                    cleanupValue.ownsNativeString = true;
                    exceptionCallArguments.push_back(std::move(cleanupValue));
                    exceptionCallScope(
                        true, exceptionCallArguments, exceptionCallDynamicValues);
                }
                callText += functionParameterABIType(signature.parameterTypes[i]) +
                            " " + argument;
                if (usesZeusValueABI(signature.parameterTypes[i])) {
                    dynamicArguments.push_back(argument);
                    ownsDynamicArguments.push_back(ownsDynamic);
                    if (ownsDynamic) {
                        exceptionCallDynamicValues.push_back(argument);
                    }
                    exceptionCallScope(
                        true, exceptionCallArguments, exceptionCallDynamicValues);
                }
                if (signature.parameterTypes[i] == "bytes" &&
                    arguments[i].dynamic) {
                    nativeByteArguments.push_back(argument);
                    exceptionCallDynamicValues.push_back(
                        detail::nativeBytesCleanupToken(argument));
                    exceptionCallScope(
                        true, exceptionCallArguments, exceptionCallDynamicValues);
                }
            }
            if (signature.hasDefaultParameters()) {
                callText += ", i64 3";
            }
            callText += ")";
            emitNativeLine(callText);
            exceptionCallScope(false, {}, {});
            std::set<std::string> releasedDynamicArguments;
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i] &&
                    releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   dynamicArguments[i] + ")");
                }
            }
            for (const auto& argument : nativeByteArguments) {
                emitNativeLine("call void @zeus_bytes_release(ptr " + argument + ")");
            }
            for (const auto& argument : nativeStringArguments) {
                emitNativeLine("call void @zeus_string_release(ptr " + argument + ")");
            }
            for (const auto& argument : arguments) {
                if (argument.dynamic && argument.ownsDynamic &&
                    releasedDynamicArguments.insert(argument.value).second) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   argument.value + ")");
                }
                if (argument.ownsNativeBytes) {
                    emitNativeLine("call void @zeus_bytes_release(ptr " +
                                   argument.value + ")");
                }
                if (argument.ownsNativeString) {
                    emitNativeLine("call void @zeus_string_release(ptr " +
                                   argument.value + ")");
                }
                if (isManagedClassValue(argument) && argument.ownsClass) {
                    emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                   argument.value + ")");
                }
            }
            valueStack.push_back(NativeValue{
                temporary,
                returnsDynamic ? "ptr" : resultType,
                false,
                returnsDynamic,
                signature.returnType,
                returnsDynamic
            });
            valueStack.back().ownsClass =
                !returnsDynamic && isClassType(signature.returnType);
            valueStack.back().ownsNativeBytes = !returnsDynamic &&
                signature.returnType == "bytes";
            valueStack.back().ownsNativeString = !returnsDynamic &&
                signature.returnType == "string";
            pendingDirectCall.clear();
            return true;
        };

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            if (reachableNativePcs.find(pc) == reachableNativePcs.end()) continue;
            const auto& ins = chunk[pc];
            activeExceptionPc = pc;
            activeExceptionTarget = exceptionTarget[pc];

            if (ins.op == VMOperator::CATCH_BEGIN &&
                nativeIncomingStates.find(pc) == nativeIncomingStates.end()) {
                const auto end = catchEndForBegin.find(pc);
                if (end == catchEndForBegin.end()) {
                    return failNative(ins, "catch block has no matching CATCH_END");
                }
                functionText += nativeLabelForPc(pc).substr(1) + ":\n";
                emitNativeLine("unreachable");
                nativeBlockTerminated = true;
                pc = end->second;
                if (pc + 1 < chunk.size() &&
                    chunk[pc + 1].metadata.tag == "fin_cleanup_rethrow") {
                    ++pc;
                }
                continue;
            }

            if ((pc != 0 || nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) &&
                nativeBlockLeaders.find(pc) != nativeBlockLeaders.end()) {
                if (!enterNativeBlock(pc, ins)) return false;
            }

            if (pc == functionExitPc || ins.op == VMOperator::FUNC_END) {
                if (!emitImplicitNilReturn()) return false;
                break;
            }

            if (ins.op == VMOperator::FUNC_BEGIN) {
                if (!recordNativeFallthrough(pc, ins)) return false;
                continue;
            }

            if (ins.op == VMOperator::IR_ANNOTATION) {
                if (annotationKind(ins, "direct_call")) {
                    std::string directCallName;
                    if (!annotationSymbolName(ins, directCallName)) {
                        return failNative(ins, "[emit-llvm] 直接调用标注缺少函数名");
                    }
                    const auto* target = resolveFunctionSignature(directCallName);
                    pendingDirectCall = target != nullptr &&
                        !hasCompleteFunctionAnnotations(*target)
                            ? std::string()
                            : (target != nullptr ? target->name : directCallName);
                }
                if (!recordNativeFallthrough(pc, ins)) return false;
                continue;
            }

            switch (ins.op) {
                case VMOperator::TRY_BEGIN:
                    // Raw address checks are gated on a thread-local depth so
                    // they only run while a try region is dynamically active.
                    functionText += "  call void @zeus_try_enter()\n";
                    break;

                case VMOperator::TRY_END:
                    functionText += "  call void @zeus_try_exit()\n";
                    break;

                case VMOperator::WITH_BEGIN:
                case VMOperator::WITH_END: {
                    NativeValue receiver;
                    if (ins.op == VMOperator::WITH_BEGIN) {
                        if (!popNative(ins, receiver) || receiver.dynamic ||
                            receiver.type != "ptr" ||
                            !isClassType(receiver.sourceType)) {
                            return failNative(ins,
                                "[emit-llvm] WITH_BEGIN requires a typed class instance");
                        }
                        int64_t captureSlot = -1;
                        if (!tryArgInt(ins, 0, captureSlot)) {
                            return failNative(ins, "[emit-llvm] invalid with capture slot");
                        }
                        if (captureSlot >= 0) {
                            auto existing = locals.find(static_cast<int>(captureSlot));
                            if (isManagedClassValue(receiver) && !receiver.ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                               receiver.value + ")");
                            }
                            if (existing != locals.end() && existing->second.ownsClass &&
                                isManagedClassValue(existing->second)) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               existing->second.value + ")");
                            }
                            NativeValue capturedReceiver = receiver;
                            capturedReceiver.ownsClass = isManagedClassValue(capturedReceiver);
                            locals[static_cast<int>(captureSlot)] = capturedReceiver;
                            receiver.ownsClass = false;
                            withValues.push_back(receiver);
                        } else {
                            withValues.push_back(receiver);
                            receiver.ownsClass = false;
                        }
                    } else {
                        if (withValues.empty()) {
                            return failNative(ins, "[emit-llvm] WITH_END has no active with block");
                        }
                        receiver = withValues.back();
                        withValues.pop_back();
                    }

                    const std::string methodName = ins.op == VMOperator::WITH_BEGIN
                        ? "operator[with]" : "operator[~with]";
                    const auto method = functionSignatures.find(
                        receiver.sourceType + "::" + methodName);
                    if (method == functionSignatures.end() &&
                        ins.op == VMOperator::WITH_END) {
                        if (receiver.ownsClass && isManagedClassValue(receiver)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           receiver.value + ")");
                        }
                        break;
                    }
                    if (method == functionSignatures.end()) {
                        return failNative(ins,
                            "[emit-llvm] with receiver has no operator[with]: " +
                            receiver.sourceType);
                    }
                    auto convertWithArgument = [&](const NativeValue& value,
                                                   const std::string& declaredType,
                                                   std::string& nativeArgument,
                                                   std::string& cleanupValue) {
                        cleanupValue.clear();
                        bool ownsDynamic = false;
                        bool ownsNativeString = false;
                        if (!emitFunctionABIArgument(ins, value, declaredType,
                                nativeArgument, ownsDynamic, true,
                                &ownsNativeString)) {
                            return false;
                        }
                        if (ownsDynamic) {
                            cleanupValue = nativeArgument;
                        } else if (ownsNativeString) {
                            cleanupValue = detail::nativeStringCleanupToken(nativeArgument);
                        } else if (declaredType == "bytes" && value.dynamic) {
                            cleanupValue = detail::nativeBytesCleanupToken(nativeArgument);
                        }
                        return true;
                    };
                    NativeValue ignoredResult;
                    if (!emitKnownClassMethodCall(ins, methodName, {receiver},
                            ignoredResult, emitNativeLine, freshNativeTmp,
                            convertWithArgument,
                            [&](const std::string& value) {
                                emitNativeLine("call void @zeus_value_release(ptr " + value + ")");
                            },
                            [&](const IRType::IRInstr& at, const std::string& message) {
                                return failNative(at, message);
                            },
                            exceptionCallScope)) {
                        return false;
                    }
                    if (ignoredResult.dynamic && ignoredResult.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                            ignoredResult.value + ")");
                    } else if (ignoredResult.ownsClass &&
                               isManagedClassValue(ignoredResult)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       ignoredResult.value + ")");
                    }
                    break;
                }

                case VMOperator::CATCH_BEGIN: {
                    int64_t slot = -1;
                    if (!tryArgInt(ins, 1, slot)) {
                        return failNative(ins, "invalid CATCH_BEGIN local slot");
                    }
                    const auto captured = exceptionValueSlots.find(pc);
                    if (captured == exceptionValueSlots.end()) {
                        return failNative(ins, "catch has no exception landing pad");
                    }
                    if (slot >= 0) {
                        const int localSlot = static_cast<int>(slot);
                        locals[localSlot] = NativeValue{
                            captured->second, "ptr", false, true, {}, true
                        };
                    } else {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                            captured->second + ")");
                    }
                    break;
                }

                case VMOperator::CATCH_END: {
                    const auto begin = catchBeginForEnd.find(pc);
                    if (begin != catchBeginForEnd.end()) {
                        const auto captured = exceptionValueSlots.find(begin->second);
                        if (captured != exceptionValueSlots.end()) {
                            int64_t slot = -1;
                            if (tryArgInt(chunk[begin->second], 1, slot) && slot >= 0) {
                                const int localSlot = static_cast<int>(slot);
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                    captured->second + ")");
                                locals.erase(localSlot);
                            }
                        }
                    }
                    break;
                }

                case VMOperator::THROW: {
                    NativeValue thrown;
                    if (!popNative(ins, thrown) || thrown.functionRef || !valueStack.empty()) {
                        return failNative(ins, "THROW requires one value on the stack");
                    }
                    std::string boxed;
                    bool ownsBoxed = false;
                    if (!emitNativeDynamicValue(ins, thrown, boxed, ownsBoxed)) return false;
                    if (isManagedClassValue(thrown) && thrown.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       thrown.value + ")");
                    }
                    emitNativeLine(std::string("call void @zeus_exception_raise") +
                        (ownsBoxed ? "_owned(ptr " : "(ptr ") + boxed + ")");
                    emitNativeLine("unreachable");
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::LLVM_SIZEOF: {
                    const std::string size =
                        llvmValueStorageSize(ins.metadata.typeName);
                    if (size.empty()) {
                        return failNative(ins,
                            "sizeof(T) requires a native LLVM value representation; type '" +
                            ins.metadata.typeName + "' is dynamically represented");
                    }
                    valueStack.push_back(NativeValue{
                        size, "i64", false, false, "int"});
                    break;
                }

                case VMOperator::LLVM_ADDR: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "invalid @addr variable locator");
                    }
                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    int64_t index = -1;
                    if (!tryArgInt(ins, 1, index) || index < 0 ||
                        index > std::numeric_limits<int>::max()) {
                        return failNative(ins, "invalid @addr variable index");
                    }

                    std::string storage;
                    std::string storageABI;
                    std::string sourceType;
                    const bool revealView = ins.args.size() > 3 &&
                        std::holds_alternative<std::string>(ins.args[3]) &&
                        std::get<std::string>(ins.args[3]) == "reveal";
                    const int slotIndex = static_cast<int>(index);
                    if (kind == "function") {
                        if (ins.args.size() < 3 ||
                            !std::holds_alternative<std::string>(ins.args[2])) {
                            return failNative(ins, "invalid @addr function locator");
                        }
                        NativeValue address;
                        std::string error;
                        if (!emitLLVMFunctionAddress(
                                std::get<std::string>(ins.args[2]), address,
                                emitNativeLine, freshNativeTmp, error)) {
                            return failNative(ins, error);
                        }
                        valueStack.push_back(std::move(address));
                        break;
                    }
                    if (kind == "global") {
                        const auto nativeViewType =
                            prescanGlobalNativeViewTypes.find(slotIndex);
                        if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                            (nativeViewType->second == "string" ||
                             nativeViewType->second == "bytes")) {
                            storage = freshNativeTmp();
                            emitNativeLine(storage +
                                " = call ptr @zeus_global_" +
                                (nativeViewType->second == "string"
                                    ? "string" : "bytes") + "_slot(i32 " +
                                std::to_string(index) + ")");
                            storageABI = "native";
                            sourceType = nativeViewType->second;
                        } else {
                            storage = freshNativeTmp();
                            emitNativeLine(storage +
                                " = call ptr @zeus_value_global_slot(i32 " +
                                std::to_string(index) + ")");
                            storageABI = "zeusvalue";
                        }
                    } else if (kind == "upvalue") {
                        storage = freshNativeTmp();
                        emitNativeLine(storage +
                            " = call ptr @zeus_closure_env_value_slot(ptr %closure.env, i32 " +
                            std::to_string(index) + ")");
                        storageABI = "zeusvalue";
                    } else if (kind == "local") {
                        const auto nativeSlot =
                            addressableNativeLocalSlots.find(slotIndex);
                        if (nativeSlot != addressableNativeLocalSlots.end()) {
                            storage = nativeSlot->second.address;
                            storageABI = "native";
                            sourceType = nativeSlot->second.sourceType;
                        } else if (referenceParameterLocals.find(slotIndex) !=
                                   referenceParameterLocals.end()) {
                            storage = "%arg" + std::to_string(index);
                            sourceType =
                                signature.parameterTypes[static_cast<size_t>(index)];
                            storageABI = usesZeusValueABI(sourceType)
                                ? "zeusvalue" : "native";
                        } else {
                            const auto captured = closureLocalCells.find(slotIndex);
                            if (captured != closureLocalCells.end()) {
                                storage = freshNativeTmp();
                                emitNativeLine(storage +
                                    " = call ptr @zeus_capture_cell_value_slot(ptr " +
                                    captured->second + ")");
                                storageABI = "zeusvalue";
                            } else {
                                const auto valueSlot = addressableLocalSlots.find(slotIndex);
                                if (valueSlot == addressableLocalSlots.end()) {
                                    return failNative(ins,
                                        "@addr local has no stable storage slot");
                                }
                                storage = valueSlot->second;
                                storageABI = "zeusvalue";
                            }
                        }
                    } else if (kind == "shared") {
                        if (ins.args.size() < 3 ||
                            !std::holds_alternative<std::string>(ins.args[2])) {
                            return failNative(ins,
                                "@addr shared locator is missing its name");
                        }
                        storage = freshNativeTmp();
                        if (revealView) {
                            emitNativeLine(storage +
                                " = call ptr @zeus_shared_value_native_address(ptr " +
                                poolStr(std::get<std::string>(ins.args[2])) + ", i32 1)");
                        } else {
                            emitNativeLine(storage +
                                " = call ptr @zeus_shared_value_slot_address(ptr " +
                                poolStr(std::get<std::string>(ins.args[2])) + ", ptr " +
                                poolStr(ins.metadata.typeName) + ", i32 " +
                                (ins.metadata.symbol.isConst ? "0" : "1") + ")");
                        }
                        storageABI = "raw";
                    } else {
                        return failNative(ins, "unsupported @addr variable scope '" +
                            kind + "'");
                    }

                    NativeValue address;
                    std::string error;
                    if (!emitLLVMAddressFromStorage(
                            storage, storageABI, sourceType, revealView, !ins.metadata.symbol.isConst, address, emitNativeLine,
                            freshNativeTmp, error)) {
                        return failNative(ins, error);
                    }
                    valueStack.push_back(std::move(address));
                    break;
                }

                case VMOperator::LLVM_FADDR: {
                    NativeValue address;
                    NativeValue stringLength;
                    std::string targetType;
                    std::string error;
                    int64_t integerByteWidth = 0;
                    if (!annotationString(ins, 0, targetType) ||
                        !llvmFaddrAccessWidth(ins, integerByteWidth, error)) {
                        return failNative(ins, error.empty()
                            ? "invalid @faddr type metadata" : error);
                    }
                    const bool rawBytesRange = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "raw_range";
                    const bool uncheckedRawFaddr =
                        ins.metadata.tag == "unchecked_raw_faddr";
                    const bool hasStringLength = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        (std::get<std::string>(ins.args[1]) == "length" ||
                         rawBytesRange);
                    const bool nativeSlot = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "slot";
                    const bool nativeSlotClear = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "slot_clear";
                    if (hasStringLength && !popNative(ins, stringLength)) {
                        return failNative(ins, "missing @faddr string length");
                    }
                    if (!popNative(ins, address)) {
                        return failNative(ins, "missing @faddr address");
                    }
                    NativeValue loaded;
                    if (!emitLLVMFetchAddress(
                            address, targetType, loaded, emitNativeLine,
                            freshNativeTmp, error,
                            hasStringLength ? &stringLength : nullptr,
                            integerByteWidth, nativeSlot, nativeSlotClear,
                            rawBytesRange, uncheckedRawFaddr)) {
                        return failNative(ins, error);
                    }
                    valueStack.push_back(std::move(loaded));
                    break;
                }

                case VMOperator::LLVM_FSTORE: {
                    NativeValue value;
                    NativeValue address;
                    NativeValue byteLength;
                    std::string targetType;
                    std::string error;
                    int64_t integerByteWidth = 0;
                    const bool rawBytesRange = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "raw_range";
                    const bool uncheckedRawFaddr =
                        ins.metadata.tag == "unchecked_raw_faddr";
                    const bool ownedBytesLength = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "length";
                    const bool hasByteRange = rawBytesRange || ownedBytesLength;
                    const bool nativeSlot = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "slot";
                    if (!annotationString(ins, 0, targetType) ||
                        !llvmFaddrAccessWidth(ins, integerByteWidth, error) ||
                        (ins.args.size() > 1 &&
                         std::holds_alternative<std::string>(ins.args[1]) &&
                         std::get<std::string>(ins.args[1]) == "slot_clear") ||
                        (ownedBytesLength && targetType != "bytes") ||
                        (rawBytesRange && targetType != "&bytes")) {
                        return failNative(ins, error.empty()
                            ? "invalid @faddr assignment metadata" : error);
                    }
                    if (!popNative(ins, value) ||
                        (hasByteRange && !popNative(ins, byteLength)) ||
                        !popNative(ins, address)) {
                        return failNative(ins, "missing @faddr assignment operands");
                    }
                    if (hasByteRange) {
                        if (!emitLLVMByteRangeStore(
                                address, byteLength, targetType, value,
                                rawBytesRange, emitNativeLine, freshNativeTmp,
                                error)) {
                            return failNative(ins, error);
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        break;
                    }
                    emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);

                    auto convertNativeValue = [&](
                        const NativeValue& source,
                        const std::string& declaredType,
                        const std::string& expectedType,
                        std::string& converted,
                        bool& sourceConsumed,
                        bool& ownsConvertedNativeString,
                        std::string& error
                    ) {
                        sourceConsumed = false;
                        ownsConvertedNativeString = false;
                        if (source.functionRef) {
                            error = "function references require a ZeusValue target slot";
                            return false;
                        }
                        if (source.dynamic) {
                            if (expectedType == "double" || expectedType == "float") {
                                converted = emitNativeFloatingValueConversion(
                                    source.value, "ptr", {}, expectedType,
                                    emitNativeLine, freshNativeTmp);
                            } else if (isIntegerLLVMType(expectedType) && (expectedType != "i64" ||
                                ZeusBuiltins::findNativeIntegerABI(declaredType))) {
                                converted = emitNativeIntegerValueConversion(
                                    source.value, "ptr", {}, expectedType, declaredType,
                                    emitNativeLine, freshNativeTmp);
                            } else if (expectedType == "i64") {
                                converted = freshNativeTmp();
                                emitNativeLine(converted +
                                    " = call i64 @zeus_value_require_i64(ptr " +
                                    source.value + ")");
                            } else if (expectedType == "i1") {
                                const std::string raw = freshNativeTmp();
                                emitNativeLine(raw +
                                    " = call i32 @zeus_value_require_bool(ptr " +
                                    source.value + ")");
                                converted = freshNativeTmp();
                                emitNativeLine(converted + " = icmp ne i32 " + raw + ", 0");
                            } else if (expectedType == "ptr" && isClassType(declaredType)) {
                                converted = freshNativeTmp();
                                emitNativeLine(converted +
                                    " = call ptr @zeus_value_" +
                                    (nativeSlot ? "try_class_ptr" : "require_class_ptr") +
                                    "(ptr " +
                                    source.value + ", ptr " + poolStr(declaredType) + ")");
                            } else if (expectedType == "ptr" &&
                                       ZeusBuiltins::isNativePointerABI(
                                           ZeusBuiltins::findNativeTypeABI(declaredType))) {
                                const auto* viewABI =
                                    ZeusBuiltins::findNativeTypeABI(declaredType);
                                converted = freshNativeTmp();
                                emitNativeLine(converted + " = call ptr @" +
                                    std::string(ZeusBuiltins::nativePointerFromDynamicHelper(viewABI)) +
                                    "(ptr " + source.value + ")");
                                ownsConvertedNativeString = viewABI != nullptr &&
                                    viewABI->valueABI ==
                                        ZeusBuiltins::NativeValueABI::NativeString;
                            } else {
                                error = "dynamic value cannot be written as native type '" +
                                    declaredType + "'";
                                return false;
                            }
                            return true;
                        }

                        const auto* declaredABI =
                            ZeusBuiltins::findNativeTypeABI(declaredType);
                        if (expectedType == "ptr" && isClassType(source.sourceType) &&
                            ZeusBuiltins::isNativePointerABI(declaredABI) &&
                            source.sourceType == "String" && declaredType == "string") {
                            const std::string boxed = freshNativeTmp();
                            emitNativeLine(boxed + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_from_class_ptr(ptr " +
                                boxed + ", ptr " + source.value + ", ptr " +
                                poolStr(source.sourceType) + ")");
                            converted = freshNativeTmp();
                            emitNativeLine(converted +
                                " = call ptr @zeus_value_to_native_string(ptr " +
                                boxed + ")");
                            ownsConvertedNativeString = true;
                            emitNativeLine("call void @zeus_value_release(ptr " + boxed + ")");
                            return true;
                        }

                        if ((expectedType == "double" || expectedType == "float") &&
                            source.sourceType != "char") {
                            converted = emitNativeFloatingValueConversion(source.value,
                                source.type, source.sourceType, expectedType,
                                emitNativeLine, freshNativeTmp);
                            if (!converted.empty()) return true;
                        }
                        if (isIntegerLLVMType(source.type) &&
                            isIntegerLLVMType(expectedType) && source.sourceType != "char") {
                            converted = emitNativeIntegerValueConversion(
                                source.value, source.type, source.sourceType,
                                expectedType, declaredType, emitNativeLine, freshNativeTmp);
                            return true;
                        }
                        if (source.type != expectedType ||
                            (isClassType(declaredType) &&
                             source.sourceType != declaredType)) {
                            error = "value type does not match @faddr target type '" +
                                declaredType + "'";
                            return false;
                        }
                        converted = source.value;
                        return true;
                    };
                    auto boxDynamicValue = [&](const NativeValue& source,
                                               std::string& boxed,
                                               bool& ownsBoxed,
                                               std::string& error) {
                        if (!emitNativeDynamicValue(ins, source, boxed, ownsBoxed)) {
                            error = "failed to create ZeusValue for @faddr assignment";
                            return false;
                        }
                        return true;
                    };
                    if (!emitLLVMStoreAddress(
                            address, targetType, value, emitNativeLine,
                            freshNativeTmp, convertNativeValue, boxDynamicValue, error,
                            integerByteWidth, nativeSlot, uncheckedRawFaddr)) {
                        return failNative(ins, error);
                    }
                    break;
                }

                case VMOperator::PUSH: {
                    NativeValue value;
                    if (!parseNativeValue(ins, value)) return false;
                    valueStack.push_back(std::move(value));
                    break;
                }

                case VMOperator::PUSH_NAMED: {
                    if (valueStack.empty() || ins.args.size() != 1 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "invalid named argument");
                    }
                    valueStack.back().namedArgument = std::get<std::string>(ins.args[0]);
                    break;
                }

                case VMOperator::LOAD_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] LOAD_VALUE_SHARED 缺少名称");
                    }
                    const std::string sharedName = std::get<std::string>(ins.args[0]);
                    const auto* sharedABI = ins.metadata.kind ==
                            IRType::IRMetadataKind::SharedBindingType
                        ? ZeusBuiltins::findNativeTypeABI(ins.metadata.typeName)
                        : nullptr;
                    if (sharedABI && sharedABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::Int64) {
                        const std::string value = freshNativeTmp();
                        emitNativeLine(value + " = call i64 @zeus_shared_value_load_i64(ptr " +
                                       poolStr(sharedName) + ")");
                        valueStack.push_back(NativeValue{
                            value, "i64", false, false, ins.metadata.typeName
                        });
                        break;
                    }
                    if (sharedABI && sharedABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::Bool1) {
                        const std::string raw = freshNativeTmp();
                        emitNativeLine(raw + " = call i32 @zeus_shared_value_load_bool(ptr " +
                                       poolStr(sharedName) + ")");
                        const std::string value = freshNativeTmp();
                        emitNativeLine(value + " = icmp ne i32 " + raw + ", 0");
                        valueStack.push_back(NativeValue{
                            value, "i1", false, false, ins.metadata.typeName
                        });
                        break;
                    }
                    const std::string value = freshNativeTmp();
                    emitNativeLine(value + " = alloca %ZeusValue, align 8");
                    emitNativeLine("call void @zeus_shared_value_load(ptr " + value +
                                   ", ptr " + poolStr(sharedName) + ")");
                    valueStack.push_back(NativeValue{
                        value, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    if (ins.args.size() == 1 &&
                        std::holds_alternative<LexerType::Value>(ins.args[0]) &&
                        ins.metadata.symbol.kind == IRType::SymbolRefKind::Class) {
                        const std::string& className = ins.metadata.symbol.name;
                        if (findClassLayout(className) == nullptr) {
                            return failNative(
                                ins, "[emit-llvm] new 使用了未知类型 '" + className + "'");
                        }
                        valueStack.push_back(NativeValue{
                            className, "classref", false, false, className
                        });
                        break;
                    }

                    if (ins.args.size() == 1 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        if (!dynamicVariadicABISymbol(name).empty()) {
                            valueStack.push_back(NativeValue{ name, "builtin", true });
                            break;
                        }
                        if (classLayouts.find(name) != classLayouts.end()) {
                            valueStack.push_back(NativeValue{
                                name, "classref", false, false, name
                            });
                            break;
                        }
                        const auto* function = resolveFunctionSignature(
                            resolveCallableName(name));
                        if (function == nullptr) {
                            function = resolveFunctionSignatureByUniqueSuffix(name);
                        }
                        if (function == nullptr) {
                            return failNative(ins,
                                "[emit-llvm] cannot resolve function name '" + name + "'");
                        }
                        valueStack.push_back(NativeValue{ function->name, "function", true });
                        break;
                    }

                    if (ins.args.size() < 2 || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] 原生函数 LOAD_VALUE 形态不支持");
                    }

                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    int64_t index = 0;
                    if (!tryArgInt(ins, 1, index)) {
                        return failNative(ins, "[emit-llvm] 原生函数 LOAD_VALUE 索引不是整数");
                    }

                    const bool borrowedReadOnlyBytesThis = kind == "local" &&
                        index == 0 && signature.ownerType == "bytes" &&
                        !signature.isStaticMethod &&
                        detail::isReadOnlyBytesMethodName(methodNameForSignature(signature));

                    if (kind == "local") {
                        const int localIndex = static_cast<int>(index);
                        const auto dynamicLocal = locals.find(localIndex);
                        if (dynamicLocal != locals.end() &&
                            dynamicLocal->second.dynamic) {
                            NativeValue clone;
                            if (!cloneNativeDynamicValue(ins, dynamicLocal->second, clone)) {
                                return false;
                            }
                            valueStack.push_back(std::move(clone));
                            break;
                        }
                        const auto addressedNative =
                            addressableNativeLocalSlots.find(localIndex);
                        if (addressedNative != addressableNativeLocalSlots.end()) {
                            const NativeSlot& slot = addressedNative->second;
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = load " + slot.type +
                                ", ptr " + slot.address +
                                (slot.type == "i1" ? ", align 1" : ", align 8"));
                            const auto* slotABI =
                                ZeusBuiltins::findNativeTypeABI(slot.sourceType);
                            if (slotABI != nullptr && slotABI->valueABI ==
                                    ZeusBuiltins::NativeValueABI::NativeBytes) {
                                if (detail::isNativeBytesDataAddressReceiver(chunk, pc) ||
                                    borrowedReadOnlyBytesThis ||
                                    isBorrowedBytesArgument(chunk, pc)) {
                                    valueStack.push_back(NativeValue{
                                        loaded, "ptr", false, false, slot.sourceType
                                    });
                                    break;
                                }
                                const std::string copy = freshNativeTmp();
                                emitNativeLine(copy +
                                    " = call ptr @zeus_bytes_clone(ptr " + loaded + ")");
                                valueStack.push_back(NativeValue{
                                    copy, "ptr", false, false, slot.sourceType
                                });
                                valueStack.back().ownsNativeBytes = true;
                                break;
                            }
                            valueStack.push_back(NativeValue{
                                loaded, slot.type, false, false, slot.sourceType
                            });
                            break;
                        }
                        if (referenceParameterLocals.find(static_cast<int>(index)) !=
                            referenceParameterLocals.end()) {
                            const std::string& parameterType =
                                signature.parameterTypes[static_cast<size_t>(index)];
                            if (!usesZeusValueABI(parameterType) &&
                                isSignedIntegerLLVMType(llvmType(parameterType))) {
                                const std::string loaded = freshNativeTmp();
                                emitNativeLine(loaded +
                                    " = call i64 @zeus_value_slot_load_i64(ptr %arg" +
                                    std::to_string(index) + ")");
                                const std::string targetType = llvmType(parameterType);
                                const std::string integerValue = emitSignedIntegerConversion(
                                    loaded, "i64", targetType, emitNativeLine, freshNativeTmp);
                                valueStack.push_back(NativeValue{
                                    integerValue, targetType, false, false, parameterType, false
                                });
                                break;
                            }
                            if (!usesZeusValueABI(parameterType) &&
                                llvmType(parameterType) == "i1") {
                                const std::string raw = freshNativeTmp();
                                emitNativeLine(raw +
                                    " = call i32 @zeus_value_slot_load_bool(ptr %arg" +
                                    std::to_string(index) + ")");
                                const std::string loaded = freshNativeTmp();
                                emitNativeLine(loaded + " = trunc i32 " + raw + " to i1");
                                valueStack.push_back(NativeValue{
                                    loaded, "i1", false, false, parameterType, false
                                });
                                break;
                            }
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_init_nil(ptr " + loaded + ")");
                            emitNativeLine("call void @zeus_value_slot_load(ptr " + loaded +
                                ", ptr %arg" + std::to_string(index) + ")");
                            valueStack.push_back(NativeValue{
                                loaded, "ptr", false, true,
                                signature.parameterTypes[static_cast<size_t>(index)], true
                            });
                            break;
                        }
                        const auto capturedCell = closureLocalCells.find(static_cast<int>(index));
                        if (capturedCell != closureLocalCells.end()) {
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_capture_cell_load(ptr " + loaded +
                                           ", ptr " + capturedCell->second + ")");
                            valueStack.push_back(NativeValue{
                                loaded, "ptr", false, true, std::string(), true
                            });
                            break;
                        }
                        auto local = locals.find(static_cast<int>(index));
                        if (local == locals.end()) {
                            return failNative(ins, "[emit-llvm] 原生函数读取了未初始化的局部变量");
                        }
                        if (local->second.dynamic) {
                            NativeValue clone;
                            if (!cloneNativeDynamicValue(ins, local->second, clone)) return false;
                            valueStack.push_back(std::move(clone));
                        } else if (local->second.type == "ptr" &&
                                   local->second.sourceType == "bytes") {
                            if (detail::isNativeBytesDataAddressReceiver(chunk, pc) ||
                                borrowedReadOnlyBytesThis) {
                                NativeValue borrowed = local->second;
                                borrowed.ownsNativeBytes = false;
                                borrowed.ownsClass = false;
                                valueStack.push_back(std::move(borrowed));
                                break;
                            }
                            const std::string copy = freshNativeTmp();
                            emitNativeLine(copy +
                                " = call ptr @zeus_bytes_clone(ptr " +
                                local->second.value + ")");
                            NativeValue cloned{
                                copy, "ptr", false, false, "bytes"
                            };
                            cloned.ownsNativeBytes = true;
                            valueStack.push_back(std::move(cloned));
                        } else {
                            NativeValue borrowed = local->second;
                            borrowed.ownsClass = false;
                            valueStack.push_back(std::move(borrowed));
                        }
                        break;
                    }

                    if (kind == "upvalue") {
                        const std::string loaded = freshNativeTmp();
                        emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_closure_env_load(ptr " + loaded +
                                       ", ptr %closure.env, i32 " +
                                       std::to_string(index) + ")");
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, true, std::string(), true
                        });
                        break;
                    }


                    if (kind == "global") {
                        const auto importedNamespace =
                            namespacePackagesByGlobal.find(static_cast<int>(index));
                        if (importedNamespace != namespacePackagesByGlobal.end()) {
                            valueStack.push_back(NativeValue{
                                importedNamespace->second,
                                "namespace",
                                false,
                                false,
                                importedNamespace->second
                            });
                            break;
                        }
                    }

                    if (kind == "global" && isFunctionGlobal(static_cast<int>(index))) {
                        valueStack.push_back(NativeValue{
                            functionGlobals[static_cast<int>(index)],
                            "function",
                            true
                        });
                        break;
                    }

                    if (kind == "global") {
                        const auto declaredDouble = nativeGlobalDeclaredDoubleTypes.find(static_cast<int>(index));
                        if (declaredDouble != nativeGlobalDeclaredDoubleTypes.end()) {
                            const auto loaded = freshNativeTmp();
                            const std::string nativeType = llvmType(declaredDouble->second);
                            const bool isFloat = nativeType == "float";
                            emitNativeLine(loaded + " = call " + nativeType + " @zeus_global_load_" +
                                (isFloat ? "float" : "double") + "(i32 " +
                                std::to_string(index) + ")");
                            valueStack.push_back(NativeValue{
                                loaded, nativeType, false, false, declaredDouble->second});
                            break;
                        }
                        const auto declaredInteger = nativeGlobalDeclaredIntegerTypes.find(
                            static_cast<int>(index));
                        if (declaredInteger != nativeGlobalDeclaredIntegerTypes.end()) {
                            const auto* unsignedABI = ZeusBuiltins::findNativeUnsignedIntegerABI(
                                declaredInteger->second);
                            const std::string raw = freshNativeTmp();
                            if (unsignedABI) {
                                emitNativeLine(raw + " = call i64 @zeus_global_load_unsigned_integer(i32 " +
                                    std::to_string(index) + ", i32 " +
                                    std::to_string(unsignedABI->integerBits) + ")");
                            } else {
                                emitNativeLine(raw + " = call i64 @zeus_global_load_i64(i32 " +
                                    std::to_string(index) + ")");
                            }
                            const std::string targetType = llvmType(declaredInteger->second);
                            const std::string rawType = unsignedABI
                                ? "@native.uint64" : "@native.longlong";
                            const std::string nativeValue = emitNativeIntegerValueConversion(
                                raw, "i64", rawType, targetType, declaredInteger->second,
                                emitNativeLine, freshNativeTmp);
                            valueStack.push_back(NativeValue{nativeValue, targetType,
                                false, false, declaredInteger->second});
                            break;
                        }
                        auto classGlobal = classGlobals.find(static_cast<int>(index));
                        if (classGlobal != classGlobals.end()) {
                            valueStack.push_back(NativeValue{
                                classGlobal->second,
                                "classref",
                                false,
                                false,
                                classGlobal->second
                            });
                            break;
                        }

                        // A global known to hold a class instance is unboxed back
                        // to a native pointer, so field access resolves against
                        // the class layout instead of failing on a boxed value.
                        const auto globalClass =
                            prescanGlobalClassTypes.find(static_cast<int>(index));
                        if (globalClass != prescanGlobalClassTypes.end() &&
                            findClassLayout(globalClass->second) != nullptr) {
                            const std::string boxed = freshNativeTmp();
                            emitNativeLine(boxed + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_global_load(ptr " +
                                boxed + ", i32 " + std::to_string(index) + ")");
                            const std::string unboxed = freshNativeTmp();
                            emitNativeLine(unboxed +
                                " = call ptr @zeus_value_require_class_ptr(ptr " +
                                boxed + ", ptr " + poolStr(globalClass->second) + ")");
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           unboxed + ")");
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                boxed + ")");
                            NativeValue loadedClass{
                                unboxed, "ptr", false, false, globalClass->second
                            };
                            loadedClass.ownsClass = true;
                            valueStack.push_back(std::move(loadedClass));
                            break;
                        }

                        const auto globalScalar =
                            nativeGlobalScalarTypes.find(static_cast<int>(index));
                        if (globalScalar != nativeGlobalScalarTypes.end()) {
                            // Unboxed access: a loop that bumps a scalar global
                            // must not build a ZeusValue per iteration.
                            const bool globalIsBool = globalScalar->second == "i1";
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = call " +
                                std::string(globalIsBool ? "i32" : "i64") +
                                " @zeus_global_load_" +
                                (globalIsBool ? "bool" : "i64") + "(i32 " +
                                std::to_string(index) + ")");
                            if (globalIsBool) {
                                const std::string flag = freshNativeTmp();
                                emitNativeLine(flag + " = icmp ne i32 " + loaded + ", 0");
                                valueStack.push_back(NativeValue{ flag, "i1", false, false });
                            } else {
                                const auto declaredInteger = nativeGlobalDeclaredIntegerTypes.find(
                                    static_cast<int>(index));
                                if (declaredInteger != nativeGlobalDeclaredIntegerTypes.end()) {
                                    const std::string targetType = llvmType(declaredInteger->second);
                                    const auto* unsignedABI =
                                        ZeusBuiltins::findNativeUnsignedIntegerABI(declaredInteger->second);
                                    std::string integerValue;
                                    if (unsignedABI) {
                                        integerValue = emitNativeIntegerValueConversion(
                                            loaded, "i64", "@native.uint64", targetType,
                                            declaredInteger->second, emitNativeLine, freshNativeTmp);
                                    } else {
                                        integerValue = emitNativeIntegerValueConversion(
                                            loaded, "i64", "@native.longlong", targetType,
                                            declaredInteger->second, emitNativeLine, freshNativeTmp);
                                    }
                                    valueStack.push_back(NativeValue{integerValue, targetType,
                                        false, false, declaredInteger->second});
                                } else {
                                    valueStack.push_back(NativeValue{ loaded, "i64", false, false });
                                }
                            }
                            break;
                        }

                    const auto nativeStringGlobalType =
                        prescanGlobalNativeViewTypes.find(static_cast<int>(index));
                    if (nativeStringGlobalType != prescanGlobalNativeViewTypes.end() &&
                        nativeStringGlobalType->second == "string") {
                        const std::string loaded = freshNativeTmp();
                        emitNativeLine(loaded +
                            " = call ptr @zeus_global_load_string(i32 " +
                            std::to_string(index) + ")");
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, false, "string"
                        });
                        valueStack.back().ownsNativeString = true;
                        break;
                    }
                    if (nativeStringGlobalType != prescanGlobalNativeViewTypes.end() &&
                        nativeStringGlobalType->second == "bytes") {
                        if (detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                            const std::string slot = freshNativeTmp();
                            emitNativeLine(slot +
                                " = call ptr @zeus_global_bytes_slot(i32 " +
                                std::to_string(index) + ")");
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = load ptr, ptr " + slot);
                            valueStack.push_back(NativeValue{
                                loaded, "ptr", false, false, "bytes"
                            });
                            break;
                        }
                        const std::string loaded = freshNativeTmp();
                        emitNativeLine(loaded +
                            " = call ptr @zeus_global_load_bytes(i32 " +
                            std::to_string(index) + ")");
                        NativeValue bytesValue{
                            loaded, "ptr", false, false, "bytes"
                        };
                        bytesValue.ownsNativeBytes = true;
                        valueStack.push_back(std::move(bytesValue));
                        break;
                    }

                        const std::string value = emitSlotLoad(
                            static_cast<int>(index), true,
                            [&](const std::string& line) { emitNativeLine(line); });
                        if (index >= 0 && static_cast<size_t>(index) < globalNames.size()) {
                            nativeGlobalValueOrigins[value] =
                                globalNames[static_cast<size_t>(index)];
                        }
                        std::string sourceType;
                        const auto nativeViewType =
                            prescanGlobalNativeViewTypes.find(static_cast<int>(index));
                        if (nativeViewType != prescanGlobalNativeViewTypes.end()) {
                            sourceType = nativeViewType->second;
                        }
                        valueStack.push_back(NativeValue{
                            value, "ptr", false, true, sourceType, true
                        });
                        break;
                    }

                    return failNative(ins, "[emit-llvm] 原生函数只支持局部值和直接函数引用");
                }

                case VMOperator::LIST_BUILD:
                case VMOperator::DICT_BUILD: {
                    int64_t count = 0;
                    if (!tryArgInt(ins, 0, count) || count < 0 ||
                        count > std::numeric_limits<int32_t>::max()) {
                        return failNative(ins, "[emit-llvm] 容器构造元素数量无效");
                    }
                    const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                    const int64_t valueCount = dictionary ? count * 2 : count;
                    std::vector<NativeValue> values(
                        static_cast<std::size_t>(valueCount));
                    for (int64_t i = valueCount - 1; i >= 0; --i) {
                        if (!popNative(ins, values[static_cast<std::size_t>(i)])) {
                            return false;
                        }
                    }

                    std::vector<std::string> dynamicValues;
                    std::vector<bool> ownsDynamicValues;
                    dynamicValues.reserve(values.size());
                    ownsDynamicValues.reserve(values.size());
                    for (const auto& value : values) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (value.functionRef ||
                            !emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return failNative(ins,
                                "[emit-llvm] 容器元素不能转换为 ZeusValue");
                        }
                        dynamicValues.push_back(dynamicValue);
                        ownsDynamicValues.push_back(ownsDynamic);
                    }

                    const std::string resultValue = ContainerABI::emitBuildCall(
                        dictionary, count, dynamicValues,
                        [&]() { return freshNativeTmp(); },
                        [&](const std::string& line) { emitNativeLine(line); });
                    for (std::size_t i = 0; i < dynamicValues.size(); ++i) {
                        if (ownsDynamicValues[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValues[i] + ")");
                        }
                    }
                    for (const auto& value : values) {
                        if (isManagedClassValue(value) && value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LENGTH:
                case VMOperator::DICT_KEYS_BUILD: {
                    NativeValue container;
                    if (!popNative(ins, container) || container.functionRef) {
                        return failNative(ins, "[emit-llvm] 容器操作需要动态值");
                    }
                    std::string dynamicContainer;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(
                            ins, container, dynamicContainer, ownsDynamic)) {
                        return false;
                    }
                    const std::string symbol = ins.op == VMOperator::LIST_LENGTH
                        ? "@zeus_value_array_length"
                        : "@zeus_value_dict_keys";
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, { dynamicContainer },
                        [&]() { return freshNativeTmp(); },
                        [&](const std::string& line) { emitNativeLine(line); });
                    if (ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       dynamicContainer + ")");
                    }
                    if (isManagedClassValue(container) && container.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       container.value + ")");
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE: {
                    const bool isStore = ins.op == VMOperator::LIST_STORE_ELEM ||
                        ins.op == VMOperator::DICT_STORE_VALUE;
                    const bool isList = ins.op == VMOperator::LIST_LOAD_ELEM ||
                        ins.op == VMOperator::LIST_STORE_ELEM;
                    NativeValue storedValue;
                    NativeValue keyOrIndex;
                    NativeValue container;
                    if ((isStore && !popNative(ins, storedValue)) ||
                        !popNative(ins, keyOrIndex) ||
                        !popNative(ins, container) || storedValue.functionRef ||
                        keyOrIndex.functionRef || container.functionRef) {
                        return failNative(ins, "[emit-llvm] 容器索引操作的栈值无效");
                    }

                    const bool hasRawBytesRangeIndexMetadata =
                        ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexLoad ||
                        ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexStore;
                    if (hasRawBytesRangeIndexMetadata) {
                        const bool metadataMatchesOperation = !isList &&
                            (isStore
                                ? ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexStore
                                : ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexLoad);
                        if (!metadataMatchesOperation ||
                            (!container.sourceType.empty() && container.sourceType != "&bytes")) {
                            return failNative(ins,
                                "[emit-llvm] raw bytes range index metadata does not match its container");
                        }
                        container.sourceType = "&bytes";
                    }

                    if (container.sourceType == "&bytes") {
                        std::string address;
                        std::string length;
                        std::string rangeError;
                        if (!emitLLVMRawBytesRangeParts(
                                container,
                                [&](const std::string& line) { emitNativeLine(line); },
                                [&]() { return freshNativeTmp(); },
                                address, length, rangeError)) {
                            return failNative(ins, rangeError);
                        }
                        std::string index;
                        bool ignoredOwnership = false;
                        if (!emitFunctionABIArgument(
                                ins, keyOrIndex, "int", index,
                                ignoredOwnership, false)) {
                            return false;
                        }
                        if (isStore) {
                            std::string element;
                            if (!emitFunctionABIArgument(
                                    ins, storedValue, "int", element,
                                    ignoredOwnership, false)) {
                                return false;
                            }
                            emitNativeLine("call void @zeus_raw_bytes_range_set(i64 " +
                                address + ", i64 " + length + ", i64 " + index +
                                ", i64 " + element + ")");
                        } else {
                            const std::string element = freshNativeTmp();
                            emitNativeLine(element +
                                " = call i64 @zeus_raw_bytes_range_get(i64 " +
                                address + ", i64 " + length + ", i64 " + index + ")");
                            valueStack.push_back(NativeValue{
                                element, "i64", false, false, "int"
                            });
                        }
                        if (container.dynamic && container.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           container.value + ")");
                        }
                        if (container.ownsNativeBytes) {
                            releaseNativeValueWithContext(container);
                        }
                        if (isStore) {
                            valueStack.push_back(std::move(storedValue));
                        }
                        break;
                    }

                    const size_t parameterCount = isStore ? 3 : 2;
                    const std::string operatorSymbol = isStore ? "[]=" : "[]";
                    const bool isArrayType = container.sourceType == "Array" ||
                        container.sourceType == "list" ||
                        container.sourceType == "array";
                    const bool isArrayValue = isList || isArrayType;
                    const bool isBuiltinContainer = isArrayType ||
                        container.sourceType == "dict";
                    const auto* indexABI =
                        ZeusBuiltins::findNativeTypeABI(keyOrIndex.sourceType);
                    const bool nativeI64ArrayIndex = isArrayValue &&
                        !keyOrIndex.dynamic && keyOrIndex.type == "i64" &&
                        indexABI != nullptr &&
                        indexABI->valueABI == ZeusBuiltins::NativeValueABI::Int64;
                    const FunctionSignature* operatorSignature = nullptr;
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        std::string targetName;
                        if (!ins.metadata.readDirectCall(targetName)) {
                            return failNative(ins,
                                "[emit-llvm] 下标运算标注缺少函数名");
                        }
                        const auto target = functionSignatures.find(targetName);
                        if (target == functionSignatures.end() ||
                            target->second.operatorSymbol != operatorSymbol ||
                            target->second.parameterTypes.size() != parameterCount) {
                            return failNative(ins,
                                "[emit-llvm] 下标运算标注与目标签名不匹配");
                        }
                        operatorSignature = &target->second;
                        container.sourceType = operatorSignature->ownerType;
                    } else if (!container.sourceType.empty() &&
                               (isClassType(container.sourceType) ||
                                ZeusBuiltins::findNativeTypeABI(
                                    container.sourceType) != nullptr)) {
                        operatorSignature = findClassOperatorSignature(
                            container.sourceType, operatorSymbol, parameterCount);
                        if (operatorSignature == nullptr && !isBuiltinContainer) {
                            return failNative(ins,
                                "[emit-llvm] type 没有匹配的下标operator");
                        }
                    }

                    if (!isList && operatorSignature != nullptr) {
                        const std::string qualifiedPrefix =
                            operatorSignature->ownerType + "::";
                        const std::string methodName = operatorSignature->name.substr(
                            qualifiedPrefix.size());
                        std::vector<NativeValue> operatorArguments{
                            container, keyOrIndex
                        };
                        if (isStore) operatorArguments.push_back(storedValue);
                        auto convertArgument = [&] (
                            const NativeValue& value,
                            const std::string& declaredType,
                            std::string& nativeArgument,
                            std::string& cleanupValue
                        ) {
                            bool ownsDynamic = false;
                            bool ownsNativeString = false;
                            if (!emitFunctionABIArgument(
                                    ins, value, declaredType,
                                    nativeArgument, ownsDynamic, true,
                                    &ownsNativeString)) {
                                return false;
                            }
                            cleanupValue = ownsDynamic
                                ? nativeArgument
                                : ownsNativeString
                                    ? detail::nativeStringCleanupToken(nativeArgument)
                                    : (declaredType == "bytes" && value.dynamic
                                        ? detail::nativeBytesCleanupToken(nativeArgument)
                                        : std::string());
                            return true;
                        };
                        NativeValue operatorResult;
                        if (!emitKnownClassMethodCall(
                                ins, methodName, operatorArguments, operatorResult,
                                [&](const std::string& line) { emitNativeLine(line); },
                                [&]() { return freshNativeTmp(); },
                                convertArgument,
                                [&](const std::string& value) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value + ")");
                                },
                                [&](const IRType::IRInstr& at,
                                    const std::string& message) {
                                    return failNative(at, message);
                                },
                                exceptionCallScope)) {
                            return false;
                        }
                        valueStack.push_back(std::move(operatorResult));
                        break;
                    }

                    NativeValue operands[] = { container, keyOrIndex, storedValue };
                    std::string dynamicOperands[3];
                    bool ownsDynamicOperands[3] = { false, false, false };
                    const std::size_t operandCount = isStore ? 3 : 2;
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (nativeI64ArrayIndex && i == 1) continue;
                        if (!emitNativeDynamicValue(
                                ins, operands[i], dynamicOperands[i],
                                ownsDynamicOperands[i])) {
                            return false;
                        }
                    }

                    std::string symbol;
                    std::vector<std::string> callArguments;
                    if (nativeI64ArrayIndex && !isStore) {
                        symbol = ins.op == VMOperator::DICT_LOAD_VALUE
                            ? "@zeus_value_array_load_i64_checked"
                            : "@zeus_value_array_load_i64";
                    } else if (nativeI64ArrayIndex) {
                        symbol = "@zeus_value_array_store_i64";
                    } else if (isArrayValue && !isStore) {
                        symbol = ins.op == VMOperator::DICT_LOAD_VALUE
                            ? "@zeus_value_array_load_checked"
                            : "@zeus_value_array_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else if (isArrayValue) {
                        symbol = "@zeus_value_array_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    } else if (!isStore) {
                        symbol = "@zeus_value_subscript_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else {
                        symbol = "@zeus_value_subscript_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    }

                    const auto freshTemporary = [&]() { return freshNativeTmp(); };
                    const auto emitLine = [&](const std::string& line) {
                        emitNativeLine(line);
                    };
                    const std::string resultValue = nativeI64ArrayIndex
                        ? ContainerABI::emitArrayIndexCall(
                              symbol, dynamicOperands[0], keyOrIndex.value,
                              isStore, isStore ? dynamicOperands[2] : std::string(),
                              freshTemporary, emitLine)
                        : ContainerABI::emitResultCall(
                              symbol, callArguments, freshTemporary, emitLine);
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (ownsDynamicOperands[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicOperands[i] + ")");
                        }
                        if (isManagedClassValue(operands[i]) &&
                            operands[i].ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           operands[i].value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::OBJ_BUILD: {
                    int64_t argc = 0;
                    const std::string annotatedTarget = annotatedConstructionTarget(ins);
                    const std::string nativeTargetType = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1])
                            ? std::get<std::string>(ins.args[1])
                            : std::string();
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc) +
                            (nativeTargetType.empty() ? 1u : 0u)) {
                        return failNative(ins, "[emit-llvm] class 构造参数数量无效");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue classReference;
                    bool dynamicClassReference = false;
                    if (nativeTargetType.empty()) {
                        if (!popNative(ins, classReference)) return false;
                        dynamicClassReference = classReference.dynamic;
                        if (!dynamicClassReference && classReference.type != "classref" &&
                            annotatedTarget.empty()) {
                            const auto origin = nativeGlobalValueOrigins.find(
                                classReference.value);
                            return failNative(ins,
                                "[emit-llvm] new 目标不是静态 class 名称 (value type=" +
                                classReference.type + ", dynamic=" +
                                (classReference.dynamic ? "true" : "false") +
                                ", sourceType='" + classReference.sourceType +
                                "', global='" + (origin == nativeGlobalValueOrigins.end()
                                    ? std::string("<unknown>") : origin->second) + "')");
                        }
                    }
                    dynamicClassReference = dynamicClassReference && annotatedTarget.empty();
                    std::string constructionClassName = dynamicClassReference
                        ? std::string() : classReference.sourceType;
                    if (!annotatedTarget.empty()) constructionClassName = annotatedTarget;
                    std::string genericClassName;
                    std::vector<std::string> genericTypeArguments;
                    std::string specializedClassName;
                    if (!dynamicClassReference && ins.metadata.readClassInstantiation(
                            genericClassName, genericTypeArguments,
                            &specializedClassName) &&
                        !specializedClassName.empty()) {
                        constructionClassName = specializedClassName;
                    }

                    NativeValue instance;
                    auto emitConstructionLine = [&](const std::string& line) {
                        emitNativeLine(line);
                    };
                    auto emitConstructionLabel = [&](const std::string& label) {
                        functionText += label + ":\n";
                        // The null-check split makes this label the block that
                        // falls through. Phi nodes must name the real LLVM
                        // predecessor, not the logical IR block we started in.
                        currentNativeBlock = "%" + label;
                    };
                    auto freshConstructionTemporary = [&]() {
                        return freshNativeTmp();
                    };
                    exceptionCallArguments = arguments;
                    exceptionCallDynamicValues.clear();
                    exceptionCallScope(true, exceptionCallArguments,
                                       exceptionCallDynamicValues);
                    auto convertConstructorArgument = [&] (
                        const NativeValue& argument,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        bool& ownsDynamic,
                        bool& ownsNativeString
                    ) {
                        ownsNativeString = false;
                        const bool converted = emitFunctionABIArgument(
                            ins, argument, declaredType, nativeArgument, ownsDynamic, true,
                            &ownsNativeString);
                        if (converted && ownsDynamic &&
                            (!argument.dynamic || nativeArgument != argument.value)) {
                            exceptionCallDynamicValues.push_back(nativeArgument);
                        }
                        if (converted && ownsNativeString) {
                            exceptionCallDynamicValues.push_back(
                                detail::nativeStringCleanupToken(nativeArgument));
                        }
                        if (converted && (ownsDynamic || ownsNativeString)) {
                            exceptionCallScope(true, exceptionCallArguments,
                                               exceptionCallDynamicValues);
                        }
                        return converted;
                    };
                    auto failConstruction = [&] (
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        return failNative(at, message);
                    };
                    const bool constructionSucceeded = nativeTargetType.empty()
                        ? (dynamicClassReference
                            ? emitDynamicClassConstruction(
                                ins,
                                classReference,
                                arguments,
                                instance,
                                emitConstructionLine,
                                emitConstructionLabel,
                                freshConstructionTemporary,
                                convertConstructorArgument,
                                failConstruction)
                            : emitClassConstruction(
                            ins,
                            constructionClassName,
                            arguments,
                            instance,
                            emitConstructionLine,
                            emitConstructionLabel,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction))
                        : emitNewOperatorConstruction(
                            ins,
                            nativeTargetType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction);
                    if (!constructionSucceeded) {
                        return false;
                    }
                    exceptionCallScope(false, {}, {});

                    valueStack.push_back(std::move(instance));
                    break;
                }

                case VMOperator::MAKE_FUNCTION: {
                    int64_t childChunk = 0;
                    if (!tryArgInt(ins, 0, childChunk) || childChunk < 0) {
                        return failNative(ins, "[emit-llvm] MAKE_FUNCTION has an invalid chunk index");
                    }
                    const auto childSignature = functionSignaturesByChunk.find(
                        static_cast<size_t>(childChunk));
                    if (childSignature == functionSignaturesByChunk.end()) {
                        return failNative(ins, "[emit-llvm] MAKE_FUNCTION has no function signature");
                    }
                    const auto captures = captureDescriptors(static_cast<size_t>(childChunk));
                    if (childSignature->second.captureCount != static_cast<int>(captures.size())) {
                        return failNative(ins, "[emit-llvm] closure capture metadata count mismatch");
                    }
                    for (const auto& capture : captures) {
                        if (capture.sourceKind == "local" &&
                            referenceParameterLocals.find(capture.sourceIndex) !=
                                referenceParameterLocals.end()) {
                            return failNative(ins,
                                "[emit-llvm] a by-reference parameter cannot escape through a closure");
                        }
                    }

                    std::vector<std::string> cells;
                    std::vector<std::string> temporaryCells;
                    cells.reserve(captures.size());
                    for (const auto& capture : captures) {
                        if (capture.sourceIndex < 0) {
                            return failNative(ins, "[emit-llvm] closure capture has an invalid source slot");
                        }
                        if (capture.sourceKind == "upvalue") {
                            const std::string cell = freshNativeTmp();
                            emitNativeLine(cell + " = call ptr @zeus_closure_capture_" +
                                (capture.byReference ? "ref" : "copy") +
                                "(ptr %closure.env, i32 " + std::to_string(capture.sourceIndex) + ")");
                            cells.push_back(cell);
                            temporaryCells.push_back(cell);
                            continue;
                        }
                        if (capture.sourceKind != "local") {
                            return failNative(ins, "[emit-llvm] closure capture source kind is unsupported");
                        }
                        const auto parentCell = closureLocalCells.find(capture.sourceIndex);
                        if (capture.byReference) {
                            if (parentCell == closureLocalCells.end()) {
                                return failNative(ins, "[emit-llvm] reference capture source was not cellified");
                            }
                            cells.push_back(parentCell->second);
                            continue;
                        }

                        std::string cell;
                        if (parentCell != closureLocalCells.end()) {
                            cell = freshNativeTmp();
                            emitNativeLine(cell + " = call ptr @zeus_capture_cell_clone(ptr " +
                                           parentCell->second + ")");
                        } else {
                            const auto source = locals.find(capture.sourceIndex);
                            if (source == locals.end()) {
                                return failNative(ins, "[emit-llvm] copy capture source local is not initialized");
                            }
                            std::string dynamicSource;
                            bool ownsDynamicSource = false;
                            if (source->second.dynamic &&
                                source->second.type == "ptr") {
                                // Copy capture clones this local into a new cell;
                                // the local remains live and must not be consumed.
                                dynamicSource = source->second.value;
                            } else if (!emitNativeDynamicValue(ins, source->second,
                                           dynamicSource, ownsDynamicSource)) {
                                return false;
                            }
                            cell = freshNativeTmp();
                            emitNativeLine(cell + " = call ptr @zeus_capture_cell_create(ptr " +
                                           dynamicSource + ")");
                            if (ownsDynamicSource) {
                                emitNativeLine("call void @zeus_value_release(ptr " + dynamicSource + ")");
                            }
                        }
                        cells.push_back(cell);
                        temporaryCells.push_back(cell);
                    }

                    std::string cellsArray = "null";
                    if (!cells.empty()) {
                        cellsArray = freshNativeTmp();
                        emitNativeLine(cellsArray + " = alloca ptr, i64 " +
                                       std::to_string(cells.size()) + ", align 8");
                        for (size_t i = 0; i < cells.size(); ++i) {
                            const std::string slot = freshNativeTmp();
                            emitNativeLine(slot + " = getelementptr inbounds ptr, ptr " +
                                           cellsArray + ", i64 " + std::to_string(i));
                            emitNativeLine("store ptr " + cells[i] + ", ptr " + slot + ", align 8");
                        }
                    }

                    const std::string closure = freshNativeTmp();
                    emitNativeLine(closure + " = alloca %ZeusValue, align 8");
                    if (childSignature->second.hasDefaultParameters() ||
                        childSignature->second.hasVariadicArgs ||
                        childSignature->second.hasVariadicKwargs) {
                        emitNativeLine("call void @zeus_closure_create_range(ptr " + closure +
                            ", ptr " + closureAdapterSymbol(static_cast<size_t>(childChunk)) +
                            ", i32 " + std::to_string(childSignature->second.minRequiredParams) +
                            ", i32 " + std::to_string(
                                childSignature->second.hasVariadicArgs ||
                                childSignature->second.hasVariadicKwargs
                                    ? std::numeric_limits<int32_t>::max()
                                    : static_cast<int32_t>(childSignature->second.parameterTypes.size())) +
                            ", i32 " + std::to_string(cells.size()) + ", ptr " + cellsArray + ")");
                    } else {
                        emitNativeLine("call void @zeus_closure_create(ptr " + closure +
                            ", ptr " + closureAdapterSymbol(static_cast<size_t>(childChunk)) +
                            ", i32 " + std::to_string(childSignature->second.parameterTypes.size()) +
                            ", i32 " + std::to_string(cells.size()) + ", ptr " + cellsArray + ")");
                    }
                    for (const auto& cell : temporaryCells) {
                        emitNativeLine("call void @zeus_capture_cell_release(ptr " + cell + ")");
                    }
                    valueStack.push_back(NativeValue{
                        closure, "ptr", false, true, std::string(), true
                    });
                    break;
                }
                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index)) {
                        return failNative(ins, "[emit-llvm] 原生函数局部变量存储失败: 槽位编号无效");
                    }
                    if (!popNative(ins, value)) {
                        return failNative(ins, "[emit-llvm] 原生函数局部变量存储失败: 临时值栈下溢 (stack_size=" + std::to_string(valueStack.size()) + ")");
                    }
                    const auto bindingType = nativeLocalBindingTypes.find(
                        static_cast<int>(index));
                    if (bindingType != nativeLocalBindingTypes.end() &&
                        (llvmType(bindingType->second) == "double" ||
                         llvmType(bindingType->second) == "float") && !value.dynamic &&
                        value.sourceType != "char" &&
                        (isIntegerLLVMType(value.type) || value.type == "float" ||
                         value.type == "double")) {
                        const std::string targetType = llvmType(bindingType->second);
                        value.value = emitNativeFloatingValueConversion(value.value,
                            value.type, value.sourceType, targetType,
                            emitNativeLine, freshNativeTmp);
                        value.type = targetType;
                        value.sourceType = bindingType->second;
                    }
                    if (bindingType != nativeLocalBindingTypes.end() &&
                        !value.dynamic && value.sourceType != "char" &&
                        isIntegerLLVMType(value.type) &&
                        isIntegerLLVMType(llvmType(bindingType->second))) {
                        const std::string targetType = llvmType(bindingType->second);
                        value.value = emitNativeIntegerValueConversion(value.value,
                            value.type, value.sourceType, targetType, bindingType->second,
                            emitNativeLine, freshNativeTmp);
                        value.type = targetType;
                        value.sourceType = bindingType->second;
                    }
                    if (bindingType != nativeLocalBindingTypes.end() &&
                        value.dynamic && !usesZeusValueABI(bindingType->second)) {
                        const NativeValue dynamicSource = value;
                        const auto* targetABI = ZeusBuiltins::findNativeTypeABI(
                            bindingType->second);
                        std::string nativeValue;
                        bool ownsDynamicArgument = false;
                        bool ownsNativeStringArgument = false;
                        if (!emitFunctionABIArgumentWithContext(
                                ins, value, bindingType->second, nativeValue,
                                ownsDynamicArgument, false, nullptr, 0,
                                &ownsNativeStringArgument)) {
                            return false;
                        }
                        value = NativeValue{
                            nativeValue,
                            functionParameterABIType(bindingType->second),
                            false,
                            false,
                            bindingType->second
                        };
                        if (targetABI != nullptr && targetABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::NativeBytes) {
                            value.ownsNativeBytes = true;
                        }
                        if (targetABI != nullptr && targetABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::NativeString) {
                            value.ownsNativeString = ownsNativeStringArgument;
                        }
                        if (isClassType(bindingType->second)) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           nativeValue + ")");
                            if (dynamicSource.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               dynamicSource.value + ")");
                            }
                            value.ownsClass = true;
                        }
                    } else if (bindingType != nativeLocalBindingTypes.end() &&
                               !value.dynamic &&
                               usesZeusValueABI(bindingType->second)) {
                        const NativeValue nativeSource = value;
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        if (nativeSource.ownsNativeBytes) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           nativeSource.value + ")");
                        }
                        if (nativeSource.ownsClass &&
                            isManagedClassValue(nativeSource)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           nativeSource.value + ")");
                        }
                        value = NativeValue{
                            dynamicValue, "ptr", false, true,
                            bindingType->second, ownsDynamic
                        };
                    }
                    emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);
                    if (referenceParameterLocals.find(static_cast<int>(index)) !=
                        referenceParameterLocals.end()) {
                        if (!value.dynamic && isSignedIntegerLLVMType(value.type) &&
                            value.sourceType != "char") {
                            const std::string integerValue = emitSignedIntegerConversion(
                                value.value, value.type, "i64", emitNativeLine, freshNativeTmp);
                            emitNativeLine("call void @zeus_value_slot_store_i64(ptr %arg" +
                                std::to_string(index) + ", i64 " + integerValue + ")");
                            if (value.ownsClass && isManagedClassValue(value)) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               value.value + ")");
                            }
                            break;
                        }
                        if (!value.dynamic && value.type == "i1") {
                            const std::string raw = freshNativeTmp();
                            emitNativeLine(raw + " = zext i1 " + value.value + " to i32");
                            emitNativeLine("call void @zeus_value_slot_store_bool(ptr %arg" +
                                std::to_string(index) + ", i32 " + raw + ")");
                            if (value.ownsClass && isManagedClassValue(value)) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               value.value + ")");
                            }
                            break;
                        }
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_slot_store(ptr %arg" +
                            std::to_string(index) + ", ptr " + dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        break;
                    }
                    const auto capturedCell = closureLocalCells.find(static_cast<int>(index));
                    if (capturedCell != closureLocalCells.end()) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) return false;
                        emitNativeLine("call void @zeus_capture_cell_store(ptr " +
                                       capturedCell->second + ", ptr " + dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        break;
                    }
                    const auto addressableSlot =
                        addressableLocalSlots.find(static_cast<int>(index));
                    if (addressableSlot != addressableLocalSlots.end()) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       addressableSlot->second + ")");
                        emitNativeLine("call void @zeus_value_clone(ptr " +
                                       addressableSlot->second + ", ptr " +
                                       dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        locals[static_cast<int>(index)] = NativeValue{
                            addressableSlot->second, "ptr", false, true,
                            std::string(), true
                        };
                        break;
                    }
                    // Keep known static function references native in ordinary
                    // local slots; box them only at a dynamic ZeusValue boundary.
                    if (value.functionRef) {
                        const bool isParameter = index >= 0 &&
                            index < static_cast<int64_t>(signature.parameterTypes.size());
                        if (isParameter) {
                            if (!usesZeusValueABI(
                                    signature.parameterTypes[static_cast<size_t>(index)])) {
                                return failNative(ins,
                                    "[emit-llvm] a function value cannot be stored in a native-typed parameter slot");
                            }
                            std::string dynamicValue;
                            bool ownsDynamic = false;
                            if (!emitNativeDynamicValue(
                                    ins, value, dynamicValue, ownsDynamic)) {
                                return false;
                            }
                            value = NativeValue{
                                dynamicValue, "ptr", false, true,
                                std::string(), ownsDynamic
                            };
                        }
                    }
                    if (index >= 0 &&
                        index < static_cast<int64_t>(signature.parameterTypes.size())) {
                        const std::string& declaredType =
                            signature.parameterTypes[static_cast<size_t>(index)];
                        const std::string addressStorageABI =
                            value.addressStorageABI;
                        std::string nativeValue;
                        bool ownsDynamic = false;
                        bool ownsNativeBytes = false;
                        bool ownsNativeString = false;
                        if (declaredType == "bytes") {
                            if (value.dynamic) {
                                nativeValue = freshNativeTmp();
                                emitNativeLine(nativeValue +
                                    " = call ptr @zeus_value_copy_bytes(ptr " +
                                    value.value + ")");
                                ownsNativeBytes = true;
                                if (value.ownsDynamic) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value.value + ")");
                                }
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "bytes") {
                                nativeValue = value.value;
                                ownsNativeBytes = value.ownsNativeBytes;
                            } else {
                                return failNative(ins,
                                    "typed bytes local requires a bytes value");
                            }
                        } else if (!emitFunctionABIArgument(
                                       ins, value, declaredType, nativeValue,
                                       ownsDynamic, false,
                                       &ownsNativeString)) {
                            return false;
                        }
                        const bool acquiredClassOwnership =
                            isClassType(declaredType) && value.dynamic;
                        const bool transferredClassOwnership =
                            value.ownsClass && isManagedClassValue(value) &&
                            !value.dynamic && nativeValue == value.value &&
                            isClassType(declaredType) &&
                            value.sourceType == declaredType;
                        const bool transferredNativeString =
                            value.ownsNativeString && !value.dynamic &&
                            value.sourceType == "string" &&
                            nativeValue == value.value;
                        if (acquiredClassOwnership) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           nativeValue + ")");
                            if (value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                        }
                        if (value.ownsClass && isManagedClassValue(value) &&
                            !transferredClassOwnership) {
                            // Converting a native class pointer to a ZeusValue or
                            // another ABI transfers ownership to the converted
                            // local only after this original reference is dropped.
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        value = NativeValue{
                            nativeValue,
                            functionParameterABIType(declaredType),
                            false,
                            usesZeusValueABI(declaredType),
                            declaredType,
                            ownsDynamic
                        };
                        value.addressStorageABI = addressStorageABI;
                        value.ownsClass = acquiredClassOwnership ||
                            transferredClassOwnership;
                        value.ownsNativeBytes = ownsNativeBytes;
                        value.ownsNativeString = ownsNativeString ||
                            transferredNativeString;
                    }

                    const auto addressedNative =
                        addressableNativeLocalSlots.find(static_cast<int>(index));
                    if (addressedNative != addressableNativeLocalSlots.end()) {
                        NativeSlot& slot = addressedNative->second;
                        if (slot.sourceType == "string") {
                            std::string stringValue;
                            bool stringValueOwned = false;
                            if (value.dynamic) {
                                stringValue = freshNativeTmp();
                                emitNativeLine(stringValue +
                                    " = call ptr @zeus_value_to_native_string(ptr " +
                                    value.value + ")");
                                stringValueOwned = true;
                                if (value.ownsDynamic) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value.value + ")");
                                }
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "string") {
                                stringValue = value.value;
                                stringValueOwned = value.ownsNativeString;
                            } else {
                                return failNative(ins,
                                    "typed string local requires a string value");
                            }
                            emitNativeLine("call void @zeus_string_store_slot(ptr " +
                                slot.address + ", ptr " + stringValue + ")");
                            slot.ownsNativeString = true;
                            if (stringValueOwned) {
                                emitNativeLine("call void @zeus_string_release(ptr " +
                                               stringValue + ")");
                            }
                            locals[static_cast<int>(index)] = NativeValue{
                                stringValue, "ptr", false, false, "string"
                            };
                            break;
                        }
                        if (slot.sourceType == "bytes") {
                            std::string bytesValue;
                            bool bytesValueOwned = false;
                            if (value.dynamic) {
                                bytesValue = freshNativeTmp();
                                emitNativeLine(bytesValue +
                                    " = call ptr @zeus_value_copy_bytes(ptr " +
                                    value.value + ")");
                                bytesValueOwned = true;
                                if (value.ownsDynamic) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value.value + ")");
                                }
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "bytes") {
                                bytesValue = value.value;
                                bytesValueOwned = value.ownsNativeBytes;
                            } else {
                                return failNative(ins,
                                    "typed bytes local requires a bytes value");
                            }
                            emitNativeLine("call void @zeus_bytes_store_slot(ptr " +
                                slot.address + ", ptr " + bytesValue + ", i32 " +
                                (bytesValueOwned ? "1" : "0") + ")");
                            const std::string loadedBytes = freshNativeTmp();
                            emitNativeLine(loadedBytes + " = load ptr, ptr " +
                                           slot.address + ", align 8");
                            locals[static_cast<int>(index)] = NativeValue{
                                loadedBytes, "ptr", false, false, "bytes"
                            };
                            break;
                        }
                        std::string nativeValue;
                        bool ownsDynamicArgument = false;
                        if (!emitFunctionABIArgument(
                                ins, value, slot.sourceType, nativeValue,
                                ownsDynamicArgument, false)) {
                            return false;
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        emitNativeLine("store " + slot.type + " " + nativeValue +
                            ", ptr " + slot.address +
                            (slot.type == "i1" ? ", align 1" : ", align 8"));
                        if (ownsDynamicArgument) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           nativeValue + ")");
                        }
                        locals[static_cast<int>(index)] = NativeValue{
                            nativeValue, slot.type, false, false, slot.sourceType
                        };
                        break;
                    }

                    if (nativeDynamicLoopLocals.find(static_cast<int>(index)) !=
                        nativeDynamicLoopLocals.end()) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        value = NativeValue{
                            dynamicValue, "ptr", false, true, std::string(),
                            ownsDynamic
                        };
                    }

                    const auto dynamicLoopStores =
                        nativeDynamicLoopStoreLocals.find(pc);
                    if (dynamicLoopStores != nativeDynamicLoopStoreLocals.end() &&
                        dynamicLoopStores->second.find(static_cast<int>(index)) !=
                            dynamicLoopStores->second.end()) {
                        const auto slot = nativeDynamicLoopLocalSlots.find(
                            static_cast<int>(index));
                        if (slot == nativeDynamicLoopLocalSlots.end()) {
                            return failNative(ins,
                                "dynamic loop assignment has no stable local slot");
                        }
                        NativeValue stableValue;
                        if (!cloneNativeDynamicValue(ins, value, stableValue)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       slot->second + ")");
                        emitNativeLine("call void @zeus_value_clone(ptr " +
                                       slot->second + ", ptr " + stableValue.value + ")");
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       stableValue.value + ")");
                        if (value.dynamic && value.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                        }
                        locals[static_cast<int>(index)] = NativeValue{
                            slot->second, "ptr", false, true, std::string(), true
                        };
                        break;
                    }

                    auto existing = locals.find(static_cast<int>(index));
                    if (!value.dynamic && value.type == "ptr" &&
                        value.sourceType == "string") {
                        if (!value.ownsNativeString) {
                            const std::string retained = freshNativeTmp();
                            emitNativeLine(retained +
                                " = call ptr @zeus_string_retain(ptr " + value.value + ")");
                            value.value = retained;
                            value.ownsNativeString = true;
                        }
                        if (existing != locals.end() &&
                            existing->second.ownsNativeString) {
                            releaseNativeValueWithContext(existing->second);
                        }
                        locals[static_cast<int>(index)] = std::move(value);
                        break;
                    }
                    if (!value.dynamic && value.type == "ptr" &&
                        value.sourceType == "bytes") {
                        if (!value.ownsNativeBytes) {
                            const std::string copy = freshNativeTmp();
                            emitNativeLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                           value.value + ")");
                            value.value = copy;
                            value.ownsNativeBytes = true;
                        }
                        if (existing != locals.end()) {
                            if (existing->second.ownsNativeBytes) {
                                releaseNativeValueWithContext(existing->second);
                            } else if (existing->second.dynamic &&
                                       existing->second.ownsDynamic) {
                                releaseNativeValueWithContext(existing->second);
                            } else if (existing->second.ownsClass &&
                                       isManagedClassValue(existing->second)) {
                                releaseNativeValueWithContext(existing->second);
                            } else if (existing->second.ownsNativeString) {
                                releaseNativeValueWithContext(existing->second);
                            }
                        }
                        locals[static_cast<int>(index)] = std::move(value);
                        break;
                    }
                    NativeValue dynamicClassSource;
                    const bool convertedDynamicClass = value.dynamic &&
                        existing != locals.end() &&
                        isManagedClassValue(existing->second);
                    if (convertedDynamicClass) {
                        dynamicClassSource = value;
                        const std::string nativeValue = freshNativeTmp();
                        emitNativeLine(nativeValue +
                            " = call ptr @zeus_value_try_class_ptr(ptr " +
                            value.value + ", ptr " +
                            poolStr(existing->second.sourceType) + ")");
                        value = NativeValue{
                            nativeValue, "ptr", false, false,
                            existing->second.sourceType
                        };
                    }

                    // Clone borrowed dynamic values before releasing the value
                    // already held by the local. This also makes self-assigns
                    // safe when both paths name the same ZeusValue pointer.
                    if (value.dynamic && !value.ownsDynamic) {
                        NativeValue owned;
                        if (!cloneNativeDynamicValue(ins, value, owned)) return false;
                        value = std::move(owned);
                    }

                    if (existing != locals.end() && existing->second.dynamic &&
                        existing->second.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       existing->second.value + ")");
                    }
                    const bool releaseOldClass = existing != locals.end() &&
                        existing->second.ownsClass &&
                        isManagedClassValue(existing->second);
                    const bool releaseOldNativeString = existing != locals.end() &&
                        existing->second.ownsNativeString;
                    if (isManagedClassValue(value)) {
                        if (!value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           value.value + ")");
                        }
                        if (releaseOldClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           existing->second.value + ")");
                        }
                        if (releaseOldNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           existing->second.value + ")");
                        }
                        value.ownsClass = true;
                        locals[static_cast<int>(index)] = std::move(value);
                    } else {
                        if (releaseOldClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           existing->second.value + ")");
                        }
                        if (releaseOldNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           existing->second.value + ")");
                        }
                        locals[static_cast<int>(index)] = value;
                    }
                    if (convertedDynamicClass && dynamicClassSource.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       dynamicClassSource.value + ")");
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] STORE_VALUE_SHARED 缺少名称");
                    }
                    NativeValue value;
                    if (!popNative(ins, value) || value.functionRef) return false;
                    emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);
                    const std::string sharedName = std::get<std::string>(ins.args[0]);
                    const auto* sharedABI = ins.metadata.kind ==
                            IRType::IRMetadataKind::SharedBindingType
                        ? ZeusBuiltins::findNativeTypeABI(ins.metadata.typeName)
                        : nullptr;
                    if (sharedABI && ZeusBuiltins::findNativeIntegerABI(
                            ins.metadata.typeName)) {
                        std::string integerValue;
                        bool ownsInteger = false;
                        if (!emitFunctionABIArgument(ins, value, ins.metadata.typeName,
                                integerValue, ownsInteger, false)) return false;
                        value = NativeValue{integerValue, llvmType(ins.metadata.typeName),
                            false, false, ins.metadata.typeName};
                    }
                    if (sharedABI && (sharedABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::Int64 ||
                        sharedABI->valueABI == ZeusBuiltins::NativeValueABI::Bool1)) {
                        std::string nativeValue;
                        bool ownsNativeValue = false;
                        if (!emitFunctionABIArgument(
                                ins, value, ins.metadata.typeName,
                                nativeValue, ownsNativeValue, false)) return false;
                        const std::string operation =
                            ins.op == VMOperator::STORE_VALUE_SHARED ? "store" : "assign";
                        if (sharedABI->valueABI == ZeusBuiltins::NativeValueABI::Int64) {
                            emitNativeLine(std::string("call void @zeus_shared_value_") +
                                operation + "_i64(ptr " + poolStr(sharedName) +
                                ", i64 " + nativeValue + ")");
                        } else {
                            const std::string boolValue = freshNativeTmp();
                            emitNativeLine(boolValue + " = zext i1 " + nativeValue + " to i32");
                            emitNativeLine(std::string("call void @zeus_shared_value_") +
                                operation + "_bool(ptr " + poolStr(sharedName) +
                                ", i32 " + boolValue + ")");
                        }
                        if (ownsNativeValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           nativeValue + ")");
                        }
                    } else {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) return false;
                        const char* operation = ins.op == VMOperator::STORE_VALUE_SHARED
                            ? "store" : "assign";
                        emitNativeLine(std::string("call void @zeus_shared_value_") + operation +
                            "(ptr " + poolStr(sharedName) +
                            ", ptr " + dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                        }
                    }
                    if (value.ownsClass && isManagedClassValue(value)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " + value.value + ")");
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_GLOBAL:
                case VMOperator::ASSIGN_VALUE_GLOBAL: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || index < 0 ||
                        !popNative(ins, value)) {
                        return failNative(ins, "[emit-llvm] 原生函数全局变量存储失败");
                    }

                    const auto declaredDouble = nativeGlobalDeclaredDoubleTypes.find(static_cast<int>(index));
                    if (declaredDouble != nativeGlobalDeclaredDoubleTypes.end()) {
                        std::string nativeValue;
                        bool ownsValue = false;
                        if (!emitFunctionABIArgument(ins, value, declaredDouble->second, nativeValue, ownsValue, false)) return false;
                        const std::string nativeType = llvmType(declaredDouble->second);
                        emitNativeLine("call void @zeus_global_store_" +
                            std::string(nativeType == "float" ? "float" : "double") +
                            "(i32 " + std::to_string(index) + ", " + nativeType +
                            " " + nativeValue + ")");
                        break;
                    }
                    const auto declaredInteger = nativeGlobalDeclaredIntegerTypes.find(
                        static_cast<int>(index));
                    if (declaredInteger != nativeGlobalDeclaredIntegerTypes.end()) {
                        std::string integerValue;
                        bool ownsInteger = false;
                        if (!emitFunctionABIArgument(ins, value, declaredInteger->second,
                                integerValue, ownsInteger, false)) return false;
                        if (const auto* unsignedABI = ZeusBuiltins::findNativeUnsignedIntegerABI(
                                declaredInteger->second)) {
                            if (llvmType(declaredInteger->second) != "i64") {
                                const std::string widened = freshNativeTmp();
                                emitNativeLine(widened + " = zext " +
                                    llvmType(declaredInteger->second) + " " + integerValue +
                                    " to i64");
                                integerValue = widened;
                            }
                            emitNativeLine("call void @zeus_global_store_unsigned_integer(i32 " +
                                std::to_string(index) + ", i64 " + integerValue + ", i32 " +
                                std::to_string(unsignedABI->integerBits) + ")");
                        } else {
                            integerValue = emitNativeIntegerValueConversion(integerValue,
                                llvmType(declaredInteger->second), declaredInteger->second,
                                "i64", "int", emitNativeLine, freshNativeTmp);
                            emitNativeLine("call void @zeus_global_store_i64(i32 " +
                                std::to_string(index) + ", i64 " + integerValue + ")");
                        }
                        break;
                    }

                    emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);
                    if (value.functionRef) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        value = NativeValue{
                            dynamicValue, "ptr", false, true,
                            std::string(), ownsDynamic
                        };
                    }
                    if (value.dynamic && !value.ownsDynamic) {
                        NativeValue owned;
                        if (!cloneNativeDynamicValue(ins, value, owned)) return false;
                        value = std::move(owned);
                    }

                    const auto nativeViewType =
                        prescanGlobalNativeViewTypes.find(static_cast<int>(index));
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "string") {
                        std::string nativeString;
                        bool ownsNativeString = false;
                        if (!emitNativeValueAsType(
                                value, "ptr", ins, nativeString, "string", false,
                                nullptr, 0, &ownsNativeString)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_global_store_string(i32 " +
                            std::to_string(index) + ", ptr " + nativeString + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        if (ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           nativeString + ")");
                        }
                        releaseNativeValueWithContext(value);
                        break;
                    }
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "bytes") {
                        std::string nativeBytes;
                        bool ownsDynamicArgument = false;
                        if (!emitFunctionABIArgumentWithContext(
                                ins, value, "bytes", nativeBytes,
                                ownsDynamicArgument, true, nullptr, 0)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_global_store_bytes(i32 " +
                            std::to_string(index) + ", ptr " + nativeBytes + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        if (value.dynamic) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           nativeBytes + ")");
                        }
                        if ((value.dynamic && value.ownsDynamic) ||
                            value.ownsNativeBytes ||
                            (value.ownsClass && isManagedClassValue(value))) {
                            releaseNativeValueWithContext(value);
                        }
                        break;
                    }

                    const auto globalScalar =
                        nativeGlobalScalarTypes.find(static_cast<int>(index));
                    if (globalScalar != nativeGlobalScalarTypes.end() &&
                        !value.dynamic && value.type == globalScalar->second) {
                        if (value.type == "i64") {
                            emitNativeLine("call void @zeus_global_store_i64(i32 " +
                                std::to_string(index) + ", i64 " + value.value + ")");
                        } else {
                            const std::string raw = freshNativeTmp();
                            emitNativeLine(raw + " = zext i1 " + value.value + " to i32");
                            emitNativeLine("call void @zeus_global_store_bool(i32 " +
                                std::to_string(index) + ", i32 " + raw + ")");
                        }
                        break;
                    }

                    std::string dynamicValue;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) {
                        return false;
                    }
                    entryDynamicValueGlobals.insert(static_cast<int>(index));
                    emitNativeLine("call void @zeus_value_global_store(i32 " +
                                   std::to_string(index) + ", ptr " + dynamicValue + ")");
                    if (isManagedClassValue(value) && value.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       value.value + ")");
                    }
                    if (value.ownsNativeBytes) {
                        emitNativeLine("call void @zeus_bytes_release(ptr " +
                                       value.value + ")");
                    }
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 || !std::holds_alternative<std::string>(ins.args[0]) ||
                        !std::holds_alternative<std::string>(ins.args[1])) {
                        return failNative(ins, "[emit-llvm] DELETE_VAR 参数无效");
                    }
                    const std::string& kind = std::get<std::string>(ins.args[1]);
                    if (kind == "shared") {
                        emitNativeLine("call i32 @zeus_shared_value_delete(ptr " +
                            poolStr(std::get<std::string>(ins.args[0])) + ")");
                        break;
                    }
                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0) {
                        return failNative(ins, "[emit-llvm] delete 变量索引无效");
                    }
                    if (kind == "global") {
                        emitNativeLine("call void @zeus_value_global_delete(i32 " +
                                       std::to_string(index) + ")");
                        break;
                    }
                    if (kind == "upvalue") {
                        emitNativeLine("call void @zeus_closure_env_delete(ptr %closure.env, i32 " +
                                       std::to_string(index) + ")");
                        break;
                    }
                    if (kind != "local") {
                        return failNative(ins, "[emit-llvm] DELETE_VAR 变量类型不支持");
                    }
                    const auto capturedCell = closureLocalCells.find(static_cast<int>(index));
                    if (capturedCell != closureLocalCells.end()) {
                        emitNativeLine("call void @zeus_capture_cell_delete(ptr " +
                                       capturedCell->second + ")");
                        break;
                    }
                    auto objectIt = locals.find(static_cast<int>(index));
                    if (objectIt == locals.end()) break;

                    if (objectIt->second.dynamic) {
                        emitNativeLine("call void @zeus_value_delete_object(ptr " +
                                       objectIt->second.value + ")");
                        if (objectIt->second.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           objectIt->second.value + ")");
                        }
                        locals.erase(objectIt);
                        break;
                    }
                    if (objectIt->second.type != "ptr" ||
                        !isClassType(objectIt->second.sourceType)) {
                        locals.erase(objectIt);
                        break;
                    }

                    const NativeValue object = objectIt->second;
                    auto emitDeleteLabel = [&](const std::string& label) {
                        functionText += label.substr(1) + ":\n";
                        // Same as the construction split: the tail label is the
                        // real predecessor of whatever comes next.
                        currentNativeBlock = label;
                    };
                    auto failDelete = [&](
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        return failNative(at, message);
                    };
                    if (!emitClassDelete(
                            ins, object, false, emitNativeLine, emitDeleteLabel,
                            freshNativeTmp, failDelete)) {
                        return false;
                    }

                    break;
                }

                case VMOperator::OBJ_LOAD_SAFE_ATTR:
                case VMOperator::OBJ_LOAD_ATTR: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取缺少字段名");
                    }

                    NativeValue object;
                    if (!popNative(ins, object) || object.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取的对象无效");
                    }
                    // Safe navigation always resolves dynamically: the
                    // receiver may be nil, and nil has no native form.
                    const bool safeNavAttr =
                        (ins.op == VMOperator::OBJ_LOAD_SAFE_ATTR);
                    if (safeNavAttr && !object.dynamic) {
                        const NativeValue originalObject = object;
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, object, dynamicValue, ownsDynamic)) {
                            return failNative(ins, "[emit-llvm] 安全导航接收者无法装箱");
                        }
                        if (originalObject.ownsClass &&
                            isManagedClassValue(originalObject)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                originalObject.value + ")");
                        } else if (originalObject.ownsNativeBytes &&
                            !originalObject.value.empty()) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                originalObject.value + ")");
                        }
                        object = NativeValue{
                            dynamicValue, "ptr", false, true,
                            std::string(), ownsDynamic
                        };
                    }
                    const std::string& fieldName = std::get<std::string>(ins.args[0]);
                    if (safeNavAttr || (object.dynamic && object.type == "ptr")) {
                        dynamicFieldLoads.insert(fieldName);
                        const std::string loaded = freshNativeTmp();
                        const std::string fieldSymbol = poolStr(fieldName);
                        emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                        const FunctionSignature* method =
                            findAnyInstanceMethod(fieldName);
                        // Safe navigation always goes through the full
                        // dynamic lookup: it nil-checks first, then applies
                        // field-wins-over-method precedence itself.
                        const bool useFieldFastPath =
                            !safeNavAttr && method == nullptr;
                        if (!useFieldFastPath) {
                            // The adapter is only invoked when the member
                            // turns out to be a method; pure fields pass null.
                            std::string adapter = "null";
                            if (method != nullptr) {
                                adapter = ensureBoundMethodAdapter(fieldName);
                            }
                            const std::string attributeCallSymbol = safeNavAttr
                                ? "zeus_value_dynamic_safe_attribute_load"
                                : "zeus_value_dynamic_attribute_load";
                            emitNativeLine(
                                "call void @" + attributeCallSymbol + "(ptr " +
                                loaded + ", ptr " + object.value + ", ptr " +
                                fieldSymbol + ", i32 " +
                                std::to_string(ins.line > 0 ? ins.line : 0) +
                                ", ptr " + adapter + ")");
                        } else {
                            emitNativeLine("call void @zeus_value_dynamic_field_load(ptr " +
                                           loaded + ", ptr " + object.value + ", ptr " +
                                           fieldSymbol + ", i32 " +
                                           std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        }
                        if (object.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           object.value + ")");
                        }
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, true, std::string(), true
                        });
                        break;
                    }
                    emitRequireLiveIfNeeded(object, emitNativeLine);

                    if (object.type == "namespace") {
                        const auto exports =
                            namespaceExportsByPackage.find(object.sourceType);
                        if (exports != namespaceExportsByPackage.end() &&
                            exports->second.find(fieldName) != exports->second.end() &&
                            isClassType(fieldName)) {
                            valueStack.push_back(NativeValue{
                                fieldName, "classref", false, false, fieldName
                            });
                            break;
                        }
                        const FunctionSignature* function =
                            findNamespaceFunction(object.sourceType, fieldName);
                        if (function == nullptr || function->isExternal ||
                            function->captureCount != 0) {
                            return failNative(ins,
                                "[emit-llvm] unsupported static module member " +
                                object.sourceType + "." + fieldName);
                        }
                        valueStack.push_back(NativeValue{
                            function->name, "function", true, false
                        });
                        break;
                    }

                    if (object.type == "classref") {
                        const FunctionSignature* function =
                            findStaticClassFunction(object.sourceType, fieldName);
                        if (function == nullptr || function->isExternal ||
                            function->captureCount != 0) {
                            return failNative(ins,
                                "[emit-llvm] unsupported static class member " +
                                object.sourceType + "." + fieldName);
                        }
                        valueStack.push_back(NativeValue{
                            function->name, "function", true, false
                        });
                        break;
                    }

                    if (isManagedClassValue(object) &&
                        shouldUseDynamicFieldSidecar(
                            object.sourceType, fieldName)) {
                        std::string dynamicReceiver;
                        bool ownsDynamicReceiver = false;
                        if (!emitNativeDynamicValue(
                                ins, object, dynamicReceiver, ownsDynamicReceiver)) {
                            return false;
                        }
                        const std::string loaded = freshNativeTmp();
                        emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_value_dynamic_field_load(ptr " +
                                       loaded + ", ptr " + dynamicReceiver + ", ptr " +
                                       poolStr(fieldName) + ", i32 " +
                                       std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        if (ownsDynamicReceiver) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicReceiver + ")");
                        }
                        if (object.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           object.value + ")");
                        }
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, true, std::string(), true
                        });
                        break;
                    }

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取的对象不是已知类布局");
                    }

                    const int fieldIndex = findFieldIndex(*layout, fieldName);
                    if (fieldIndex < 0 || static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        const auto method = functionSignatures.find(
                            object.sourceType + "::" + fieldName);
                        if (method != functionSignatures.end() &&
                            method->second.ownerType == object.sourceType &&
                            !method->second.isStaticMethod &&
                            !method->second.parameterTypes.empty() &&
                            method->second.parameterTypes.front() == object.sourceType) {
                            NativeValue boundMethod;
                            auto boxReceiver = [&](const NativeValue& value,
                                                   const IRType::IRInstr& at,
                                                   std::string& dynamicValue,
                                                   bool& ownsDynamic) {
                                return emitNativeDynamicValue(
                                    at, value, dynamicValue, ownsDynamic);
                            };
                            if (!emitBoundMethodClosure(
                                    ins, object, method->second, boundMethod,
                                    [&](const std::string& line) {
                                        emitNativeLine(line);
                                    },
                                    [&]() { return freshNativeTmp(); },
                                    boxReceiver)) {
                                return failNative(
                                    ins, "[emit-llvm] could not create a bound method closure");
                            }
                            valueStack.push_back(std::move(boundMethod));
                            break;
                        }
                        return failNative(ins, "[emit-llvm] 原生函数访问了不存在的字段'" + fieldName + "'");
                    }

                    const std::string& fieldTypeName =
                        layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    if (fieldType.empty()) {
                        return failNative(ins, "[emit-llvm] 类字段类型无效");
                    }
                    const std::string fieldAddress = freshNativeTmp();
                    emitNativeLine(
                        fieldAddress + " = getelementptr inbounds " +
                        llvmStructType(object.sourceType) + ", ptr " + object.value +
                        ", i32 0, i32 " + std::to_string(fieldIndex)
                    );
                    const auto inferred = layout->inferredFieldFlags.find(fieldName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshNativeTmp();
                        const std::string initialized = freshNativeTmp();
                        const std::string classSymbol = poolStr(object.sourceType);
                        const std::string fieldSymbol = poolStr(fieldName);
                        emitNativeLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitNativeLine(initialized + " = load i1, ptr " + flagAddress);
                        const std::string initialized32 = freshNativeTmp();
                        emitNativeLine(initialized32 + " = zext i1 " + initialized + " to i32");
                        emitNativeLine("call void @zeus_class_field_require_initialized(i32 " +
                            initialized32 + ", ptr " + classSymbol + ", ptr " + fieldSymbol +
                            ", i32 " + std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                    }
                    if (usesZeusValueABI(fieldTypeName)) {
                        NativeValue loaded{
                            fieldAddress, "ptr", false, true, fieldTypeName, false
                        };
                        if (object.ownsClass && isManagedClassValue(object)) {
                            loaded.value = freshNativeTmp();
                            emitNativeLine(loaded.value + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_clone(ptr " + loaded.value +
                                           ", ptr " + fieldAddress + ")");
                            loaded.ownsDynamic = true;
                        }
                        valueStack.push_back(std::move(loaded));
                        if (object.ownsClass && isManagedClassValue(object)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           object.value + ")");
                        }
                        break;
                    }
                    const std::string temporary = freshNativeTmp();
                    emitNativeLine(
                        temporary + " = load " + fieldType + ", ptr " + fieldAddress
                    );
                    NativeValue loaded{
                        temporary,
                        fieldType,
                        false,
                        false,
                        fieldTypeName
                    };
                    const auto* loadedFieldABI =
                        ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                    if (loadedFieldABI != nullptr && loadedFieldABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::NativeBytes) {
                        if (detail::isNativeBytesDataAddressReceiver(chunk, pc) ||
                            isBorrowedBytesArgument(chunk, pc)) {
                            loaded.ownsNativeBytes = false;
                        } else {
                            const std::string copy = freshNativeTmp();
                            emitNativeLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                           temporary + ")");
                            loaded.value = copy;
                            loaded.ownsNativeBytes = true;
                        }
                    } else if (loadedFieldABI != nullptr &&
                               loadedFieldABI->valueABI ==
                                   ZeusBuiltins::NativeValueABI::NativeString &&
                               object.ownsClass && isManagedClassValue(object)) {
                        loaded.value = freshNativeTmp();
                        emitNativeLine(loaded.value +
                            " = call ptr @zeus_string_retain(ptr " + temporary + ")");
                        loaded.ownsNativeString = true;
                    }
                    if (isClassType(fieldTypeName) &&
                        object.ownsClass && isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                       temporary + ")");
                        loaded.ownsClass = true;
                    }
                    valueStack.push_back(std::move(loaded));
                    if (object.ownsClass && isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       object.value + ")");
                    }
                    break;
                }

                case VMOperator::OBJ_STORE_ATTR: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] OBJ_STORE_ATTR 缺少字段名");
                    }

                    NativeValue value;
                    NativeValue object;
                    if (!popNative(ins, value) || !popNative(ins, object) ||
                        value.functionRef || object.functionRef) {
                        return failNative(ins, "[emit-llvm] OBJ_STORE_ATTR 操作数无效");
                    }
                    emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);
                    emitRequireLiveIfNeeded(object, emitNativeLine);

                    const std::string& fieldName = std::get<std::string>(ins.args[0]);
                    if (object.dynamic && object.type == "ptr") {
                        dynamicFieldStores.insert(fieldName);
                        std::string dynamicValue;
                        bool ownsDynamicValue = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamicValue)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_dynamic_field_store(ptr " +
                                       object.value + ", ptr " + poolStr(fieldName) +
                                       ", ptr " + dynamicValue + ", i32 " +
                                       std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        if (ownsDynamicValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        if (object.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           object.value + ")");
                        }
                        break;
                    }

                    if (isManagedClassValue(object) &&
                        shouldUseDynamicFieldSidecar(
                            object.sourceType, fieldName)) {
                        std::string dynamicReceiver;
                        std::string dynamicValue;
                        bool ownsDynamicReceiver = false;
                        bool ownsDynamicValue = false;
                        if (!emitNativeDynamicValue(
                                ins, object, dynamicReceiver, ownsDynamicReceiver) ||
                            !emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamicValue)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_dynamic_field_store(ptr " +
                                       dynamicReceiver + ", ptr " + poolStr(fieldName) +
                                       ", ptr " + dynamicValue + ", i32 " +
                                       std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        if (ownsDynamicValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (ownsDynamicReceiver) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicReceiver + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        if (object.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           object.value + ")");
                        }
                        break;
                    }

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        return failNative(ins, "[emit-llvm] 对象没有已知的类布局");
                    }

                    const int fieldIndex = findFieldIndex(*layout, fieldName);
                    if (fieldIndex < 0 || static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        return failNative(ins, "[emit-llvm] 类不存在字段 '" + fieldName + "'");
                    }
                    if (static_cast<size_t>(fieldIndex) < layout->fieldConstness.size() &&
                        layout->fieldConstness[static_cast<size_t>(fieldIndex)]) {
                        return failNative(ins, "[emit-llvm] 不能修改 const 字段 '" + fieldName + "'");
                    }

                    const std::string& fieldTypeName =
                        layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    const std::string fieldAddress = freshNativeTmp();
                    emitNativeLine(
                        fieldAddress + " = getelementptr inbounds " +
                        llvmStructType(object.sourceType) + ", ptr " + object.value +
                        ", i32 0, i32 " + std::to_string(fieldIndex)
                    );
                    const auto inferred = layout->inferredFieldFlags.find(fieldName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshNativeTmp();
                        emitNativeLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitNativeLine("store i1 1, ptr " + flagAddress);
                    }

                    if (usesZeusValueABI(fieldTypeName)) {
                        std::string sourceValue;
                        bool ownsSourceValue = false;
                        if (!emitNativeDynamicValue(
                                ins, value, sourceValue, ownsSourceValue)) {
                            return false;
                        }
                        const std::string ownedCopy = freshNativeTmp();
                        emitNativeLine(ownedCopy + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_value_clone(ptr " + ownedCopy +
                                       ", ptr " + sourceValue + ")");
                        if (ownsSourceValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           sourceValue + ")");
                        }
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       fieldAddress + ")");
                        emitNativeLine("call void @zeus_value_clone(ptr " +
                                       fieldAddress + ", ptr " + ownedCopy + ")");
                        emitNativeLine("call void @zeus_value_release(ptr " + ownedCopy + ")");
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        if (object.ownsClass && isManagedClassValue(object)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           object.value + ")");
                        }
                        break;
                    }

                    const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                    if (fieldABI != nullptr && fieldABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::NativeString) {
                        std::string stringValue;
                        bool stringValueOwned = false;
                        if (value.dynamic) {
                            stringValue = freshNativeTmp();
                            emitNativeLine(stringValue +
                                " = call ptr @zeus_value_to_native_string(ptr " +
                                value.value + ")");
                            stringValueOwned = true;
                            if (value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                        } else if (value.type == "ptr" &&
                                   value.sourceType == "string") {
                            stringValue = value.value;
                            stringValueOwned = value.ownsNativeString;
                        } else if (isClassType(value.sourceType) &&
                                   value.sourceType == "String") {
                            std::string boxed;
                            bool ownsBoxed = false;
                            if (!emitNativeDynamicValue(ins, value, boxed,
                                                        ownsBoxed)) {
                                return false;
                            }
                            stringValue = freshNativeTmp();
                            emitNativeLine(stringValue +
                                " = call ptr @zeus_value_to_native_string(ptr " +
                                boxed + ")");
                            stringValueOwned = true;
                            if (ownsBoxed) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               boxed + ")");
                            }
                        } else {
                            return failNative(ins,
                                "string field assignment requires a string value");
                        }
                        emitNativeLine("call void @zeus_string_store_slot(ptr " +
                            fieldAddress + ", ptr " + stringValue + ")");
                        if (stringValueOwned) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           stringValue + ")");
                        }
                        if (value.ownsNativeString && value.value != stringValue) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           value.value + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            releaseNativeValueWithContext(value);
                        }
                        if (object.ownsClass && isManagedClassValue(object)) {
                            releaseNativeValueWithContext(object);
                        }
                        break;
                    }
                    if (fieldABI != nullptr && fieldABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::NativeBytes) {
                        std::string bytesValue;
                        bool bytesValueOwned = false;
                        if (value.dynamic) {
                            bytesValue = freshNativeTmp();
                            emitNativeLine(bytesValue +
                                " = call ptr @zeus_value_copy_bytes(ptr " +
                                value.value + ")");
                            bytesValueOwned = true;
                            if (value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                        } else if (value.type == "ptr" &&
                                   value.sourceType == "bytes") {
                            bytesValue = value.value;
                            bytesValueOwned = value.ownsNativeBytes;
                        } else {
                            return failNative(ins,
                                "bytes field assignment requires a bytes value");
                        }
                        emitNativeLine("call void @zeus_bytes_store_slot(ptr " +
                            fieldAddress + ", ptr " + bytesValue + ", i32 " +
                            (bytesValueOwned ? "1" : "0") + ")");
                        if (object.ownsClass && isManagedClassValue(object)) {
                            releaseNativeValueWithContext(object);
                        }
                        break;
                    }

                    std::string storedValue = value.value;
                    if (ZeusBuiltins::isNativePointerABI(fieldABI)) {
                        if (value.dynamic) {
                            bool ownsConvertedValue = false;
                            if (!emitFunctionABIArgument(
                                    ins, value, fieldTypeName, storedValue,
                                    ownsConvertedValue, false)) {
                                return false;
                            }
                            if (ownsConvertedValue) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               storedValue + ")");
                            }
                        } else {
                            if (value.type != fieldType) {
                                return failNative(ins,
                                    "[emit-llvm] 类字段赋值类型不匹配");
                            }
                            storedValue = freshNativeTmp();
                            emitNativeLine(storedValue + " = call ptr @" +
                                           std::string(ZeusBuiltins::nativePointerInternHelper(fieldABI)) +
                                           "(ptr " + value.value + ")");
                        }
                    } else if (value.dynamic) {
                        bool ownsConvertedValue = false;
                        if (!emitFunctionABIArgument(
                                ins, value, fieldTypeName, storedValue,
                                ownsConvertedValue, false)) {
                            return false;
                        }
                        if (ownsConvertedValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           storedValue + ")");
                        }
                    } else if (fieldType == "double" && !value.dynamic &&
                               value.sourceType != "char" && isSignedIntegerLLVMType(value.type)) {
                        storedValue = emitDoubleConversion(value.value, value.type, emitNativeLine, freshNativeTmp);
                    } else if (isSignedIntegerLLVMType(value.type) &&
                               isSignedIntegerLLVMType(fieldType) && value.sourceType != "char") {
                        storedValue = emitSignedIntegerConversion(
                            value.value, value.type, fieldType, emitNativeLine, freshNativeTmp);
                    } else if (fieldType.empty() || value.type != fieldType ||
                               (isClassType(fieldTypeName) &&
                                value.sourceType != fieldTypeName)) {
                        return failNative(ins, "[emit-llvm] 类字段赋值类型不匹配");
                    }

                    if (isClassType(fieldTypeName)) {
                        // Every field owns one reference. Retain before replacing
                        // the field, then release the consumed temporary below.
                        emitNativeLine("call void @zeus_class_ref_retain_edge(ptr " +
                                       storedValue + ")");
                        const std::string previous = freshNativeTmp();
                        emitNativeLine(previous + " = load ptr, ptr " + fieldAddress);
                        emitNativeLine("call void @zeus_class_ref_release_edge(ptr " +
                                       previous + ")");
                        emitNativeLine("store ptr " + storedValue + ", ptr " + fieldAddress);
                        if (value.dynamic && value.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                        }
                        if (value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           storedValue + ")");
                        }
                    } else {
                        emitNativeLine(
                            "store " + fieldType + " " + storedValue + ", ptr " + fieldAddress
                        );
                    }
                    if (object.ownsClass && isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       object.value + ")");
                    }
                    break;
                }

                case VMOperator::OBJ_REPLACE_STATE: {
                    if (ins.args.empty() ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins,
                            "[emit-llvm] @this replacement is missing its class type");
                    }
                    const std::string& expectedType =
                        std::get<std::string>(ins.args[0]);
                    if (!isClassType(expectedType)) {
                        return failNative(ins,
                            "[emit-llvm] @this replacement requires a declared class type");
                    }

                    NativeValue replacement;
                    NativeValue receiver;
                    if (!popNative(ins, replacement) ||
                        !popNative(ins, receiver) || replacement.functionRef ||
                        receiver.functionRef) {
                        return failNative(ins,
                            "[emit-llvm] @this replacement requires class instances");
                    }

                    std::string receiverPointer;
                    std::string replacementPointer;
                    bool ownsReceiverArgument = false;
                    bool ownsReplacementArgument = false;
                    if (!emitFunctionABIArgument(
                            ins, receiver, expectedType, receiverPointer,
                            ownsReceiverArgument, true) ||
                        !emitFunctionABIArgument(
                            ins, replacement, expectedType, replacementPointer,
                            ownsReplacementArgument, true)) {
                        return false;
                    }

                    emitNativeLine("call void @zeus_class_ref_require_live(ptr " +
                                   receiverPointer + ")");
                    emitNativeLine("call void @zeus_class_ref_require_live(ptr " +
                                   replacementPointer + ")");

                    const ClassLayout* layout = findClassLayout(expectedType);
                    if (layout == nullptr) {
                        return failNative(ins,
                            "[emit-llvm] @this replacement has no class layout");
                    }

                    for (size_t fieldIndex = 0;
                         fieldIndex < layout->fieldTypes.size(); ++fieldIndex) {
                        const std::string& fieldTypeName =
                            layout->fieldTypes[fieldIndex];
                        const std::string fieldType = llvmFieldType(fieldTypeName);
                        if (fieldType.empty()) {
                            return failNative(ins,
                                "[emit-llvm] @this replacement found a field without LLVM storage");
                        }

                        const std::string destinationAddress = freshNativeTmp();
                        const std::string sourceAddress = freshNativeTmp();
                        const std::string structType = llvmStructType(expectedType);
                        const std::string fieldOffset = std::to_string(fieldIndex);
                        emitNativeLine(destinationAddress +
                            " = getelementptr inbounds " + structType + ", ptr " +
                            receiverPointer + ", i32 0, i32 " + fieldOffset);
                        emitNativeLine(sourceAddress +
                            " = getelementptr inbounds " + structType + ", ptr " +
                            replacementPointer + ", i32 0, i32 " + fieldOffset);

                        if (usesZeusValueABI(fieldTypeName)) {
                            const std::string savedValue = freshNativeTmp();
                            emitNativeLine(savedValue + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_clone(ptr " +
                                           savedValue + ", ptr " + sourceAddress + ")");
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           destinationAddress + ")");
                            emitNativeLine("call void @zeus_value_clone(ptr " +
                                           destinationAddress + ", ptr " + savedValue + ")");
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           savedValue + ")");
                        } else if (const auto* stringFieldABI =
                                       ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                                   stringFieldABI != nullptr && stringFieldABI->valueABI ==
                                       ZeusBuiltins::NativeValueABI::NativeString) {
                            const std::string sourceValue = freshNativeTmp();
                            emitNativeLine(sourceValue + " = load ptr, ptr " +
                                           sourceAddress);
                            emitNativeLine("call void @zeus_string_store_slot(ptr " +
                                           destinationAddress + ", ptr " + sourceValue + ")");
                        } else if (const auto* fieldABI =
                                       ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                                   fieldABI != nullptr && fieldABI->valueABI ==
                                       ZeusBuiltins::NativeValueABI::NativeBytes) {
                            const std::string sourceValue = freshNativeTmp();
                            const std::string copiedValue = freshNativeTmp();
                            const std::string previousValue = freshNativeTmp();
                            emitNativeLine(sourceValue + " = load ptr, ptr " +
                                           sourceAddress);
                            emitNativeLine(copiedValue +
                                " = call ptr @zeus_bytes_clone(ptr " + sourceValue + ")");
                            emitNativeLine(previousValue + " = load ptr, ptr " +
                                           destinationAddress);
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           previousValue + ")");
                            emitNativeLine("store ptr " + copiedValue + ", ptr " +
                                           destinationAddress);
                        } else if (isClassType(fieldTypeName)) {
                            const std::string sourceValue = freshNativeTmp();
                            const std::string previousValue = freshNativeTmp();
                            emitNativeLine(sourceValue + " = load ptr, ptr " +
                                           sourceAddress);
                            emitNativeLine("call void @zeus_class_ref_retain_edge(ptr " +
                                           sourceValue + ")");
                            emitNativeLine(previousValue + " = load ptr, ptr " +
                                           destinationAddress);
                            emitNativeLine("call void @zeus_class_ref_release_edge(ptr " +
                                           previousValue + ")");
                            emitNativeLine("store ptr " + sourceValue + ", ptr " +
                                           destinationAddress);
                        } else {
                            const std::string sourceValue = freshNativeTmp();
                            emitNativeLine(sourceValue + " = load " + fieldType +
                                           ", ptr " + sourceAddress);
                            emitNativeLine("store " + fieldType + " " + sourceValue +
                                           ", ptr " + destinationAddress);
                        }
                    }

                    emitNativeLine(
                        "call void @zeus_value_dynamic_field_replace_state(ptr " +
                        receiverPointer + ", ptr " + replacementPointer + ")");

                    auto releaseReplacementOperand = [&](const NativeValue& operand) {
                        if (operand.dynamic && operand.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           operand.value + ")");
                        } else if (operand.ownsClass &&
                                   isManagedClassValue(operand)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           operand.value + ")");
                        }
                    };
                    releaseReplacementOperand(replacement);
                    releaseReplacementOperand(receiver);
                    break;
                }

                case VMOperator::LOGICAL_NOT: {
                    NativeValue operand;
                    if (!popNative(ins, operand) || operand.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数一元逻辑非操作数无效");
                    }

                    const std::string resultValue = freshNativeTmp();
                    if (!operand.dynamic && operand.type == "i1") {
                        emitNativeLine(resultValue + " = xor i1 " + operand.value + ", true");
                    } else {
                        std::string dynamicValue;
                        bool ownsDynamicValue = false;
                        if (!emitNativeDynamicValue(
                                ins, operand, dynamicValue, ownsDynamicValue)) {
                            return false;
                        }
                        const std::string truthy = freshNativeTmp();
                        emitNativeLine(truthy + " = call i32 @zeus_value_truthy(ptr " +
                                       dynamicValue + ")");
                        if (ownsDynamicValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (isManagedClassValue(operand) && operand.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           operand.value + ")");
                        }
                        emitNativeLine(resultValue + " = icmp eq i32 " + truthy + ", 0");
                    }
                    valueStack.push_back(NativeValue{ resultValue, "i1", false });
                    break;
                }

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
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            return failNative(ins, "[emit-llvm] 静态运算标注缺少函数名");
                        }
                    }

                    if (!pendingDirectCall.empty()) {
                        if (!emitNativeDirectCall(ins)) return false;
                        break;
                    }

                    NativeValue right;
                    NativeValue left;
                    if (!popNative(ins, right) || !popNative(ins, left) ||
                        left.functionRef || right.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数二元运算操作数无效");
                    }

                    NativeValue floatingResult;
                    const bool leftUnsigned =
                        ZeusBuiltins::findNativeUnsignedIntegerABI(left.sourceType) != nullptr;
                    const bool rightUnsigned =
                        ZeusBuiltins::findNativeUnsignedIntegerABI(right.sourceType) != nullptr;
                    if (emitNativeFloatingBinary(ins.op, left, right, floatingResult,
                            leftUnsigned, rightUnsigned, emitNativeLine, freshNativeTmp)) {
                        std::set<std::string> releasedFloatingOperands;
                        for (const NativeValue* operand : { &left, &right }) {
                            if (operand->dynamic && operand->ownsDynamic &&
                                releasedFloatingOperands.insert(operand->value).second) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               operand->value + ")");
                            }
                        }
                        valueStack.push_back(std::move(floatingResult));
                        break;
                    }

                    // Class instances have no static operator, while native
                    // string equality has a direct NativeString ABI.
                    const bool classOperands = !left.dynamic && !right.dynamic &&
                        (isClassType(left.sourceType) || isClassType(right.sourceType));
                    const bool classOperatorSupported =
                        ins.op == VMOperator::ADD || ins.op == VMOperator::EQUL ||
                        ins.op == VMOperator::NOT_EQUAL;
                    const auto isNativeString = [](const NativeValue& value) {
                        const auto* abi = value.dynamic
                            ? nullptr
                            : ZeusBuiltins::findNativeTypeABI(value.sourceType);
                        return abi != nullptr &&
                            abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString;
                    };
                    const bool stringAddition = ins.op == VMOperator::ADD &&
                        (isNativeString(left) || isNativeString(right));
                    const bool stringComparison =
                        (ins.op == VMOperator::EQUL || ins.op == VMOperator::NOT_EQUAL) &&
                        (isNativeString(left) || isNativeString(right));
                    const bool nativeStringConcatenation =
                        ins.op == VMOperator::ADD && !left.dynamic && !right.dynamic &&
                        isNativeString(left) && isNativeString(right);

                    if (nativeStringConcatenation) {
                        const std::string concatenated = freshNativeTmp();
                        emitNativeLine(concatenated +
                            " = call ptr @zeus_native_string_concat(ptr " +
                            left.value + ", ptr " + right.value + ")");
                        if (left.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " + left.value + ")");
                        }
                        if (right.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " + right.value + ")");
                        }
                        valueStack.push_back(NativeValue{
                            concatenated, "ptr", false, false, "string"
                        });
                        valueStack.back().ownsNativeString = true;
                        break;
                    }

                    if (!left.dynamic && !right.dynamic &&
                        isNativeString(left) && isNativeString(right) &&
                        (ins.op == VMOperator::EQUL || ins.op == VMOperator::NOT_EQUAL)) {
                        const std::string prefix = "zeus_string_eq_" +
                            std::to_string(ci) + "_" + std::to_string(pc) + "_" +
                            std::to_string(nativeTmpCount++);
                        const std::string checkLabel = prefix + "_check";
                        const std::string loopLabel = prefix + "_loop";
                        const std::string bodyLabel = prefix + "_body";
                        const std::string stepLabel = prefix + "_step";
                        const std::string smallLoopLabel = prefix + "_small_loop";
                        const std::string smallBodyLabel = prefix + "_small_body";
                        const std::string smallStepLabel = prefix + "_small_step";
                        const std::string tailLoopLabel = prefix + "_tail_loop";
                        const std::string tailBodyLabel = prefix + "_tail_body";
                        const std::string tailStepLabel = prefix + "_tail_step";
                        const std::string matchLabel = prefix + "_match";
                        const std::string mismatchLabel = prefix + "_mismatch";
                        const std::string mergeLabel = prefix + "_merge";

                        const std::string leftData = left.value;
                        const std::string rightData = right.value;

                        const std::string leftLengthAddress = freshNativeTmp();
                        const std::string rightLengthAddress = freshNativeTmp();
                        emitNativeLine(leftLengthAddress +
                            " = getelementptr i8, ptr " + left.value + ", i64 -8");
                        emitNativeLine(rightLengthAddress +
                            " = getelementptr i8, ptr " + right.value + ", i64 -8");
                        const std::string leftLength = freshNativeTmp();
                        const std::string rightLength = freshNativeTmp();
                        emitNativeLine(leftLength + " = load i64, ptr " +
                                       leftLengthAddress + ", align 8");
                        emitNativeLine(rightLength + " = load i64, ptr " +
                                       rightLengthAddress + ", align 8");
                        const std::string lengthsEqual = freshNativeTmp();
                        emitNativeLine(lengthsEqual + " = icmp eq i64 " + leftLength +
                                       ", " + rightLength);
                        emitNativeLine("br i1 " + lengthsEqual + ", label %" +
                                       checkLabel + ", label %" + mismatchLabel);

                        functionText += checkLabel + ":\n";
                        currentNativeBlock = "%" + checkLabel;
                        const std::string sameData = freshNativeTmp();
                        emitNativeLine(sameData + " = icmp eq ptr " + leftData +
                                       ", " + rightData);
                        emitNativeLine("br i1 " + sameData + ", label %" + matchLabel +
                                       ", label %" + loopLabel);

                        functionText += loopLabel + ":\n";
                        currentNativeBlock = "%" + loopLabel;
                        const std::string index = freshNativeTmp();
                        const std::string advancedIndex = freshNativeTmp();
                        emitNativeLine(index + " = phi i64 [ 0, %" + checkLabel +
                                       " ], [ " + advancedIndex + ", %" + stepLabel +
                                       " ]");
                        const std::string hasMore = freshNativeTmp();
                        const std::string remaining = freshNativeTmp();
                        emitNativeLine(remaining + " = sub i64 " + leftLength + ", " +
                                       index);
                        emitNativeLine(hasMore + " = icmp uge i64 " + remaining +
                                       ", 32");
                        emitNativeLine("br i1 " + hasMore + ", label %" + bodyLabel +
                                       ", label %" + smallLoopLabel);

                        functionText += bodyLabel + ":\n";
                        currentNativeBlock = "%" + bodyLabel;
                        std::string chunksEqual;
                        for (int chunk = 0; chunk < 4; ++chunk) {
                            std::string chunkOffset = index;
                            if (chunk > 0) {
                                chunkOffset = freshNativeTmp();
                                emitNativeLine(chunkOffset + " = add i64 " + index +
                                    ", " + std::to_string(chunk * 8));
                            }
                            const std::string leftChunkAddress = freshNativeTmp();
                            const std::string rightChunkAddress = freshNativeTmp();
                            emitNativeLine(leftChunkAddress +
                                " = getelementptr i8, ptr " + leftData + ", i64 " +
                                chunkOffset);
                            emitNativeLine(rightChunkAddress +
                                " = getelementptr i8, ptr " + rightData + ", i64 " +
                                chunkOffset);
                            const std::string leftChunk = freshNativeTmp();
                            const std::string rightChunk = freshNativeTmp();
                            emitNativeLine(leftChunk + " = load i64, ptr " +
                                           leftChunkAddress + ", align 1");
                            emitNativeLine(rightChunk + " = load i64, ptr " +
                                           rightChunkAddress + ", align 1");
                            const std::string chunkMatches = freshNativeTmp();
                            emitNativeLine(chunkMatches + " = icmp eq i64 " +
                                           leftChunk + ", " + rightChunk);
                            if (chunk == 0) {
                                chunksEqual = chunkMatches;
                            } else {
                                const std::string allChunksMatch = freshNativeTmp();
                                emitNativeLine(allChunksMatch + " = and i1 " +
                                    chunksEqual + ", " + chunkMatches);
                                chunksEqual = allChunksMatch;
                            }
                        }
                        emitNativeLine("br i1 " + chunksEqual + ", label %" + stepLabel +
                                       ", label %" + mismatchLabel);

                        functionText += stepLabel + ":\n";
                        currentNativeBlock = "%" + stepLabel;
                        emitNativeLine(advancedIndex + " = add i64 " + index + ", 32");
                        emitNativeLine("br label %" + loopLabel);

                        functionText += smallLoopLabel + ":\n";
                        currentNativeBlock = "%" + smallLoopLabel;
                        const std::string smallIndex = freshNativeTmp();
                        const std::string advancedSmallIndex = freshNativeTmp();
                        emitNativeLine(smallIndex + " = phi i64 [ " + index + ", %" +
                            loopLabel + " ], [ " + advancedSmallIndex + ", %" +
                            smallStepLabel + " ]");
                        const std::string hasSmallChunk = freshNativeTmp();
                        const std::string smallRemaining = freshNativeTmp();
                        emitNativeLine(smallRemaining + " = sub i64 " + leftLength +
                                       ", " + smallIndex);
                        emitNativeLine(hasSmallChunk + " = icmp uge i64 " +
                                       smallRemaining + ", 8");
                        emitNativeLine("br i1 " + hasSmallChunk + ", label %" +
                                       smallBodyLabel + ", label %" + tailLoopLabel);

                        functionText += smallBodyLabel + ":\n";
                        currentNativeBlock = "%" + smallBodyLabel;
                        const std::string leftSmallAddress = freshNativeTmp();
                        const std::string rightSmallAddress = freshNativeTmp();
                        emitNativeLine(leftSmallAddress +
                            " = getelementptr i8, ptr " + leftData + ", i64 " +
                            smallIndex);
                        emitNativeLine(rightSmallAddress +
                            " = getelementptr i8, ptr " + rightData + ", i64 " +
                            smallIndex);
                        const std::string leftSmallChunk = freshNativeTmp();
                        const std::string rightSmallChunk = freshNativeTmp();
                        emitNativeLine(leftSmallChunk + " = load i64, ptr " +
                                       leftSmallAddress + ", align 1");
                        emitNativeLine(rightSmallChunk + " = load i64, ptr " +
                                       rightSmallAddress + ", align 1");
                        const std::string smallChunksEqual = freshNativeTmp();
                        emitNativeLine(smallChunksEqual + " = icmp eq i64 " +
                                       leftSmallChunk + ", " + rightSmallChunk);
                        emitNativeLine("br i1 " + smallChunksEqual + ", label %" +
                                       smallStepLabel + ", label %" + mismatchLabel);

                        functionText += smallStepLabel + ":\n";
                        currentNativeBlock = "%" + smallStepLabel;
                        emitNativeLine(advancedSmallIndex + " = add i64 " + smallIndex +
                                       ", 8");
                        emitNativeLine("br label %" + smallLoopLabel);

                        functionText += tailLoopLabel + ":\n";
                        currentNativeBlock = "%" + tailLoopLabel;
                        const std::string tailIndex = freshNativeTmp();
                        const std::string advancedTailIndex = freshNativeTmp();
                        emitNativeLine(tailIndex + " = phi i64 [ " + smallIndex +
                                       ", %" + smallLoopLabel + " ], [ " +
                                       advancedTailIndex + ", %" + tailStepLabel + " ]");
                        const std::string tailHasMore = freshNativeTmp();
                        emitNativeLine(tailHasMore + " = icmp ult i64 " + tailIndex +
                                       ", " + leftLength);
                        emitNativeLine("br i1 " + tailHasMore + ", label %" +
                                       tailBodyLabel + ", label %" + matchLabel);

                        functionText += tailBodyLabel + ":\n";
                        currentNativeBlock = "%" + tailBodyLabel;
                        const std::string leftByteAddress = freshNativeTmp();
                        const std::string rightByteAddress = freshNativeTmp();
                        emitNativeLine(leftByteAddress + " = getelementptr i8, ptr " +
                                       leftData + ", i64 " + tailIndex);
                        emitNativeLine(rightByteAddress + " = getelementptr i8, ptr " +
                                       rightData + ", i64 " + tailIndex);
                        const std::string leftByte = freshNativeTmp();
                        const std::string rightByte = freshNativeTmp();
                        emitNativeLine(leftByte + " = load i8, ptr " + leftByteAddress +
                                       ", align 1");
                        emitNativeLine(rightByte + " = load i8, ptr " + rightByteAddress +
                                       ", align 1");
                        const std::string bytesEqual = freshNativeTmp();
                        emitNativeLine(bytesEqual + " = icmp eq i8 " + leftByte + ", " +
                                       rightByte);
                        emitNativeLine("br i1 " + bytesEqual + ", label %" +
                                       tailStepLabel + ", label %" + mismatchLabel);

                        functionText += tailStepLabel + ":\n";
                        currentNativeBlock = "%" + tailStepLabel;
                        emitNativeLine(advancedTailIndex + " = add i64 " + tailIndex +
                                       ", 1");
                        emitNativeLine("br label %" + tailLoopLabel);

                        functionText += matchLabel + ":\n";
                        currentNativeBlock = "%" + matchLabel;
                        emitNativeLine("br label %" + mergeLabel);

                        functionText += mismatchLabel + ":\n";
                        currentNativeBlock = "%" + mismatchLabel;
                        emitNativeLine("br label %" + mergeLabel);

                        functionText += mergeLabel + ":\n";
                        currentNativeBlock = "%" + mergeLabel;
                        std::string equal = freshNativeTmp();
                        emitNativeLine(equal + " = phi i1 [ true, %" + matchLabel +
                                       " ], [ false, %" + mismatchLabel + " ]");
                        if (left.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " + left.value + ")");
                        }
                        if (right.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " + right.value + ")");
                        }
                        if (ins.op == VMOperator::NOT_EQUAL) {
                            const std::string notEqual = freshNativeTmp();
                            emitNativeLine(notEqual + " = xor i1 " + equal + ", true");
                            equal = notEqual;
                        }
                        nativeBlockTerminated = false;
                        valueStack.push_back(NativeValue{
                            equal, "i1", false, false, "bool", false
                        });
                        break;
                    }

                    if (left.dynamic || right.dynamic ||
                        (classOperands && classOperatorSupported) || stringAddition ||
                        stringComparison) {
                        const int dynamicOpcode = DynamicBinaryABI::opcodeFor(ins.op);
                        if (dynamicOpcode < 0) {
                            return failNative(ins,
                                "[emit-llvm] ZeusValue 参数暂不支持该动态二元运算");
                        }

                        std::string leftDynamic;
                        std::string rightDynamic;
                        bool ownsLeftDynamic = false;
                        bool ownsRightDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, left, leftDynamic, ownsLeftDynamic) ||
                            !emitNativeDynamicValue(
                                ins, right, rightDynamic, ownsRightDynamic)) {
                            return false;
                        }

                        const std::string temporary = freshNativeTmp();
                        emitNativeLine(temporary + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_value_dynamic_binary(ptr " +
                                       temporary + ", ptr null, i32 " +
                                       std::to_string(dynamicOpcode) + ", ptr " +
                                       leftDynamic + ", ptr " + rightDynamic + ")");
                        if (ownsLeftDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           leftDynamic + ")");
                        }
                        if (ownsRightDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           rightDynamic + ")");
                        }
                        for (const NativeValue* operand : { &left, &right }) {
                            if (isManagedClassValue(*operand) && operand->ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               operand->value + ")");
                            }
                            if (operand->ownsNativeString) {
                                emitNativeLine("call void @zeus_string_release(ptr " +
                                               operand->value + ")");
                            }
                        }
                        valueStack.push_back(NativeValue{
                            temporary, "ptr", false, true, "", true
                        });
                        break;
                    }

                    if (isSignedIntegerLLVMType(left.type) &&
                        isSignedIntegerLLVMType(right.type) &&
                        left.sourceType != "char" && right.sourceType != "char") {
                        left.value = emitSignedIntegerConversion(
                            left.value, left.type, "i64", emitNativeLine, freshNativeTmp);
                        right.value = emitSignedIntegerConversion(
                            right.value, right.type, "i64", emitNativeLine, freshNativeTmp);
                        left.type = right.type = "i64";
                    }
                    std::string valueType;
                    std::string expression;
                    bool isComparison = false;

                    if (ins.op == VMOperator::LOGICAL_AND || ins.op == VMOperator::LOGICAL_OR) {
                        if (left.type != "i1" || right.type != "i1") {
                            return failNative(ins, "[emit-llvm] 逻辑运算要求 bool 操作数");
                        }
                        valueType = "i1";
                        expression = ins.op == VMOperator::LOGICAL_AND ? "and" : "or";
                    } else if (ins.op == VMOperator::EQUL || ins.op == VMOperator::NOT_EQUAL ||
                               ins.op == VMOperator::GREATER_THAN || ins.op == VMOperator::LESS_THAN ||
                               ins.op == VMOperator::GREATER_EQUAL || ins.op == VMOperator::LESS_EQUAL) {
                        if (left.type != right.type || (left.type != "i64" &&
                            left.type != "i1" && left.type != "i8")) {
                            return failNative(ins,
                                "[emit-llvm] 原生函数比较操作数类型不一致 (left=" +
                                left.type + ", source=" + left.sourceType +
                                ", dynamic=" + (left.dynamic ? "true" : "false") +
                                "; right=" + right.type + ", source=" + right.sourceType +
                                ", dynamic=" + (right.dynamic ? "true" : "false") + ")");
                        }
                        valueType = "i1";
                        isComparison = true;
                        if (ins.op == VMOperator::EQUL) expression = "eq";
                        if (ins.op == VMOperator::NOT_EQUAL) expression = "ne";
                        if (ins.op == VMOperator::GREATER_THAN) expression = "sgt";
                        if (ins.op == VMOperator::LESS_THAN) expression = "slt";
                        if (ins.op == VMOperator::GREATER_EQUAL) expression = "sge";
                        if (ins.op == VMOperator::LESS_EQUAL) expression = "sle";
                    } else {
                        if (left.type != "i64" || right.type != "i64") {
                            return failNative(ins, "[emit-llvm] 算术运算要求 int 操作数");
                        }
                        valueType = "i64";
                        if (ins.op == VMOperator::ADD) expression = "add";
                        if (ins.op == VMOperator::SUBTRACT) expression = "sub";
                        if (ins.op == VMOperator::MULTIPLY) expression = "mul";
                        if (ins.op == VMOperator::DIVIDE) expression = "sdiv";
                        if (ins.op == VMOperator::MODULUS) expression = "srem";
                    }

                    std::string temporary = freshNativeTmp();
                    const std::string llvmOpcode = isComparison
                        ? "icmp " + expression
                        : expression;
                    if (ins.op == VMOperator::DIVIDE || ins.op == VMOperator::MODULUS) {
                        emitNativeLine("call void @zeus_i64_check_division(i64 " +
                            left.value + ", i64 " + right.value + ", i32 " +
                            std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                    }
                    emitNativeLine(
                        temporary + " = " + llvmOpcode + " " + left.type + " " +
                        left.value + ", " + right.value
                    );
                    valueStack.push_back(NativeValue{ temporary, valueType, false });
                    break;
                }

                case VMOperator::LOAD_NATIVE: {
                    int64_t nativeId = 0;
                    if (!tryArgInt(ins, 0, nativeId)) {
                        return failNative(ins, "[emit-llvm] 原生函数 LOAD_NATIVE id 不是整数");
                    }
                    std::string nativeName;
                    if (!resolveNativeHandle(nativeId, nativeName)) {
                        return failNative(ins, "[emit-llvm] 原生函数句柄无有效 ABI 声明");
                    }
                    valueStack.push_back(NativeValue{
                        std::move(nativeName), "builtin", true, false
                    });
                    break;
                }

                case VMOperator::CALL_NATIVE: {
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        static_cast<size_t>(argc + 1) > valueStack.size()) {
                        return failNative(ins, "[emit-llvm] 原生动态变长调用参数数量或栈状态无效");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue callee;
                    if (!popNative(ins, callee) || !callee.functionRef ||
                        callee.type != "builtin") {
                        return failNative(ins, "[emit-llvm] CALL_NATIVE 缺少原生函数目标");
                    }

                    std::string resultValue;
                    if (const auto* nativeDeclaration =
                            findNativeBuiltinDeclaration(callee.value)) {
                        if (arguments.size() < static_cast<size_t>(
                                nativeDeclaration->minRequiredParams) ||
                            arguments.size() > nativeDeclaration->parameterTypes.size()) {
                            return failNative(ins,
                                "[emit-llvm] @native builtin call argument count does not match '" +
                                callee.value + "'");
                        }

                        exceptionCallArguments = arguments;
                        exceptionCallDynamicValues.clear();
                        std::vector<std::string> nativeArguments;
                        std::vector<bool> ownsDynamicArguments;
                        std::vector<std::string> nativeByteArguments;
                        std::vector<std::string> nativeStringArguments;
                        for (size_t i = 0; i < nativeDeclaration->parameterTypes.size(); ++i) {
                            const std::string& parameterType =
                                nativeDeclaration->parameterTypes[i];
                            if (i >= arguments.size()) {
                                NativeValue placeholder;
                                if (!emitOptionalArgumentPlaceholder(
                                        parameterType, placeholder, emitNativeLine,
                                        freshNativeTmp)) {
                                    return failNative(ins,
                                        "[emit-llvm] @native builtin parameter has no default ABI placeholder");
                                }
                                nativeArguments.push_back(placeholder.value);
                                ownsDynamicArguments.push_back(placeholder.dynamic &&
                                                               placeholder.ownsDynamic);
                                if (placeholder.dynamic && placeholder.ownsDynamic) {
                                    exceptionCallDynamicValues.push_back(placeholder.value);
                                }
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                                continue;
                            }

                            std::string nativeArgument;
                            bool ownsDynamicArgument = false;
                            bool ownsNativeStringArgument = false;
                            if (!emitFunctionABIArgument(
                                    ins, arguments[i], parameterType, nativeArgument,
                                    ownsDynamicArgument, true,
                                    &ownsNativeStringArgument)) {
                                return false;
                            }
                            if (ownsNativeStringArgument) {
                                nativeStringArguments.push_back(nativeArgument);
                                NativeValue cleanupValue{
                                    nativeArgument, "ptr", false, false, "string"
                                };
                                cleanupValue.ownsNativeString = true;
                                exceptionCallArguments.push_back(std::move(cleanupValue));
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                            }
                            nativeArguments.push_back(std::move(nativeArgument));
                            ownsDynamicArguments.push_back(ownsDynamicArgument);
                            if (ownsDynamicArgument) {
                                exceptionCallDynamicValues.push_back(nativeArguments.back());
                            } else if (parameterType == "bytes" &&
                                       arguments[i].dynamic) {
                                nativeByteArguments.push_back(nativeArguments.back());
                                exceptionCallDynamicValues.push_back(
                                    detail::nativeBytesCleanupToken(nativeArguments.back()));
                            }
                            exceptionCallScope(
                                true, exceptionCallArguments,
                                exceptionCallDynamicValues);
                        }

                        const bool returnsDynamic =
                            usesZeusValueABI(nativeDeclaration->returnType);
                        const std::string returnType = returnsDynamic
                            ? "void" : llvmType(nativeDeclaration->returnType);
                        if (returnType.empty()) {
                            return failNative(ins,
                                "[emit-llvm] @native builtin return type has no LLVM ABI");
                        }

                        resultValue = freshNativeTmp();
                        std::string callText;
                        if (returnsDynamic) {
                            emitNativeLine(resultValue + " = alloca %ZeusValue, align 8");
                            callText = "call void " + llvmCallTarget(*nativeDeclaration) +
                                "(ptr " + resultValue;
                        } else {
                            callText = resultValue + " = call " + returnType + " " +
                                llvmCallTarget(*nativeDeclaration) + "(";
                        }
                        for (size_t i = 0; i < nativeArguments.size(); ++i) {
                            if (i > 0 || returnsDynamic) callText += ", ";
                            callText += functionParameterABIType(
                                nativeDeclaration->parameterTypes[i]) + " " +
                                nativeArguments[i];
                        }
                        if (nativeDeclaration->hasDefaultParameters()) {
                            if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
                            callText += "i64 " + std::to_string(providedPrefixMask(arguments.size()));
                        }
                        callText += ")";
                        emitNativeLine(callText);
                        exceptionCallScope(false, {}, {});

                        const std::string nativeResultType =
                            isReferenceTypeName(nativeDeclaration->returnType)
                            ? referencedTypeName(nativeDeclaration->returnType)
                            : nativeDeclaration->returnType;
                        const bool nativeReturnsClass =
                            isClassType(nativeResultType);
                        const bool nativeReturnsOwnedReference =
                            isReferenceTypeName(nativeDeclaration->returnType) &&
                            nativeReturnsClass;
                        if (isReferenceTypeName(nativeDeclaration->returnType) &&
                            !nativeReturnsClass) {
                            return failNative(ins,
                                "->& native return requires a managed class value");
                        }
                        if (nativeReturnsClass) {
                            emitNativeLine(std::string("call void @zeus_class_ref_") +
                                (nativeReturnsOwnedReference
                                    ? "retain_owned" : "require_managed_live") +
                                "(ptr " + resultValue + ")");
                        }

                        std::set<std::string> releasedDynamicArguments;
                        for (size_t i = 0; i < nativeArguments.size(); ++i) {
                            if (ownsDynamicArguments[i] &&
                                releasedDynamicArguments.insert(nativeArguments[i]).second) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               nativeArguments[i] + ")");
                            }
                        }
                        for (const auto& nativeBytes : nativeByteArguments) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           nativeBytes + ")");
                        }
                        for (const auto& nativeString : nativeStringArguments) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           nativeString + ")");
                        }
                        for (const auto& argument : arguments) {
                            if (argument.dynamic && argument.ownsDynamic &&
                                releasedDynamicArguments.insert(argument.value).second) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               argument.value + ")");
                            }
                            if (argument.ownsNativeBytes) {
                                emitNativeLine("call void @zeus_bytes_release(ptr " +
                                               argument.value + ")");
                            }
                            if (argument.ownsNativeString) {
                                emitNativeLine("call void @zeus_string_release(ptr " +
                                               argument.value + ")");
                            }
                            if (isManagedClassValue(argument) && argument.ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               argument.value + ")");
                            }
                        }

                        if (returnsDynamic) {
                            valueStack.push_back(NativeValue{
                                resultValue, "ptr", false, true,
                                nativeDeclaration->returnType, true
                            });
                        } else {
                            const auto* returnABI = ZeusBuiltins::findNativeTypeABI(
                                nativeDeclaration->returnType);
                            const bool returnsNativeBytes = returnABI != nullptr &&
                                returnABI->valueABI ==
                                    ZeusBuiltins::NativeValueABI::NativeBytes;
                            const bool returnsNativeString = returnABI != nullptr &&
                                returnABI->valueABI ==
                                    ZeusBuiltins::NativeValueABI::NativeString;
                            if (ZeusBuiltins::isNativePointerABI(returnABI) &&
                                !returnsNativeBytes && !returnsNativeString) {
                                const std::string stableView = freshNativeTmp();
                                emitNativeLine(stableView + " = call ptr @" +
                                    std::string(ZeusBuiltins::nativePointerInternHelper(returnABI)) +
                                    "(ptr " + resultValue + ")");
                                resultValue = stableView;
                            }
                            valueStack.push_back(NativeValue{
                                resultValue, returnType, false, false,
                                nativeResultType, false
                            });
                            valueStack.back().ownsClass = nativeReturnsClass;
                            valueStack.back().ownsNativeBytes = returnsNativeBytes;
                            valueStack.back().ownsNativeString = returnsNativeString;
                        }
                        break;
                    }

                    std::vector<std::string> dynamicArguments(arguments.size());
                    std::vector<bool> ownsDynamicArguments(arguments.size(), false);
                    exceptionCallArguments = arguments;
                    exceptionCallDynamicValues.clear();
                    for (size_t i = 0; i < arguments.size(); ++i) {
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, arguments[i], dynamicArguments[i], ownsDynamic)) {
                            return false;
                        }
                        ownsDynamicArguments[i] = ownsDynamic;
                        if (ownsDynamic) {
                            exceptionCallDynamicValues.push_back(dynamicArguments[i]);
                        }
                        exceptionCallScope(
                            true, exceptionCallArguments,
                            exceptionCallDynamicValues);
                    }
                    const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
                    if (callSymbol.empty()) {
                        return failNative(ins,
                            "[emit-llvm] native call has no implementation ABI for '" +
                            callee.value + "'");
                    }
                    resultValue = emitDynamicVariadicCall(
                        callSymbol, callee.value, dynamicArguments,
                        emitNativeLine, freshNativeTmp);
                    exceptionCallScope(false, {}, {});
                    for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                        if (ownsDynamicArguments[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicArguments[i] + ")");
                        }
                        if (isManagedClassValue(arguments[i]) &&
                            arguments[i].ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           arguments[i].value + ")");
                        }
                        if (arguments[i].ownsNativeBytes) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           arguments[i].value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, "nil", true
                    });
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            return failNative(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
                        }
                        if (const auto* function = resolveFunctionSignature(
                                resolveCallableName(pendingDirectCall))) {
                            pendingDirectCall = function->name;
                        }
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0) {
                        return failNative(ins, "[emit-llvm] 原生函数只支持带直接调用标注的 CALL_CLOSURE");
                    }

                    // A captureless internal function reference has a complete
                    // typed ABI, so reuse the direct-call lowering below.
                    if (pendingDirectCall.empty() &&
                        static_cast<size_t>(argc) + 1 <= valueStack.size()) {
                        const size_t calleeIndex = valueStack.size() -
                            static_cast<size_t>(argc) - 1;
                        const NativeValue& candidate = valueStack[calleeIndex];
                        if (candidate.functionRef && candidate.type == "function") {
                            const auto target = functionSignatures.find(candidate.value);
                            if (target != functionSignatures.end() &&
                                !target->second.isExternal &&
                                target->second.captureCount == 0 &&
                                hasCompleteFunctionAnnotations(target->second)) {
                                pendingDirectCall = target->second.name;
                            }
                        }
                    }

                    if (pendingDirectCall.empty()) {
                        if (argc < 0 || static_cast<size_t>(argc) + 1 > valueStack.size()) {
                            return failNative(ins, "[emit-llvm] 原生动态变长调用参数数量或栈状态无效");
                        }

                        std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                        for (int64_t i = argc - 1; i >= 0; --i) {
                            if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                        }

                        NativeValue callee;
                        if (!popNative(ins, callee)) return false;
                        if (callee.dynamic) {
                            NativeValue closureResult;
                            if (!emitClosureInvoke(
                                    ins, callee, arguments, closureResult, nullptr)) {
                                return false;
                            }
                            valueStack.push_back(std::move(closureResult));
                            pendingDirectCall.clear();
                            break;
                        }
                        if (!callee.functionRef) {
                            return failNative(ins,
                                "[emit-llvm] indirect call target is neither a closure nor a native function "
                                "(type='" + callee.type + "', value='" + callee.value +
                                "', functionRef=" + (callee.functionRef ? "true" : "false") +
                                ", dynamic=" + (callee.dynamic ? "true" : "false") + ")");
                        }
                        const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
                        if (callSymbol.empty()) {
                            return failNative(ins,
                                "[emit-llvm] 原生函数 '" + callee.value +
                                "' 没有动态变长 ABI（type='" + callee.type +
                                "', pendingDirectCall='" + pendingDirectCall + "'）");
                        }

                        exceptionCallArguments = arguments;
                        exceptionCallDynamicValues.clear();
                        std::vector<std::string> dynamicArguments(arguments.size());
                        std::vector<bool> ownsDynamicArguments(arguments.size(), false);
                        for (size_t i = 0; i < arguments.size(); ++i) {
                            bool ownsDynamic = false;
                            if (!emitNativeDynamicValue(
                                    ins,
                                    arguments[i],
                                    dynamicArguments[i],
                                    ownsDynamic)) {
                                return false;
                            }
                            ownsDynamicArguments[i] = ownsDynamic;
                            if (ownsDynamic) {
                                exceptionCallDynamicValues.push_back(dynamicArguments[i]);
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                            }
                        }

                        const std::string resultValue = emitDynamicVariadicCall(
                            callSymbol,
                            callee.value,
                            dynamicArguments,
                            emitNativeLine,
                            freshNativeTmp
                        );
                        exceptionCallScope(false, {}, {});
                        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                            if (ownsDynamicArguments[i]) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               dynamicArguments[i] + ")");
                            }
                        }

                        valueStack.push_back(NativeValue{
                            resultValue, "ptr", false, true, "nil", true
                        });
                        break;
                    }

                    auto function = functionSignatures.find(pendingDirectCall);
                    if (function == functionSignatures.end()) {
                        return failNative(ins, "[emit-llvm] 找不到直接调用目标'" + pendingDirectCall + "'");
                    }

                    const auto& called = function->second;
                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue callee;
                    if (!popNative(ins, callee)) return false;
                    // A proven captureless function alias may have been stored
                    // in a ZeusValue slot. Keep the call statically typed by
                    // replacing that loaded value with the annotated target.
                    if (callee.dynamic &&
                        ins.metadata.tag == "static_function_alias") {
                        releaseNativeValueWithContext(callee);
                        callee = NativeValue{
                            pendingDirectCall, "function", true, false
                        };
                    }
                    if (callee.dynamic) {
                        NativeValue closureResult;
                        if (!emitClosureInvoke(
                                ins, callee, arguments, closureResult, &called)) {
                            return false;
                        }
                        valueStack.push_back(std::move(closureResult));
                        pendingDirectCall.clear();
                        break;
                    }
                    if (!callee.functionRef ||
                        !matchesSpecializedFunctionReference(
                            callee.value, pendingDirectCall)) {
                        return failNative(ins, "[emit-llvm] 原生函数调用缺少匹配的函数引用");
                    }

                    std::vector<NativeValue> orderedArguments;
                    std::vector<bool> providedArguments;
                    std::vector<NativeValue> variadicArguments;
                    std::vector<std::pair<std::string, NativeValue>> keywordArguments;
                    std::string bindingError;
                    if (!bindCallArguments(called, arguments, orderedArguments,
                            providedArguments, variadicArguments, keywordArguments,
                            bindingError)) {
                        return failNative(ins, "[emit-llvm] " + bindingError);
                    }
                    exceptionCallArguments = arguments;
                    exceptionCallDynamicValues.clear();

                    // 与声明端保持一致：显式 `-> void` 的外部声明按 C 的 void
                    // 函数直接 `call void @sym(args...)`，不分配也不传递
                    // ZeusValue 出参槽。空返回类型仍走动态出参 ABI。
                    // `@native func` 定义同理：它导出的是 C 链接函数，未标注
                    // 返回类型（IR 里落成 "nil"）时调用点也不能传 out 槽，
                    // 否则会多一个实参并释放一块从未初始化的栈内存。
                    const bool calledIsVoidReturn = called.isExternal
                        ? (called.returnType == "void" || called.returnType == "nil")
                        : (nativeExportFunctions.count(called.name) != 0 &&
                           (called.returnType.empty() ||
                            called.returnType == "nil" ||
                            called.returnType == "void"));
                    const bool calledReturnsDynamic =
                        !calledIsVoidReturn && usesZeusValueABI(called.returnType);
                    std::string calledReturnType = calledReturnsDynamic
                        ? "void"
                        : (calledIsVoidReturn ? "void" : llvmType(called.returnType));
                    if (calledReturnType.empty()) {
                        return failNative(ins, "[emit-llvm] 直接调用目标返回类型没有 LLVM ABI 映射");
                    }

                    std::string resultValue = freshNativeTmp();
                    if (calledReturnsDynamic) {
                        emitNativeLine(resultValue + " = alloca %ZeusValue, align 8");
                    }
                    std::string callText = calledReturnsDynamic
                        ? "call void " + llvmCallTarget(called.name) + "(ptr " + resultValue
                        : calledIsVoidReturn
                            ? "call void " + llvmCallTarget(called.name) + "("
                            : resultValue + " = call " + calledReturnType + " " + llvmCallTarget(called.name) + "(";
                    std::vector<std::string> dynamicCallArguments;
                    std::vector<bool> ownsDynamicCallArguments;
                    std::vector<std::string> nativeByteCallArguments;
                    std::vector<std::string> nativeStringCallArguments;
                    for (size_t i = 0; i < called.parameterTypes.size(); ++i) {
                        const bool byReference = i < called.parameterIsReference.size() &&
                            called.parameterIsReference[i];
                        if (!providedArguments[i]) {
                            if (byReference) {
                                return failNative(ins,
                                    "[emit-llvm] a reference parameter cannot use a default argument");
                            }
                            NativeValue placeholder;
                            if (!emitOptionalArgumentPlaceholder(
                                    called.parameterTypes[i], placeholder,
                                    emitNativeLine, freshNativeTmp)) {
                                return failNative(ins,
                                    "[emit-llvm] optional argument has no native ABI placeholder");
                            }
                            if (i > 0 || calledReturnsDynamic) callText += ", ";
                            callText += functionParameterABIType(
                                            called.parameterTypes[i], byReference) +
                                        " " + placeholder.value;
                            continue;
                        }
                        if (i > 0 || calledReturnsDynamic) callText += ", ";
                        std::string nativeArgument;
                        bool ownsDynamicArgument = false;
                        if (byReference) {
                            const std::string parameterName = i < called.parameterNames.size()
                                ? called.parameterNames[i]
                                : std::string();
                            const IRType::CallArgumentSource* source = nullptr;
                            for (const auto& candidate : ins.metadata.callArguments) {
                                const bool matches = candidate.namedParameter.empty()
                                    ? candidate.positionalIndex == static_cast<int>(i)
                                    : candidate.namedParameter == parameterName;
                                if (matches) {
                                    source = &candidate;
                                    break;
                                }
                            }
                            if (!source || !source->isAssignable) {
                                return failNative(ins,
                                    "[emit-llvm] a reference parameter requires a variable argument");
                            }
                            if (!emitReferenceForSymbol(source->source, ins, nativeArgument)) {
                                return false;
                            }
                            emitReferenceArgumentTypeCheck(
                                nativeArgument, called, i, emitNativeLine);
                            if (called.isExternal &&
                                called.nativeReferenceABI != "slot") {
                                const std::string slot = nativeArgument;
                                emitExternalReferenceArgument(
                                    called.parameterTypes[i], slot, nativeArgument,
                                    emitNativeLine, freshNativeTmp);
                            }
                        } else {
                            bool ownsNativeStringArgument = false;
                            if (orderedArguments[i].dynamic) {
                                const std::string nativeParameterType =
                                    llvmType(called.parameterTypes[i]);
                                if (!isSignedIntegerLLVMType(nativeParameterType) &&
                                    nativeParameterType != "i1") {
                                    emitDynamicArgumentTypeCheck(
                                        orderedArguments[i].value,
                                        called,
                                        i,
                                        emitNativeLine);
                                }
                            }
                            if (!emitFunctionABIArgumentWithContext(
                                    ins,
                                    orderedArguments[i],
                                    called.parameterTypes[i],
                                    nativeArgument,
                                    ownsDynamicArgument,
                                    true,
                                    &called,
                                    i,
                                    &ownsNativeStringArgument)) {
                                return false;
                            }
                            if (ownsNativeStringArgument) {
                                nativeStringCallArguments.push_back(nativeArgument);
                                NativeValue cleanupValue{
                                    nativeArgument, "ptr", false, false, "string"
                                };
                                cleanupValue.ownsNativeString = true;
                                exceptionCallArguments.push_back(std::move(cleanupValue));
                            }
                        }
                        callText += functionParameterABIType(
                                        called.parameterTypes[i], byReference) +
                                    " " + nativeArgument;
                        if (!byReference && usesZeusValueABI(called.parameterTypes[i])) {
                            dynamicCallArguments.push_back(nativeArgument);
                            ownsDynamicCallArguments.push_back(ownsDynamicArgument);
                            if (ownsDynamicArgument) {
                                exceptionCallDynamicValues.push_back(nativeArgument);
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                            }
                        }
                        if (!byReference && called.parameterTypes[i] == "bytes" &&
                            orderedArguments[i].dynamic) {
                            nativeByteCallArguments.push_back(nativeArgument);
                            exceptionCallDynamicValues.push_back(
                                detail::nativeBytesCleanupToken(nativeArgument));
                            exceptionCallScope(
                                true, exceptionCallArguments,
                                exceptionCallDynamicValues);
                        }
                    }
                    std::string packedArgs;
                    std::string packedKwargs;
                    if (called.hasVariadicArgs || called.hasVariadicKwargs) {
                        if (!emitVariadicContainers(
                                called.hasVariadicArgs, called.hasVariadicKwargs,
                                variadicArguments, keywordArguments,
                                [&](const NativeValue& value, std::string& out, bool& owns) {
                                    return emitNativeDynamicValue(ins, value, out, owns);
                                }, freshNativeTmp, emitNativeLine, packedArgs, packedKwargs)) {
                            return false;
                        }
                        if (called.hasVariadicArgs) {
                            if (!called.parameterTypes.empty() || calledReturnsDynamic) callText += ", ";
                            callText += "ptr " + packedArgs;
                            exceptionCallDynamicValues.push_back(packedArgs);
                        }
                        if (called.hasVariadicKwargs) {
                            if (!called.parameterTypes.empty() || calledReturnsDynamic ||
                                called.hasVariadicArgs) callText += ", ";
                            callText += "ptr " + packedKwargs;
                            exceptionCallDynamicValues.push_back(packedKwargs);
                        }
                        exceptionCallScope(
                            true, exceptionCallArguments,
                            exceptionCallDynamicValues);
                    }
                    if (called.hasDefaultParameters()) {
                        if (!called.parameterTypes.empty() || calledReturnsDynamic ||
                            called.hasVariadicArgs || called.hasVariadicKwargs) callText += ", ";
                        callText += "i64 " + std::to_string(providedArgumentMask(providedArguments));
                    }
                    callText += ")";

                    emitNativeLine(callText);
                    exceptionCallScope(false, {}, {});
                    if (!packedArgs.empty()) emitNativeLine("call void @zeus_value_release(ptr " + packedArgs + ")");
                    if (!packedKwargs.empty()) emitNativeLine("call void @zeus_value_release(ptr " + packedKwargs + ")");
                    const auto* calledReturnABI =
                        ZeusBuiltins::findNativeTypeABI(called.returnType);
                    const bool calledReturnsNativeBytes = !calledReturnsDynamic &&
                        calledReturnABI != nullptr && calledReturnABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::NativeBytes;
                    const bool calledReturnsNativeString = !calledReturnsDynamic &&
                        calledReturnABI != nullptr && calledReturnABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::NativeString;
                    if (!calledReturnsDynamic &&
                        ZeusBuiltins::isNativePointerABI(calledReturnABI) &&
                        !calledReturnsNativeBytes && !calledReturnsNativeString) {
                        const std::string stableView = freshNativeTmp();
                        emitNativeLine(stableView + " = call ptr @" +
                                       std::string(ZeusBuiltins::nativePointerInternHelper(
                                           calledReturnABI)) + "(ptr " +
                                       resultValue + ")");
                        resultValue = stableView;
                    }
                    std::set<std::string> releasedDynamicArguments;
                    for (size_t i = 0; i < dynamicCallArguments.size(); ++i) {
                        if (ownsDynamicCallArguments[i] &&
                            releasedDynamicArguments.insert(dynamicCallArguments[i]).second) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicCallArguments[i] + ")");
                        }
                    }
                    for (const auto& nativeBytes : nativeByteCallArguments) {
                        emitNativeLine("call void @zeus_bytes_release(ptr " +
                                       nativeBytes + ")");
                    }
                    for (const auto& nativeString : nativeStringCallArguments) {
                        emitNativeLine("call void @zeus_string_release(ptr " +
                                       nativeString + ")");
                    }
                    for (const auto& argument : arguments) {
                        if (argument.dynamic && argument.ownsDynamic &&
                            releasedDynamicArguments.insert(argument.value).second) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           argument.value + ")");
                        }
                        if (argument.ownsNativeBytes) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           argument.value + ")");
                        }
                        if (argument.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           argument.value + ")");
                        }
                        if (isManagedClassValue(argument) && argument.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           argument.value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue,
                        calledReturnsDynamic ? "ptr" : calledReturnType,
                        false,
                        calledReturnsDynamic,
                        isReferenceTypeName(called.returnType) &&
                                isClassType(referencedTypeName(called.returnType))
                            ? referencedTypeName(called.returnType)
                            : called.returnType,
                        calledReturnsDynamic
                    });
                    valueStack.back().ownsClass =
                        !calledReturnsDynamic && isClassType(called.returnType);
                    valueStack.back().ownsNativeBytes = calledReturnsNativeBytes;
                    valueStack.back().ownsNativeString = calledReturnsNativeString;
                    pendingDirectCall.clear();
                    break;
                }

                case VMOperator::OBJ_CALL_SAFE:
                case VMOperator::OBJ_CALL_METHOD: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] class method call is missing its method name");
                    }

                    // Safe navigation always dispatches dynamically: the
                    // receiver may be nil, and nil has no native form.
                    const bool safeNavCall =
                        (ins.op == VMOperator::OBJ_CALL_SAFE);
                    const char* dynamicCallSymbol = safeNavCall
                        ? "zeus_value_dynamic_safe_member_call"
                        : "zeus_value_dynamic_member_call";

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 1, argc) || argc < 0 ||
                        static_cast<size_t>(argc) + 1 > valueStack.size()) {
                        return failNative(ins, "[emit-llvm] class method call has invalid arguments");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc) + 1);
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i) + 1])) {
                            return false;
                        }
                    }
                    if (!popNative(ins, arguments[0])) return false;

                    auto convertArgument = [&] (
                        const NativeValue& value,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        std::string& cleanupValue
                    ) {
                        cleanupValue.clear();
                        if (usesZeusValueABI(declaredType)) {
                            bool ownsDynamic = false;
                            if (!emitNativeDynamicValue(
                                    ins, value, nativeArgument, ownsDynamic)) {
                                return false;
                            }
                            if (ownsDynamic) {
                                cleanupValue = nativeArgument;
                            } else if (declaredType == "bytes" && value.dynamic) {
                                cleanupValue = detail::nativeBytesCleanupToken(nativeArgument);
                            }
                            return true;
                        }

                        bool ignoredOwnership = false;
                        bool ownsNativeString = false;
                        if (!emitFunctionABIArgument(
                            ins, value, declaredType, nativeArgument,
                            ignoredOwnership, true, &ownsNativeString)) {
                            return false;
                        }
                        if (ownsNativeString) {
                            cleanupValue = detail::nativeStringCleanupToken(nativeArgument);
                        }
                        return true;
                    };
                    auto releaseDynamic = [&](const std::string& value) {
                        emitNativeLine("call void @zeus_value_release(ptr " + value + ")");
                    };
                    auto failMethod = [&](const IRType::IRInstr& at,
                                          const std::string& message) {
                        return failNative(at, message);
                    };

                    NativeValue callResult;
                    const std::string methodName = std::get<std::string>(ins.args[0]);
                    const bool hasTypedImplMethod = hasTypedInstanceMethodSignature(
                        arguments[0].sourceType, methodName);
                    const bool staticClassReceiver = arguments[0].type == "classref" &&
                        isClassType(arguments[0].sourceType);
                    std::string resolvedMethod;
                    const bool resolvedTypedMethod = ins.metadata.readDirectCall(resolvedMethod) &&
                        functionSignatures.find(resolvedMethod) != functionSignatures.end();
                    if (safeNavCall || (!resolvedTypedMethod && (arguments[0].dynamic ||
                        (arguments[0].type != "ptr" && !staticClassReceiver) ||
                        (!isClassType(arguments[0].sourceType) && !hasTypedImplMethod)))) {
                        auto toDynamic = [&](const NativeValue& value,
                                             std::string& dynamicValue,
                                             bool& ownsDynamic) {
                            return emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic);
                        };
                        if (!emitDynamicMemberCall(
                                ins,
                                methodName,
                                arguments,
                                callResult,
                                emitNativeLine,
                                freshNativeTmp,
                                toDynamic,
                                releaseDynamic,
                                failMethod,
                                exceptionCallScope,
                                dynamicCallSymbol)) {
                            return false;
                        }
                    } else {
                        auto emitMethodReference = [&] (
                            size_t parameterIndex,
                            const FunctionSignature& called,
                            std::string& nativeArgument
                        ) {
                            const size_t receiverOffset = called.isStaticMethod ? 0 : 1;
                            if (parameterIndex < receiverOffset) {
                                return failNative(ins,
                                    "[emit-llvm] method receiver cannot bind to a reference parameter");
                            }
                            const size_t userArgumentIndex = parameterIndex - receiverOffset;
                            const std::string parameterName = parameterIndex <
                                    called.parameterNames.size()
                                ? called.parameterNames[parameterIndex]
                                : std::string();
                            const IRType::CallArgumentSource* source = nullptr;
                            for (const auto& candidate : ins.metadata.callArguments) {
                                const bool matches = candidate.namedParameter.empty()
                                    ? candidate.positionalIndex ==
                                        static_cast<int>(userArgumentIndex)
                                    : candidate.namedParameter == parameterName;
                                if (matches) {
                                    source = &candidate;
                                    break;
                                }
                            }
                            if (!source || !source->isAssignable) {
                                return failNative(ins,
                                    "[emit-llvm] a method reference argument must be an assignable variable");
                            }
                            if (!emitReferenceForSymbol(source->source, ins, nativeArgument)) {
                                return false;
                            }
                            emitReferenceArgumentTypeCheck(
                                nativeArgument, called, parameterIndex,
                                [&](const std::string& line) { emitNativeLine(line); });
                            if (called.isExternal && called.nativeReferenceABI != "slot") {
                                const std::string slot = nativeArgument;
                                emitExternalReferenceArgument(
                                    called.parameterTypes[parameterIndex], slot,
                                    nativeArgument,
                                    [&](const std::string& line) { emitNativeLine(line); },
                                    [&]() { return freshNativeTmp(); });
                            }
                            return true;
                        };
                        if (!emitKnownClassMethodCall(
                                   ins,
                                   methodName,
                                   arguments,
                                   callResult,
                                   emitNativeLine,
                                   freshNativeTmp,
                                   convertArgument,
                                   releaseDynamic,
                                   failMethod,
                                   exceptionCallScope,
                                   emitMethodReference)) {
                            return false;
                        }
                    }
                    valueStack.push_back(std::move(callResult));
                    break;
                }

                case VMOperator::JUMP:
                case VMOperator::LOOP_BREAK: {
                    if (!valueStack.empty()) {
                        return failNative(ins,
                            "LLVM 函数无条件分支要求操作数栈为空");
                    }
                    const auto target = nativeBranchTargets.find(pc);
                    if (target == nativeBranchTargets.end()) {
                        return failNative(ins,
                            "LLVM 函数无条件分支缺少有效目标块");
                    }
                    if (target->second <= pc) {
                        if (emitFunctionSafepoints) {
                            emitNativeLine("call void @zeus_gc_safepoint()");
                        }
                    }
                    if (!recordNativeEdge(target->second, ins, pc)) return false;
                    emitTryDepthExits(
                        tryLevelBefore(pc) - tryLevelBefore(target->second));
                    emitNativeLine("br label " + nativeLabelForPc(target->second));
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::JUMP_IF_PARAM_PROVIDED: {
                    int64_t parameterIndex = -1;
                    if (!tryArgInt(ins, 0, parameterIndex) || parameterIndex < 0 ||
                        parameterIndex >= static_cast<int64_t>(
                            signature.defaultParameterMask.size()) ||
                        !signature.defaultParameterMask[static_cast<size_t>(parameterIndex)] ||
                        !signature.hasDefaultParameters()) {
                        return failNative(ins,
                            "LLVM default-parameter branch refers to a non-default parameter");
                    }
                    if (!valueStack.empty()) {
                        return failNative(ins,
                            "LLVM default-parameter branch requires an empty VM value stack");
                    }

                    const auto target = nativeBranchTargets.find(pc);
                    if (target == nativeBranchTargets.end()) {
                        return failNative(ins,
                            "LLVM default-parameter branch has no verified target");
                    }
                    const size_t fallthrough = pc + 1 > functionExitPc
                        ? functionExitPc
                        : pc + 1;
                    if (!promoteBorrowedDynamicLocalsForConditionalEdges(
                            target->second, fallthrough, ins)) {
                        return false;
                    }
                    if (!recordNativeEdge(target->second, ins, pc) ||
                        !recordNativeEdge(fallthrough, ins, pc)) {
                        return false;
                    }

                    const std::string isProvided = freshNativeTmp();
                    const std::string providedBit = freshNativeTmp();
                    emitNativeLine(providedBit + " = and i64 %provided.mask, " +
                                   std::to_string(uint64_t{1} << parameterIndex));
                    emitNativeLine(isProvided + " = icmp ne i64 " + providedBit + ", 0");
                    const int providedTargetExits =
                        tryLevelBefore(pc) - tryLevelBefore(target->second);
                    const int providedFallthroughExits =
                        tryLevelBefore(pc) - tryLevelBefore(fallthrough);
                    if (providedTargetExits > 0 || providedFallthroughExits > 0) {
                        std::string edgeBlocks;
                        const std::string takenLabel = tryExitEdgeTarget(
                            nativeLabelForPc(target->second), providedTargetExits,
                            target->second, pc, edgeBlocks);
                        const std::string skipLabel = tryExitEdgeTarget(
                            nativeLabelForPc(fallthrough), providedFallthroughExits,
                            fallthrough, pc, edgeBlocks);
                        emitNativeLine("br i1 " + isProvided + ", label " +
                                       takenLabel + ", label " + skipLabel);
                        functionText += edgeBlocks;
                    } else {
                        emitNativeLine("br i1 " + isProvided + ", label " +
                                       nativeLabelForPc(target->second) + ", label " +
                                       nativeLabelForPc(fallthrough));
                    }
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_TRUE: {
                    NativeValue condition;
                    if (!popNative(ins, condition) || condition.functionRef) {
                        return failNative(ins,
                            "LLVM 函数条件分支的条件值无效");
                    }
                    if (!valueStack.empty()) {
                        return failNative(ins,
                            "LLVM 函数条件分支要求条件值之外的操作数栈为空");
                    }

                    std::string booleanValue;
                    if (condition.dynamic) {
                        const std::string truthy = freshNativeTmp();
                        emitNativeLine(truthy +
                            " = call i32 @zeus_value_truthy(ptr " + condition.value + ")");
                        if (condition.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           condition.value + ")");
                        }
                        booleanValue = freshNativeTmp();
                        emitNativeLine(booleanValue +
                            " = icmp ne i32 " + truthy + ", 0");
                    } else if (condition.type == "i1") {
                        booleanValue = condition.value;
                    } else if (isSignedIntegerLLVMType(condition.type)) {
                        booleanValue = freshNativeTmp();
                        emitNativeLine(booleanValue + " = icmp ne " + condition.type + " " +
                                       condition.value + ", 0");
                    } else {
                        return failNative(ins,
                            "LLVM 函数条件分支暂不支持该条件类型");
                    }

                    const auto target = nativeBranchTargets.find(pc);
                    if (target == nativeBranchTargets.end()) {
                        return failNative(ins,
                            "LLVM 函数条件分支缺少已验证的目标块");
                    }
                    const size_t fallthrough = pc + 1 > functionExitPc
                        ? functionExitPc
                        : pc + 1;
                    if (target->second <= pc) {
                        if (emitFunctionSafepoints) {
                            emitNativeLine("call void @zeus_gc_safepoint()");
                        }
                    }
                    if (!promoteBorrowedDynamicLocalsForConditionalEdges(
                            target->second, fallthrough, ins)) {
                        return false;
                    }
                    if (!recordNativeEdge(target->second, ins, pc) ||
                        !recordNativeEdge(fallthrough, ins, pc)) {
                        return false;
                    }

                    const int targetExits =
                        tryLevelBefore(pc) - tryLevelBefore(target->second);
                    const int fallthroughExits =
                        tryLevelBefore(pc) - tryLevelBefore(fallthrough);
                    std::string edgeBlocks;
                    const std::string targetLabel = tryExitEdgeTarget(
                        nativeLabelForPc(target->second), targetExits,
                        target->second, pc, edgeBlocks);
                    const std::string fallthroughLabel = tryExitEdgeTarget(
                        nativeLabelForPc(fallthrough), fallthroughExits,
                        fallthrough, pc, edgeBlocks);
                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        emitNativeLine("br i1 " + booleanValue + ", label " +
                                       fallthroughLabel + ", label " + targetLabel);
                    } else {
                        emitNativeLine("br i1 " + booleanValue + ", label " +
                                       targetLabel + ", label " + fallthroughLabel);
                    }
                    functionText += edgeBlocks;
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO: {
                    NativeValue checked;
                    if (!popNative(ins, checked)) return false;
                    const std::string checkCall = rangeCheckCall(checked, ins);
                    if (!checkCall.empty()) emitNativeLine(checkCall);
                    if (checked.dynamic && checked.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       checked.value + ")");
                    }
                    break;
                }

                case VMOperator::RETURN: {
                    // A return leaves active try regions after return-value
                    // conversion and frame cleanup have completed.
                    if (definesVoidReturn) {
                        // C void 函数：`return;` 或 `return expr;` 都只做清理，
                        // 表达式结果不进入 ABI。
                        while (!valueStack.empty()) {
                            const NativeValue value = valueStack.back();
                            valueStack.pop_back();
                            if (value.dynamic && value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                            if (value.ownsClass && isManagedClassValue(value)) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               value.value + ")");
                            }
                            if (value.ownsNativeBytes) {
                                emitNativeLine("call void @zeus_bytes_release(ptr " +
                                               value.value + ")");
                            }
                            if (value.ownsNativeString) {
                                emitNativeLine("call void @zeus_string_release(ptr " +
                                               value.value + ")");
                            }
                        }
                        releaseOwnedFunctionLocals();
                        if (emitFunctionSafepoints) {
                            emitNativeLine("call void @zeus_gc_safepoint()");
                        }
                        emitTryDepthExits(tryLevelBefore(pc));
                        emitNativeLine("ret void");
                    } else if (returnsDynamic) {
                        NativeValue value;
                        if (!popNative(ins, value) || value.functionRef) {
                            return failNative(ins, "[emit-llvm] ZeusValue 返回值无效");
                        }
                        emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_clone(ptr %return.out, ptr " +
                                       dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        if (value.ownsNativeBytes) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           value.value + ")");
                        }
                        if (value.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           value.value + ")");
                        }
                        releaseOwnedFunctionLocals();
                        if (emitFunctionSafepoints) {
                            emitNativeLine("call void @zeus_gc_safepoint()");
                        }
                        emitTryDepthExits(tryLevelBefore(pc));
                        emitNativeLine("ret void");
                    } else {
                        NativeValue value;
                        if (!popNative(ins, value) || value.functionRef) {
                            return failNative(ins, "[emit-llvm] 原生函数返回值类型不匹配");
                        }
                        emitClassAssignmentCopy(ins, value, emitNativeLine, freshNativeTmp);
                        std::string nativeValue;
                        bool ignoredOwnership = false;
                        const std::string returnClassType =
                            isReferenceTypeName(signature.returnType)
                            ? referencedTypeName(signature.returnType)
                            : signature.returnType;
                        if (signature.returnType == "bytes") {
                            if (value.dynamic) {
                                nativeValue = freshNativeTmp();
                                emitNativeLine(nativeValue +
                                    " = call ptr @zeus_value_copy_bytes(ptr " +
                                    value.value + ")");
                                if (value.ownsDynamic) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value.value + ")");
                                }
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "bytes") {
                                nativeValue = freshNativeTmp();
                                emitNativeLine(nativeValue +
                                    " = call ptr @zeus_bytes_clone(ptr " +
                                    value.value + ")");
                                if (value.ownsNativeBytes) {
                                    emitNativeLine("call void @zeus_bytes_release(ptr " +
                                                   value.value + ")");
                                }
                            } else {
                                return failNative(ins,
                                    "bytes return requires a bytes value");
                            }
                        } else if (signature.returnType == "string") {
                            if (value.dynamic) {
                                nativeValue = freshNativeTmp();
                                emitNativeLine(nativeValue +
                                    " = call ptr @zeus_value_to_native_string(ptr " +
                                    value.value + ")");
                                if (value.ownsDynamic) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value.value + ")");
                                }
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "string") {
                                nativeValue = freshNativeTmp();
                                emitNativeLine(nativeValue +
                                    " = call ptr @zeus_string_retain(ptr " +
                                    value.value + ")");
                                if (value.ownsNativeString) {
                                    emitNativeLine("call void @zeus_string_release(ptr " +
                                                   value.value + ")");
                                }
                            } else {
                                return failNative(ins,
                                    "string return requires a string value");
                            }
                        } else if (!emitFunctionABIArgument(
                                       ins, value,
                                       isReferenceTypeName(signature.returnType) &&
                                               isClassType(returnClassType)
                                           ? returnClassType
                                           : signature.returnType,
                                       nativeValue, ignoredOwnership, false)) {
                            return false;
                        }
                        if (isClassType(returnClassType) && value.dynamic) {
                            emitNativeLine("call void @zeus_class_ref_retain_owned(ptr " +
                                           nativeValue + ")");
                            if (value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                        } else if (isClassType(returnClassType) &&
                                   isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_require_managed_live(ptr " +
                                           nativeValue + ")");
                            if (!value.ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_retain_owned(ptr " +
                                               nativeValue + ")");
                            }
                        }
                        releaseOwnedFunctionLocals();
                        if (emitFunctionSafepoints) {
                            emitNativeLine("call void @zeus_gc_safepoint()");
                        }
                        emitTryDepthExits(tryLevelBefore(pc));
                        emitNativeLine("ret " + returnType + " " + nativeValue);
                    }
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::CLEAR:
                    for (const auto& value : valueStack) {
                        if (value.dynamic && value.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                        } else if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        } else if (value.ownsNativeBytes) {
                            emitNativeLine("call void @zeus_bytes_release(ptr " +
                                           value.value + ")");
                        } else if (value.ownsNativeString) {
                            emitNativeLine("call void @zeus_string_release(ptr " +
                                           value.value + ")");
                        }
                    }
                    valueStack.clear();
                    break;

                case VMOperator::COPY: {
                    if (valueStack.empty()) {
                        return failNative(ins, "[emit-llvm] 原生函数值栈无法 COPY");
                    }
                    NativeValue copy = valueStack.back();
                    if (isManagedClassValue(copy) && copy.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                       copy.value + ")");
                    } else if (copy.type == "ptr" && copy.sourceType == "bytes") {
                        const std::string cloned = freshNativeTmp();
                        emitNativeLine(cloned + " = call ptr @zeus_bytes_clone(ptr " +
                                       copy.value + ")");
                        copy.value = cloned;
                        copy.ownsNativeBytes = true;
                    } else if (copy.type == "ptr" && copy.sourceType == "string") {
                        const std::string retained = freshNativeTmp();
                        emitNativeLine(retained + " = call ptr @zeus_string_retain(ptr " +
                                       copy.value + ")");
                        copy.value = retained;
                        copy.ownsNativeString = true;
                    } else if (copy.dynamic && copy.ownsDynamic) {
                        NativeValue cloned;
                        if (!cloneNativeDynamicValue(ins, copy, cloned)) return false;
                        copy = std::move(cloned);
                    }
                    valueStack.push_back(std::move(copy));
                    break;
                }

                case VMOperator::SWAP:
                    if (valueStack.size() < 2) {
                        return failNative(ins, "[emit-llvm] 原生函数值栈无法 SWAP");
                    }
                    std::swap(valueStack[valueStack.size() - 1],
                              valueStack[valueStack.size() - 2]);
                    break;

                case VMOperator::POP: {
                    NativeValue value;
                    if (!popNative(ins, value)) return false;
                    if (value.dynamic && value.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       value.value + ")");
                    } else if (value.ownsClass && isManagedClassValue(value)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       value.value + ")");
                    } else if (value.ownsNativeBytes) {
                        emitNativeLine("call void @zeus_bytes_release(ptr " +
                                       value.value + ")");
                    } else if (value.ownsNativeString) {
                        emitNativeLine("call void @zeus_string_release(ptr " +
                                       value.value + ")");
                    }
                    break;
                }

                case VMOperator::ASSIGN_VALUE_UPVALUE: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || !popNative(ins, value) ||
                        index < 0 || index >= signature.captureCount) {
                        return failNative(ins, "[emit-llvm] upvalue assignment slot is invalid");
                    }
                    std::string dynamicValue;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) return false;
                    emitNativeLine("call void @zeus_closure_env_store(ptr %closure.env, i32 " +
                                   std::to_string(index) + ", ptr " + dynamicValue + ")");
                    if (ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                    }
                    break;
                }

                case VMOperator::CAPTURE_OUTSIDE_VAR:
                    break;

                default:
                    return failNative(ins, "[emit-llvm] 原生函数暂不支持操作 " + VMO_toString(ins.op));
            }

            if (!nativeBlockTerminated && !recordNativeFallthrough(pc, ins)) {
                return false;
            }
        }

        if (nativeExitReachable && functionExitPc == chunk.size()) {
            if (chunk.empty() || !enterNativeBlock(functionExitPc, chunk.back()) ||
                !emitImplicitNilReturn()) {
                return false;
            }
        }

        for (const NativeLoopPhiPatch& patch : nativeLoopPhiPatches) {
            const auto incoming = nativeIncomingStates.find(patch.headerPc);
            if (incoming == nativeIncomingStates.end()) {
                result.error = "[emit-llvm] loop phi is missing incoming states";
                return false;
            }
            const auto edge = std::find_if(
                incoming->second.begin(), incoming->second.end(),
                [&](const NativeIncomingState& state) {
                    return state.sourcePc == patch.sourcePc;
                }
            );
            if (edge == incoming->second.end()) {
                result.error = "[emit-llvm] loop phi is missing a backedge";
                return false;
            }
            const auto local = edge->locals.find(patch.localIndex);
            const bool dynamicPointerTypesCompatible =
                local != edge->locals.end() && patch.dynamic &&
                local->second.dynamic && patch.type == "ptr" &&
                local->second.type == "ptr";
            bool scalarSourceTypesCompatible = false;
            if (local != edge->locals.end() &&
                !patch.dynamic && !local->second.dynamic &&
                (isSignedIntegerLLVMType(patch.type) || patch.type == "i1")) {
                // Scalar ABI types carry the value representation. Source type
                // metadata may be absent on a generated range index, or name
                // an alias such as int32/int64 that shares the same ABI.
                // Pointer values keep the stricter nominal type check below.
                if (patch.sourceType.empty() || local->second.sourceType.empty()) {
                    scalarSourceTypesCompatible = true;
                } else {
                    const auto* expectedABI = ZeusBuiltins::findNativeTypeABI(
                        patch.sourceType);
                    const auto* actualABI = ZeusBuiltins::findNativeTypeABI(
                        local->second.sourceType);
                    scalarSourceTypesCompatible = expectedABI != nullptr &&
                        actualABI != nullptr &&
                        expectedABI->llvmType == actualABI->llvmType &&
                        expectedABI->valueABI == actualABI->valueABI;
                }
            }
            if (local == edge->locals.end() ||
                local->second.type != patch.type ||
                local->second.dynamic != patch.dynamic ||
                (local->second.sourceType != patch.sourceType &&
                 !scalarSourceTypesCompatible &&
                 !dynamicPointerTypesCompatible) ||
                local->second.functionRef ||
                (patch.dynamic && !local->second.ownsDynamic)) {
                result.error = "[emit-llvm] loop-carried local changed ABI type or ownership in '" +
                    signature.name + "' (local " +
                    std::to_string(patch.localIndex) + ", expected type=" + patch.type +
                    " dynamic=" + (patch.dynamic ? "true" : "false") +
                    " sourceType='" + patch.sourceType + "', actual type=" +
                    (local == edge->locals.end() ? "<missing>" : local->second.type) +
                    " dynamic=" + (local == edge->locals.end() || !local->second.dynamic ? "false" : "true") +
                    " sourceType='" + (local == edge->locals.end() ? "<missing>" : local->second.sourceType) +
                    "' ownsDynamic=" +
                    (local != edge->locals.end() && local->second.ownsDynamic ? "true" : "false") + ")";
                return false;
            }

            const size_t value = functionText.find(patch.placeholder);
            if (value == std::string::npos) {
                result.error = "[emit-llvm] loop phi placeholder was lost during lowering";
                return false;
            }
            functionText.replace(value, patch.placeholder.size(), local->second.value);

            const size_t label = functionText.find(patch.labelPlaceholder);
            if (label == std::string::npos) {
                result.error = "[emit-llvm] loop phi predecessor placeholder was lost";
                return false;
            }
            functionText.replace(label, patch.labelPlaceholder.size(), edge->predecessor);
        }

        if (exceptionEdgeFailed) {
            if (result.error.empty()) result.error = "[emit-llvm] exception edge could not merge local state";
            return false;
        }
        for (const NativeExceptionPad& pad : exceptionPads) {
            if (isMCUMode()) continue;
            if (pad.cleanupOnly) {
                const std::string cleanupToken = freshNativeTmp();
                functionText += pad.switchLabel.substr(1) + ":\n";
                functionText += "  " + cleanupToken + " = cleanuppad within none []\n";
                for (const auto& cleanupCall : pad.cleanupCalls) {
                    functionText += "  " + cleanupCall;
                    if (cleanupCall.rfind("call ", 0) == 0) {
                        functionText += " [ \"funclet\"(token " + cleanupToken + ") ]";
                    }
                    functionText += "\n";
                }
                functionText += "  cleanupret from " + cleanupToken +
                    " unwind to caller\n";
                continue;
            }
            const std::string switchToken = freshNativeTmp();
            const std::string catchToken = freshNativeTmp();
            functionText += pad.switchLabel.substr(1) + ":\n";
            functionText += "  " + switchToken +
                " = catchswitch within none [label " + pad.padLabel +
                "] unwind to caller\n";
            functionText += pad.padLabel.substr(1) + ":\n";
            functionText += "  " + catchToken + " = catchpad within " + switchToken +
                " [ptr null, i32 64, ptr null]\n";
            functionText += "  call void @zeus_exception_capture(ptr " + pad.valueSlot +
                ", ptr @__zeus_make_runtime_error, ptr @__zeus_make_type_error) [ \"funclet\"(token " + catchToken + ") ]\n";
            for (const auto& cleanupCall : pad.cleanupCalls) {
                functionText += "  " + cleanupCall;
                if (cleanupCall.rfind("call ", 0) == 0) {
                    functionText += " [ \"funclet\"(token " + catchToken + ") ]";
                }
                functionText += "\n";
            }
            functionText += "  catchret from " + catchToken + " to label " +
                pad.resumeLabel + "\n";
            functionText += pad.resumeLabel.substr(1) + ":\n";
            functionText += "  br label " + nativeLabelForPc(pad.catchPc) + "\n";
        }

        // Hoist the body allocas to the function entry: an alloca inside a loop
        // grows the frame on every iteration at -O0.
        std::string hoistedAllocas;
        const std::string hoistedBody = takeAllocas(functionText, nativeEntryBody, hoistedAllocas);
        functionText = functionText.substr(0, nativeEntryBody) + hoistedAllocas + hoistedBody;
        functionText += "}\n\n";
        nativeFunctions += functionText;

        if (needsClosureAdapter(ci)) {
        std::string adapterText;
        adapterText += "define void " + closureAdapterSymbol(ci) +
            "(ptr %out, ptr %env, i32 %argc, ptr %args, ptr %argument.refs, ptr %argument.names) {\nentry:\n";
        std::vector<std::string> adapterArguments;
        adapterArguments.reserve(signature.parameterTypes.size());
        int adapterTmp = 0;
        auto freshAdapterTmp = [&]() {
            return std::string("%a") + std::to_string(adapterTmp++);
        };
        auto emitAdapterLine = [&](const std::string& line) {
            adapterText += "  " + line + "\n";
        };

        const size_t regularParameterCount = signature.parameterTypes.size();
        if (signature.parameterNames.size() != regularParameterCount) {
            result.error = "[emit-llvm] closure signature '" + signature.name +
                "' has incomplete parameter-name metadata (names=" +
                std::to_string(signature.parameterNames.size()) + ", parameters=" +
                std::to_string(regularParameterCount) + ")";
            return false;
        }

        std::string orderedArgumentArray = "null";
        if (regularParameterCount != 0) {
            orderedArgumentArray = freshAdapterTmp();
            emitAdapterLine(orderedArgumentArray + " = alloca ptr, i64 " +
                            std::to_string(regularParameterCount) + ", align 8");
        }
        std::string orderedReferenceArray = "null";
        if (regularParameterCount != 0) {
            orderedReferenceArray = freshAdapterTmp();
            emitAdapterLine(orderedReferenceArray + " = alloca ptr, i64 " +
                            std::to_string(regularParameterCount) + ", align 8");
        }
        std::string parameterNameArray = "null";
        if (regularParameterCount != 0) {
            parameterNameArray = freshAdapterTmp();
            emitAdapterLine(parameterNameArray + " = alloca ptr, i64 " +
                            std::to_string(regularParameterCount) + ", align 8");
            for (size_t i = 0; i < regularParameterCount; ++i) {
                const std::string slot = freshAdapterTmp();
                const std::string nameSymbol = poolStr(signature.parameterNames[i]);
                const std::string namePointer = freshAdapterTmp();
                emitAdapterLine(slot + " = getelementptr inbounds ptr, ptr " +
                    parameterNameArray + ", i64 " + std::to_string(i));
                emitAdapterLine(namePointer + " = getelementptr inbounds [" +
                    std::to_string(signature.parameterNames[i].size() + 1) +
                    " x i8], ptr " + nameSymbol + ", i64 0, i64 0");
                emitAdapterLine("store ptr " + namePointer + ", ptr " + slot +
                    ", align 8");
            }
        }

        const std::string providedMaskStorage = freshAdapterTmp();
        emitAdapterLine(providedMaskStorage + " = alloca i64, align 8");
        std::string variadicArgsValue = "null";
        if (signature.hasVariadicArgs) {
            variadicArgsValue = freshAdapterTmp();
            emitAdapterLine(variadicArgsValue + " = alloca %ZeusValue, align 8");
        }
        std::string variadicKwargsValue = "null";
        if (signature.hasVariadicKwargs) {
            variadicKwargsValue = freshAdapterTmp();
            emitAdapterLine(variadicKwargsValue + " = alloca %ZeusValue, align 8");
        }
        emitAdapterLine(
            "call void @zeus_closure_bind_arguments(i32 " +
            std::to_string(regularParameterCount) + ", ptr " + parameterNameArray +
            ", i64 " + std::to_string(providedArgumentMask(
                signature.defaultParameterMask)) +
            ", i32 " + std::to_string(signature.hasVariadicArgs ? 1 : 0) +
            ", i32 " + std::to_string(signature.hasVariadicKwargs ? 1 : 0) +
            ", i32 %argc, ptr %args, ptr %argument.refs, ptr %argument.names, ptr " +
            orderedArgumentArray + ", ptr " + orderedReferenceArray +
            ", ptr " + providedMaskStorage +
            ", ptr " + variadicArgsValue + ", ptr " + variadicKwargsValue + ")");
        const std::string providedMaskValue = freshAdapterTmp();
        emitAdapterLine(providedMaskValue + " = load i64, ptr " +
                        providedMaskStorage + ", align 8");

        for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
            const std::string& declaredType = signature.parameterTypes[i];
            const bool declaredNullable = !declaredType.empty() &&
                declaredType.back() == '?';
            const bool byReference = i < signature.parameterIsReference.size() &&
                signature.parameterIsReference[i];
            const std::string abiType = functionParameterABIType(
                declaredType, byReference);
            auto emitProvidedArgument = [&](std::string& nativeArgument) {
                const std::string slot = freshAdapterTmp();
                const std::string dynamicArgument = freshAdapterTmp();
                emitAdapterLine(slot + " = getelementptr inbounds ptr, ptr " +
                                orderedArgumentArray + ", i64 " + std::to_string(i));
                emitAdapterLine(dynamicArgument + " = load ptr, ptr " + slot + ", align 8");
                if (byReference) {
                    const std::string referenceSlot = freshAdapterTmp();
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(referenceSlot +
                        " = getelementptr inbounds ptr, ptr " +
                        orderedReferenceArray + ", i64 " + std::to_string(i));
                    emitAdapterLine(nativeArgument + " = load ptr, ptr " +
                        referenceSlot + ", align 8");
                    const size_t receiverOffset =
                        !signature.ownerType.empty() && !signature.isStaticMethod
                            ? 1 : 0;
                    const size_t userArgumentIndex = i > receiverOffset
                        ? i - receiverOffset
                        : 0;
                    emitAdapterLine(
                        "call void @zeus_value_slot_validate_argument_type(ptr " +
                        nativeArgument + ", ptr " + dynamicArgument + ", ptr " +
                        poolStr(signature.name) + ", i32 " +
                        std::to_string(userArgumentIndex) + ", ptr " +
                        poolStr(declaredType) + ")");
                    if (signature.isExternal &&
                        signature.nativeReferenceABI != "slot") {
                        const std::string slot = nativeArgument;
                        emitExternalReferenceArgument(
                            declaredType, slot, nativeArgument,
                            emitAdapterLine, freshAdapterTmp);
                    }
                    return true;
                }
                nativeArgument = dynamicArgument;
                // Nullable (`?`) parameters stay dynamic in phase 1: nil has
                // no native representation, so the box passes straight through
                // (nil included) instead of converting to native.
                if (declaredNullable) return true;
                if (usesZeusValueABI(declaredType)) return true;

                const std::string expectedType = llvmType(declaredType);
                if (expectedType == "double" || expectedType == "float") {
                    nativeArgument = emitNativeFloatingValueConversion(
                        dynamicArgument, "ptr", {}, expectedType,
                        emitAdapterLine, freshAdapterTmp);
                } else if (isIntegerLLVMType(expectedType) && (expectedType != "i64" ||
                    ZeusBuiltins::findNativeIntegerABI(declaredType))) {
                    nativeArgument = emitNativeIntegerValueConversion(dynamicArgument, "ptr", {},
                        expectedType, declaredType, emitAdapterLine, freshAdapterTmp);
                } else if (expectedType == "i64") {
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(nativeArgument + " = call i64 @zeus_value_require_i64(ptr " +
                                    dynamicArgument + ")");
                } else if (expectedType == "i1") {
                    const std::string raw = freshAdapterTmp();
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(raw + " = call i32 @zeus_value_require_bool(ptr " +
                                    dynamicArgument + ")");
                    emitAdapterLine(nativeArgument + " = icmp ne i32 " + raw + ", 0");
                } else if (expectedType == "%ZeusRawBytesRange" &&
                           declaredType == "&bytes") {
                    const std::string storage = freshAdapterTmp();
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(storage + " = alloca %ZeusRawBytesRange, align 8");
                    emitAdapterLine(
                        "call void @zeus_value_require_raw_bytes_range(ptr " +
                        storage + ", ptr " + dynamicArgument + ")");
                    emitAdapterLine(nativeArgument +
                        " = load %ZeusRawBytesRange, ptr " + storage +
                        ", align 8");
                } else if (expectedType == "ptr" && isReferenceTypeName(declaredType)) {
                    const std::string referenced = referencedTypeName(declaredType);
                    nativeArgument = freshAdapterTmp();
                    if (referenced == "int" || referenced == "int64") {
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_require_i64_ptr(ptr " + dynamicArgument + ")");
                    } else if (referenced == "bool") {
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_require_bool_ptr(ptr " + dynamicArgument + ")");
                    } else if (isClassType(referenced)) {
                        const std::string typeName = poolStr(referenced);
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_require_class_ptr(ptr " + dynamicArgument +
                            ", ptr " + typeName + ")");
                    } else {
                        result.error = "[emit-llvm] dynamic closure ABI cannot pass reference parameter '" +
                            declaredType + "': it transports ZeusValue arguments, not a caller variable slot";
                        return false;
                    }
                } else if (expectedType == "ptr" && isClassType(declaredType)) {
                    const std::string typeName = poolStr(declaredType);
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(nativeArgument +
                        " = call ptr @zeus_value_require_class_ptr(ptr " + dynamicArgument +
                        ", ptr " + typeName + ")");
                } else if (expectedType == "ptr") {
                    const auto* abi = ZeusBuiltins::findNativeTypeABI(declaredType);
                    if (ZeusBuiltins::isNativePointerABI(abi)) {
                        nativeArgument = freshAdapterTmp();
                        emitAdapterLine(nativeArgument + " = call ptr @" +
                            std::string(ZeusBuiltins::nativePointerFromDynamicHelper(abi)) +
                            "(ptr " +
                            dynamicArgument + ")");
                    } else {
                        result.error = "[emit-llvm] closure ABI cannot unbox parameter '" +
                            declaredType + "'";
                        return false;
                    }
                } else {
                    result.error = "[emit-llvm] closure ABI has no native parameter conversion for '" +
                        declaredType + "'";
                    return false;
                }
                return true;
            };

            std::string nativeArgument;
            const bool hasDefault = i < signature.defaultParameterMask.size() &&
                signature.defaultParameterMask[i];
            if (hasDefault) {
                const std::string suffix = std::to_string(i);
                const std::string providedLabel = "param_" + suffix + "_provided";
                const std::string missingLabel = "param_" + suffix + "_missing";
                const std::string joinLabel = "param_" + suffix + "_join";
                const std::string providedBit = freshAdapterTmp();
                const std::string isProvided = freshAdapterTmp();
                emitAdapterLine(providedBit + " = and i64 " + providedMaskValue + ", " +
                                std::to_string(uint64_t{1} << i));
                emitAdapterLine(isProvided + " = icmp ne i64 " + providedBit + ", 0");
                emitAdapterLine("br i1 " + isProvided + ", label %" + providedLabel +
                                ", label %" + missingLabel);

                adapterText += providedLabel + ":\n";
                std::string providedValue;
                if (!emitProvidedArgument(providedValue)) return false;
                emitAdapterLine("br label %" + joinLabel);

                adapterText += missingLabel + ":\n";
                NativeValue placeholder;
                if (byReference) {
                    const std::string missingValue = freshAdapterTmp();
                    emitAdapterLine(missingValue + " = alloca %ZeusValue, align 8");
                    emitAdapterLine("call void @zeus_value_init_nil(ptr " +
                                    missingValue + ")");
                    const size_t receiverOffset =
                        !signature.ownerType.empty() && !signature.isStaticMethod
                            ? 1 : 0;
                    const size_t userArgumentIndex = i > receiverOffset
                        ? i - receiverOffset
                        : 0;
                    emitAdapterLine(
                        "call void @zeus_value_slot_validate_argument_type(ptr null, ptr " +
                        missingValue + ", ptr " + poolStr(signature.name) +
                        ", i32 " + std::to_string(userArgumentIndex) + ", ptr " +
                        poolStr(declaredType) + ")");
                    emitAdapterLine("unreachable");
                } else {
                    if (!emitOptionalArgumentPlaceholder(
                            declaredType,
                            placeholder,
                            emitAdapterLine,
                            freshAdapterTmp)) {
                        result.error = "[emit-llvm] closure ABI has no default placeholder for '" +
                            declaredType + "'";
                        return false;
                    }
                    emitAdapterLine("br label %" + joinLabel);
                }

                adapterText += joinLabel + ":\n";
                nativeArgument = freshAdapterTmp();
                if (byReference) {
                    emitAdapterLine(nativeArgument + " = phi " + abiType + " [ " +
                        providedValue + ", %" + providedLabel + " ]");
                } else {
                    emitAdapterLine(nativeArgument + " = phi " + abiType + " [ " +
                        providedValue + ", %" + providedLabel + " ], [ " +
                        placeholder.value + ", %" + missingLabel + " ]");
                }
            } else if (!emitProvidedArgument(nativeArgument)) {
                return false;
            }
            adapterArguments.push_back(abiType + " " + nativeArgument);
        }

        const bool adapterReturnsDynamic = usesZeusValueABI(signature.returnType);
        const std::string adapterReturnType = adapterReturnsDynamic
            ? "void" : llvmType(signature.returnType);
        if (adapterReturnType.empty()) {
            result.error = "[emit-llvm] closure ABI has no return conversion for '" +
                signature.returnType + "'";
            return false;
        }
        std::vector<std::string> callArguments;
        if (signature.captureCount != 0) callArguments.push_back("ptr %env");
        if (adapterReturnsDynamic) callArguments.push_back("ptr %out");
        callArguments.insert(callArguments.end(), adapterArguments.begin(), adapterArguments.end());
        std::vector<std::string> variadicAdapterValues;
        if (signature.hasVariadicArgs) {
            callArguments.push_back("ptr " + variadicArgsValue);
            variadicAdapterValues.push_back(variadicArgsValue);
        }
        if (signature.hasVariadicKwargs) {
            callArguments.push_back("ptr " + variadicKwargsValue);
            variadicAdapterValues.push_back(variadicKwargsValue);
        }
        if (signature.hasDefaultParameters()) {
            callArguments.push_back("i64 " + providedMaskValue);
        }
        std::string callText;
        std::string nativeReturn;
        if (adapterReturnsDynamic) {
            callText = "call void " + llvmCallTarget(signature) + "(";
        } else {
            nativeReturn = freshAdapterTmp();
            callText = nativeReturn + " = call " + adapterReturnType + " " +
                llvmCallTarget(signature) + "(";
        }
        for (size_t i = 0; i < callArguments.size(); ++i) {
            if (i != 0) callText += ", ";
            callText += callArguments[i];
        }
        callText += ")";
        emitAdapterLine(callText);
        for (const auto& value : variadicAdapterValues) {
            emitAdapterLine("call void @zeus_value_release(ptr " + value + ")");
        }
        if (!adapterReturnsDynamic) {
            const auto* nativeIntegerReturn = ZeusBuiltins::findNativeIntegerABI(
                signature.returnType);
            const auto* unsignedReturn = ZeusBuiltins::findNativeUnsignedIntegerABI(
                signature.returnType);
            if (unsignedReturn != nullptr) {
                std::string wide = nativeReturn;
                if (unsignedReturn->integerBits < 64) {
                    wide = freshAdapterTmp();
                    emitAdapterLine(wide + " = zext " + adapterReturnType + " " +
                                    nativeReturn + " to i64");
                }
                emitAdapterLine("call void @zeus_value_init_unsigned_integer(ptr %out, i64 " +
                    wide + ", i32 " + std::to_string(unsignedReturn->integerBits) + ")");
            } else if (adapterReturnType == "float") {
                emitAdapterLine("call void @zeus_value_init_float(ptr %out, float " + nativeReturn + ")");
            } else if (adapterReturnType == "double") {
                emitAdapterLine("call void @zeus_value_init_double(ptr %out, double " + nativeReturn + ")");
            } else if (nativeIntegerReturn && isIntegerLLVMType(adapterReturnType) &&
                       adapterReturnType != "i64") {
                const auto wide = emitSignedIntegerConversion(nativeReturn, adapterReturnType, "i64",
                    emitAdapterLine, freshAdapterTmp);
                emitAdapterLine("call void @zeus_value_init_i64(ptr %out, i64 " + wide + ")");
            } else if (adapterReturnType == "i64") {
                emitAdapterLine("call void @zeus_value_init_i64(ptr %out, i64 " +
                                nativeReturn + ")");
            } else if (adapterReturnType == "i1") {
                const std::string raw = freshAdapterTmp();
                emitAdapterLine(raw + " = zext i1 " + nativeReturn + " to i32");
                emitAdapterLine("call void @zeus_value_init_bool(ptr %out, i32 " + raw + ")");
            } else if (adapterReturnType == "%ZeusRawBytesRange" &&
                       signature.returnType == "&bytes") {
                const std::string addressPointer = freshAdapterTmp();
                const std::string address = freshAdapterTmp();
                const std::string length = freshAdapterTmp();
                emitAdapterLine(addressPointer +
                    " = extractvalue %ZeusRawBytesRange " + nativeReturn + ", 0");
                emitAdapterLine(address + " = ptrtoint ptr " + addressPointer + " to i64");
                emitAdapterLine(length +
                    " = extractvalue %ZeusRawBytesRange " + nativeReturn + ", 1");
                emitAdapterLine(
                    "call void @zeus_value_from_raw_bytes_range(ptr %out, i64 " +
                    address + ", i64 " + length + ")");
            } else if (adapterReturnType == "ptr" &&
                       (isClassType(signature.returnType) ||
                        (isReferenceTypeName(signature.returnType) &&
                         isClassType(referencedTypeName(signature.returnType))))) {
                const std::string classType = isReferenceTypeName(signature.returnType)
                    ? referencedTypeName(signature.returnType) : signature.returnType;
                const std::string typeName = poolStr(classType);
                emitAdapterLine(
                    "call void @zeus_class_ref_require_managed_live(ptr " +
                    nativeReturn + ")");
                emitAdapterLine("call void @zeus_value_from_class_ptr(ptr %out, ptr " +
                                nativeReturn + ", ptr " + typeName + ")");
                emitAdapterLine("call void @zeus_class_ref_release(ptr " +
                                nativeReturn + ")");
            } else if (adapterReturnType == "ptr" && isReferenceTypeName(signature.returnType)) {
                result.error = "[emit-llvm] closure ABI requires ->& to return a managed class";
                return false;
            } else {
                const auto* abi = ZeusBuiltins::findNativeTypeABI(signature.returnType);
                if (adapterReturnType == "ptr" && ZeusBuiltins::isNativePointerABI(abi)) {
                    emitAdapterLine("call void @" +
                                    std::string(ZeusBuiltins::nativePointerToDynamicHelper(abi)) +
                                    "(ptr %out, ptr " +
                                    nativeReturn + ")");
                } else {
                    result.error = "[emit-llvm] closure ABI cannot box return '" +
                        signature.returnType + "'";
                    return false;
                }
            }
        }
        emitAdapterLine("ret void");
        adapterText += "}\n\n";
        nativeFunctions += adapterText;
        }
        return true;
    }

    static std::string labelFor(size_t ci, size_t pc) {
        return "%L" + std::to_string(ci) + "_" + std::to_string(pc);
    }

    // chunk 尾出口（跳转目标 == size 即程序块尾，语义是“执行流离开该 chunk”）。
    static std::string endLabelFor(size_t ci) {
        return "%L" + std::to_string(ci) + "_end";
    }

    // The top-level fast path keeps proven scalar values in LLVM registers.
    // Dynamic values use ZeusValue and return to the adapter only for VM services.
    bool emitNativeTopLevelChunk(
        const std::vector<IRType::IRInstr>& chunk,
        size_t ci,
        bool isLast
    ) {
        std::map<int, std::string> declaredLocalIntegers;
        for (const auto& instruction : chunk) {
            if (instruction.metadata.kind != IRType::IRMetadataKind::LocalBindingType ||
                (!ZeusBuiltins::findNativeIntegerABI(instruction.metadata.typeName) &&
                 !ZeusBuiltins::isNativeFloatingPointType(instruction.metadata.typeName))) continue;
            int64_t index = -1;
            if (tryArgInt(instruction, 0, index) && index >= 0) {
                declaredLocalIntegers[static_cast<int>(index)] = instruction.metadata.typeName;
            }
        }
        auto checkDeclaredInteger = [&](NativeValue& value, const std::string& declaredType,
                                        const IRType::IRInstr& instruction) {
            if (ZeusBuiltins::isNativeFloatingPointType(declaredType)) {
                std::string converted;
                const std::string nativeType = llvmType(declaredType);
                if (!emitNativeValueAsType(value, nativeType, instruction, converted, declaredType)) return false;
                value = NativeValue{converted, nativeType, false, false, declaredType};
                return true;
            }
            const auto* abi = ZeusBuiltins::findNativeIntegerABI(declaredType);
            if (!abi) return true;
            std::string narrowed;
            if (!emitNativeValueAsType(value, std::string(abi->llvmType), instruction,
                                       narrowed, declaredType)) return false;
            if (abi->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger) {
                const std::string boxed = emitScalarAsValue(
                    std::string(abi->llvmType), narrowed, declaredType);
                value = NativeValue{boxed, "ptr", false, true, declaredType, true};
            } else {
                const auto wide = emitNativeIntegerValueConversion(
                    narrowed, std::string(abi->llvmType), declaredType, "i64", "int",
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); });
                value = NativeValue{wide, "i64", false, false, "int"};
            }
            return true;
        };
        bool hasControlFlow = false;
        bool hasLLVMAddress = false;

        for (const auto& ins : chunk) {
            hasLLVMAddress = hasLLVMAddress ||
                ins.op == VMOperator::LLVM_ADDR || ins.op == VMOperator::LLVM_FADDR ||
                ins.op == VMOperator::LLVM_FSTORE ||
                ins.op == VMOperator::LLVM_SIZEOF;
            switch (ins.op) {
                case VMOperator::LLVM_ADDR:
                case VMOperator::LLVM_FADDR:
                case VMOperator::LLVM_FSTORE:
                case VMOperator::LLVM_SIZEOF:
                case VMOperator::IR_ANNOTATION:
                case VMOperator::PUSH:
                case VMOperator::PUSH_NAMED:
                case VMOperator::POP:
                case VMOperator::CLEAR:
                case VMOperator::SWAP:
                case VMOperator::LOAD_VALUE:
                case VMOperator::LOAD_VALUE_SHARED:
                case VMOperator::OBJ_BUILD:
                case VMOperator::OBJ_LOAD_ATTR:
                case VMOperator::OBJ_LOAD_SAFE_ATTR:
                case VMOperator::OBJ_STORE_ATTR:
                case VMOperator::OBJ_CALL_METHOD:
                case VMOperator::OBJ_CALL_SAFE:
                case VMOperator::LIST_BUILD:
                case VMOperator::LIST_LENGTH:
                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_BUILD:
                case VMOperator::DICT_KEYS_BUILD:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE:
                case VMOperator::LOAD_NATIVE:
                case VMOperator::CALL_NATIVE:
                case VMOperator::CALL_CLOSURE:
                case VMOperator::MAKE_FUNCTION:
                case VMOperator::STORE_VALUE_GLOBAL:
                case VMOperator::ASSIGN_VALUE_GLOBAL:
                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED:
                case VMOperator::DELETE_VAR:
                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL:
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
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR:
                case VMOperator::LOGICAL_NOT:
                case VMOperator::TRY_BEGIN:
                case VMOperator::TRY_END:
                case VMOperator::CATCH_BEGIN:
                case VMOperator::CATCH_END:
                case VMOperator::THROW:
                case VMOperator::JUMP:
                case VMOperator::JUMP_IF_TRUE:
                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::LOOP_BREAK:
                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO:
                    hasControlFlow = hasControlFlow ||
                        ins.op == VMOperator::TRY_BEGIN ||
                        ins.op == VMOperator::JUMP ||
                        ins.op == VMOperator::JUMP_IF_TRUE ||
                        ins.op == VMOperator::JUMP_IF_FALSE ||
                        ins.op == VMOperator::LOOP_BREAK;
                    break;
                default:
                    ZEUS_DEBUG_PRINT(
                        "[emit-llvm] native top-level chunk %zu rejected by opcode: %s",
                        ci, ins.toString().c_str());
                    return false;
            }
        }

        const std::string oldBody = body;
        const auto oldStringPool = strPool;
        const int oldStrCount = strCount;
        const long oldTmpCount = tmpCount;
        const auto oldNativeGlobals = nativeGlobals;
        const auto oldNativeLocals = nativeLocals;
        const auto oldNativeGlobalSlots = nativeGlobalSlots;
        const auto oldNativeLocalSlots = nativeLocalSlots;
        const auto oldNativeGlobalClassTypes = nativeGlobalClassTypes;
        const auto oldNativeGlobalScalarTypes = nativeGlobalScalarTypes;
        const auto oldPrescanGlobalClassTypes = prescanGlobalClassTypes;
        const auto oldEmbeddedSlotTypes = embeddedSlotTypes;
        const auto oldEntryDynamicValueGlobals = entryDynamicValueGlobals;
        const std::string oldEntryPrologue = entryPrologue;
        const std::string oldPendingVariadicCall = pendingVariadicCall;
        const std::string oldPendingDirectCall = pendingDirectCall;
        const std::string oldPendingFunctionLoad = pendingFunctionLoad;
        const bool oldBlkTerminated = blkTerminated;
        const bool oldBlkNeedLabel = blkNeedLabel;
        const bool oldEntryLabelEmitted = entryLabelEmitted;
        const std::string oldError = result.error;
        const int oldErrorLine = result.errorLine;
        const auto oldEntryExceptionPads = entryExceptionPads;
        const auto oldEntryExceptionSlots = entryExceptionSlots;
        const auto oldEntryNativeStringOwners = entryNativeStringOwners;
        const auto oldEntryNativeStringOwnerGenerations =
            entryNativeStringOwnerGenerations;
        const uint64_t oldEntryNativeStringOwnerGeneration =
            entryNativeStringOwnerGeneration;
        const auto oldEntryNativeStringTryBaselines = entryNativeStringTryBaselines;
        const bool oldEntryHasTry = entryHasTry;
        const bool oldNeedsRuntimeErrorFactory = needsRuntimeErrorFactory;
        const int64_t oldActiveEntryExceptionTarget = activeEntryExceptionTarget;

        auto rollback = [&]() {
            const bool preserveLLVMError = hasLLVMAddress &&
                !result.error.empty() && result.error != oldError;
            const std::string llvmError = result.error;
            const int llvmErrorLine = result.errorLine;
            if (!result.error.empty() && result.error != oldError) {
                ZEUS_DEBUG_PRINT(
                    "[emit-llvm] native top-level chunk %zu rolled back: %s",
                    ci, result.error.c_str());
            }
            body = oldBody;
            strPool = oldStringPool;
            strCount = oldStrCount;
            tmpCount = oldTmpCount;
            // The prologue is rolled back with everything else: it holds temp
            // names taken from the same counter, so leaving it behind would let
            // the retried body hand out the same name twice.
            entryPrologue = oldEntryPrologue;
            nativeGlobals = oldNativeGlobals;
            nativeLocals = oldNativeLocals;
            nativeGlobalSlots = oldNativeGlobalSlots;
            nativeLocalSlots = oldNativeLocalSlots;
            nativeGlobalClassTypes = oldNativeGlobalClassTypes;
            nativeGlobalScalarTypes = oldNativeGlobalScalarTypes;
            prescanGlobalClassTypes = oldPrescanGlobalClassTypes;
            embeddedSlotTypes = oldEmbeddedSlotTypes;
            entryDynamicValueGlobals = oldEntryDynamicValueGlobals;
            pendingVariadicCall = oldPendingVariadicCall;
            pendingDirectCall = oldPendingDirectCall;
            pendingFunctionLoad = oldPendingFunctionLoad;
            blkTerminated = oldBlkTerminated;
            blkNeedLabel = oldBlkNeedLabel;
            entryLabelEmitted = oldEntryLabelEmitted;
            result.error = preserveLLVMError ? llvmError : oldError;
            result.errorLine = preserveLLVMError ? llvmErrorLine : oldErrorLine;
            entryExceptionPads = oldEntryExceptionPads;
            entryExceptionSlots = oldEntryExceptionSlots;
            entryNativeStringOwners = oldEntryNativeStringOwners;
            entryNativeStringOwnerGenerations = oldEntryNativeStringOwnerGenerations;
            entryNativeStringOwnerGeneration = oldEntryNativeStringOwnerGeneration;
            entryNativeStringTryBaselines = oldEntryNativeStringTryBaselines;
            entryHasTry = oldEntryHasTry;
            needsRuntimeErrorFactory = oldNeedsRuntimeErrorFactory;
            activeEntryExceptionTarget = oldActiveEntryExceptionTarget;
        };

        auto failNativeTopLevel = [&](const IRType::IRInstr& ins, const std::string& message) {
            result.error = message + " (" + ins.toString() + ")";
            result.errorLine = ins.line;
        };

        std::vector<NativeValue> valueStack;
        bool controlFlowStarted = false;
        bool slotsMaterialized = false;

        // Every native scalar width has to survive a control flow join, so
        // the check covers the whole fixed width set and not just int/bool
        // and double. Leaving i8/i16/i32/float out would make a local of
        // those types fail to compile only when it reaches a merge point.
        auto isNativeScalar = [](const NativeValue& value) {
            return !value.dynamic && !value.functionRef &&
                   (value.type == "i8" || value.type == "i16" ||
                    value.type == "i32" || value.type == "i64" ||
                    value.type == "i1" || value.type == "float" ||
                    value.type == "double");
        };
        auto isNativeSlotValue = [this, &isNativeScalar](const NativeValue& value) {
            return isNativeScalar(value) ||
                   (!value.dynamic && !value.functionRef &&
                    value.type == "%ZeusRawBytesRange" &&
                    value.sourceType == "&bytes") ||
                   (!value.dynamic && !value.functionRef && value.type == "ptr" &&
                    (isClassType(value.sourceType) ||
                     ZeusBuiltins::isNativePointerABI(
                         ZeusBuiltins::findNativeTypeABI(value.sourceType))));
        };

        auto ensureSlot = [&](std::map<int, NativeSlot>& slots,
                              int index,
                              const std::string& type,
                              const std::string& sourceType,
                              const std::string& valueAddressStorageABI,
                              const IRType::IRInstr& ins) {
            auto existing = slots.find(index);
            if (existing != slots.end()) {
                if (existing->second.type != type ||
                    existing->second.sourceType != sourceType ||
                    (!existing->second.valueAddressStorageABI.empty() &&
                     existing->second.valueAddressStorageABI !=
                         valueAddressStorageABI)) {
                    failNativeTopLevel(ins, "[emit-llvm] 控制流变量类型发生变化");
                    return false;
                }
                return true;
            }

            if (controlFlowStarted) {
                failNativeTopLevel(ins,
                                   "[emit-llvm] 控制流开始后才首次出现的原生槽位暂不支持");
                return false;
            }

            NativeSlot slot;
            slot.address = freshTmp();
            slot.type = type;
            slot.sourceType = sourceType;
            slot.addressStorageABI = "native";
            slot.valueAddressStorageABI = valueAddressStorageABI;
            // The storage has to dominate every later access, so it belongs to
            // the entry block even when the first store sits inside a branch.
            entryPrologue += slot.address + " = alloca " + type + "\n";
            if (type == "ptr" && (isClassType(sourceType) || sourceType == "bytes" ||
                                   sourceType == "string")) {
                entryPrologue += "store ptr null, ptr " + slot.address + "\n";
            } else if (type == "%ZeusRawBytesRange") {
                entryPrologue += "store %ZeusRawBytesRange zeroinitializer, ptr " +
                    slot.address + ", align 8\n";
            }
            slots.emplace(index, std::move(slot));
            return true;
        };

        auto emitNativeSlotStore = [&](const NativeValue& value,
                                       NativeSlot& slot) {
            emitLine("call void @zeus_lvalue_unregister_revealed(ptr " + slot.address + ")");
            if (isManagedClassValue(value)) {
                emitLine("call void @zeus_class_ref_retain(ptr " + value.value + ")");
                const std::string previous = freshTmp();
                emitLine(previous + " = load ptr, ptr " + slot.address);
                emitLine("call void @zeus_class_ref_release(ptr " + previous + ")");
                emitLine("store ptr " + value.value + ", ptr " + slot.address);
                if (value.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " + value.value + ")");
                }
            } else if (value.type == "ptr" && value.sourceType == "bytes") {
                emitLine("call void @zeus_bytes_store_slot(ptr " + slot.address +
                         ", ptr " + value.value + ", i32 " +
                         (value.ownsNativeBytes ? "1" : "0") + ")");
            } else if (value.type == "ptr" && value.sourceType == "string") {
                emitLine("call void @zeus_string_store_slot(ptr " + slot.address +
                         ", ptr " + value.value + ")");
                if (value.ownsNativeString) {
                    emitLine("call void @zeus_string_release(ptr " +
                             value.value + ")");
                }
                slot.ownsNativeString = true;
            } else {
                emitLine("store " + value.type + " " + value.value +
                         ", ptr " + slot.address);
            }
            slot.valueAddressStorageABI = value.addressStorageABI;
        };

        auto convertDynamicToNativeSlot = [&](const NativeValue& source,
                                               const NativeSlot& slot,
                                               const IRType::IRInstr& ins,
                                               NativeValue& converted) {
            if (!source.dynamic || slot.addressStorageABI == "zeusvalue") {
                converted = source;
                return true;
            }
            if (slot.type == "double") {
                const auto nativeValue = emitDoubleConversion(source.value, "ptr",
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); });
                converted = NativeValue{nativeValue, "double", false, false, slot.sourceType};
                return true;
            }
            if (ZeusBuiltins::findNativeIntegerABI(slot.sourceType)) {
                const auto nativeValue = emitNativeIntegerValueConversion(
                    source.value, "ptr", {}, slot.type, slot.sourceType,
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); });
                converted = NativeValue{nativeValue, slot.type, false, false, slot.sourceType};
                return true;
            }
            if (slot.type == "i64") {
                const std::string nativeValue = freshTmp();
                emitLine(nativeValue + " = call i64 @zeus_value_require_i64(ptr " +
                         source.value + ")");
                converted = NativeValue{
                    nativeValue, "i64", false, false, slot.sourceType
                };
                return true;
            }
            if (slot.type == "i1") {
                const std::string raw = freshTmp();
                emitLine(raw + " = call i32 @zeus_value_require_bool(ptr " +
                         source.value + ")");
                const std::string nativeValue = freshTmp();
                emitLine(nativeValue + " = icmp ne i32 " + raw + ", 0");
                converted = NativeValue{
                    nativeValue, "i1", false, false, slot.sourceType
                };
                return true;
            }
            if (slot.type == "ptr" && isClassType(slot.sourceType)) {
                const std::string nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @zeus_value_try_class_ptr(ptr " +
                    source.value + ", ptr " + poolStr(slot.sourceType) + ")");
                converted = NativeValue{
                    nativeValue, "ptr", false, false, slot.sourceType
                };
                return true;
            }
            const auto* abi = ZeusBuiltins::findNativeTypeABI(slot.sourceType);
            if (slot.type == "ptr" && ZeusBuiltins::isNativePointerABI(abi)) {
                const std::string nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @" +
                    std::string(ZeusBuiltins::nativePointerFromDynamicHelper(abi)) +
                    "(ptr " + source.value + ")");
                converted = NativeValue{
                    nativeValue, "ptr", false, false, slot.sourceType
                };
                converted.ownsNativeBytes =
                    abi->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes;
                return true;
            }
            if (slot.type == "%ZeusRawBytesRange" &&
                slot.sourceType == "&bytes") {
                const std::string storage = freshTmp();
                const std::string nativeValue = freshTmp();
                emitLine(storage + " = alloca %ZeusRawBytesRange, align 8");
                emitLine("call void @zeus_value_require_raw_bytes_range(ptr " +
                         storage + ", ptr " + source.value + ")");
                emitLine(nativeValue + " = load %ZeusRawBytesRange, ptr " +
                         storage + ", align 8");
                converted = NativeValue{
                    nativeValue, "%ZeusRawBytesRange", false, false, "&bytes"
                };
                return true;
            }
            failNativeTopLevel(ins,
                "[emit-llvm] dynamic value cannot be converted to addressed native slot type '" +
                slot.sourceType + "'");
            return false;
        };

        auto materializeNativeValues = [&](std::map<int, NativeValue>& values,
                                           std::map<int, NativeSlot>& slots) {
            for (const auto& entry : values) {
                const int index = entry.first;
                const NativeValue& value = entry.second;
                if (!isNativeSlotValue(value)) {
                    failNativeTopLevel(chunk.front(),
                                       "[emit-llvm] 控制流槽位只支持 int/bool 或 class 指针");
                    return false;
                }

                if (!ensureSlot(slots, index, value.type,
                                value.sourceType, value.addressStorageABI,
                                chunk.front())) return false;
                emitNativeSlotStore(value, slots[index]);
            }
            values.clear();
            return true;
        };

        auto flushNativeGlobalForReference = [&](int index,
                                                  const IRType::IRInstr& ins) {
            auto native = nativeGlobals.find(index);
            if (native != nativeGlobals.end()) {
                const NativeValue value = native->second;
                if (value.type == "i64" && !value.dynamic) {
                    emitLine("call void @zeus_global_store_i64(i32 " +
                             std::to_string(index) + ", i64 " + value.value + ")");
                } else if (value.type == "i1" && !value.dynamic) {
                    const std::string raw = freshTmp();
                    emitLine(raw + " = zext i1 " + value.value + " to i32");
                    emitLine("call void @zeus_global_store_bool(i32 " +
                             std::to_string(index) + ", i32 " + raw + ")");
                } else if (isManagedClassValue(value)) {
                    const std::string boxed = freshTmp();
                    emitLine(boxed + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_from_class_ptr(ptr " + boxed +
                             ", ptr " + value.value + ", ptr " +
                             poolStr(value.sourceType) + ")");
                    emitLine("call void @zeus_value_global_store(i32 " +
                             std::to_string(index) + ", ptr " + boxed + ")");
                } else if (value.type == "ptr" && value.sourceType == "bytes") {
                    emitLine("call void @zeus_global_store_bytes(i32 " +
                             std::to_string(index) + ", ptr " + value.value + ")");
                } else if (value.type == "ptr" &&
                           ZeusBuiltins::isNativePointerABI(
                               ZeusBuiltins::findNativeTypeABI(value.sourceType))) {
                    const auto* abi = ZeusBuiltins::findNativeTypeABI(value.sourceType);
                    const std::string boxed = freshTmp();
                    emitLine(boxed + " = alloca %ZeusValue, align 8");
                    emitLine("call void @" +
                        std::string(ZeusBuiltins::nativePointerToDynamicHelper(abi)) +
                        "(ptr " + boxed + ", ptr " + value.value + ")");
                    emitLine("call void @zeus_value_global_store(i32 " +
                             std::to_string(index) + ", ptr " + boxed + ")");
                } else if (value.dynamic) {
                    const std::string boxed = freshTmp();
                    emitLine(boxed + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_init_nil(ptr " + boxed + ")");
                    emitLine("call void @zeus_value_clone(ptr " + boxed +
                             ", ptr " + value.value + ")");
                    emitLine("call void @zeus_value_global_store(i32 " +
                             std::to_string(index) + ", ptr " + boxed + ")");
                    if (value.ownsDynamic) {
                        emitRelease(value.value);
                    }
                } else {
                    failNativeTopLevel(ins,
                        "[emit-llvm] cannot flush this global representation before passing it by reference");
                    return false;
                }
                if (value.ownsClass && isManagedClassValue(value)) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             value.value + ")");
                }
                if (value.ownsNativeBytes) {
                    emitLine("call void @zeus_bytes_release(ptr " + value.value + ")");
                }
                nativeGlobals.erase(native);
            }

            auto slot = nativeGlobalSlots.find(index);
            if (slot != nativeGlobalSlots.end()) {
                NativeValue value;
                value.type = slot->second.type;
                value.sourceType = slot->second.sourceType;
                value.value = freshTmp();
                emitLine(value.value + " = load " + value.type + ", ptr " +
                         slot->second.address);
                if (value.type == "i64") {
                    emitLine("call void @zeus_global_store_i64(i32 " +
                             std::to_string(index) + ", i64 " + value.value + ")");
                } else if (value.type == "i1") {
                    const std::string raw = freshTmp();
                    emitLine(raw + " = zext i1 " + value.value + " to i32");
                    emitLine("call void @zeus_global_store_bool(i32 " +
                             std::to_string(index) + ", i32 " + raw + ")");
                } else if (isManagedClassValue(value)) {
                    const std::string boxed = freshTmp();
                    emitLine(boxed + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_from_class_ptr(ptr " + boxed +
                             ", ptr " + value.value + ", ptr " +
                             poolStr(value.sourceType) + ")");
                    emitLine("call void @zeus_value_global_store(i32 " +
                             std::to_string(index) + ", ptr " + boxed + ")");
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             value.value + ")");
                } else if (value.type == "ptr" && value.sourceType == "bytes") {
                    emitLine("call void @zeus_global_store_bytes(i32 " +
                             std::to_string(index) + ", ptr " + value.value + ")");
                    emitLine("call void @zeus_bytes_release(ptr " + value.value + ")");
                } else if (value.type == "ptr" &&
                           ZeusBuiltins::isNativePointerABI(
                               ZeusBuiltins::findNativeTypeABI(value.sourceType))) {
                    const auto* abi = ZeusBuiltins::findNativeTypeABI(value.sourceType);
                    const std::string boxed = freshTmp();
                    emitLine(boxed + " = alloca %ZeusValue, align 8");
                    emitLine("call void @" +
                        std::string(ZeusBuiltins::nativePointerToDynamicHelper(abi)) +
                        "(ptr " + boxed + ", ptr " + value.value + ")");
                    emitLine("call void @zeus_value_global_store(i32 " +
                             std::to_string(index) + ", ptr " + boxed + ")");
                    if (slot->second.sourceType == "bytes") {
                        const std::string ownedBytes = freshTmp();
                        emitLine(ownedBytes + " = load ptr, ptr " +
                                 slot->second.address);
                        emitLine("call void @zeus_bytes_release(ptr " + ownedBytes + ")");
                    }
                } else {
                    failNativeTopLevel(ins,
                        "[emit-llvm] cannot flush this global slot before passing it by reference");
                    return false;
                }
                nativeGlobalSlots.erase(slot);
            }

            // Keep the declared type metadata for future typed loads. The
            // mutable native cache has already been flushed above, and globals
            // with an exposed address use the runtime slot from then on.
            entryDynamicValueGlobals.insert(index);
            return true;
        };

        std::vector<int64_t> nativeEntryExceptionTarget(chunk.size(), -1);
        // Active top-level try regions per pc (see emitNativeFunction: TRY_END
        // counts its own region, TRY_BEGIN counts zero).
        std::vector<int> nativeEntryTryLevel(chunk.size(), 0);
        std::vector<size_t> nativeEntryExceptionStack;
        std::vector<size_t> nativeEntryFinExitHandlerStack;
        bool nativeEntryFinExitHandlerStackActive = false;
        std::vector<size_t> nativeEntryCatchStack;
        std::map<size_t, size_t> nativeEntryCatchBeginForEnd;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            if (ins.op == VMOperator::TRY_BEGIN) {
                int64_t offset = 0;
                if (!tryArgInt(ins, 0, offset) || offset <= 0 ||
                    static_cast<uint64_t>(offset) >= chunk.size() - pc ||
                    chunk[pc + static_cast<size_t>(offset)].op != VMOperator::CATCH_BEGIN) {
                    failNativeTopLevel(ins, "[emit-llvm] invalid top-level TRY_BEGIN catch target");
                    rollback();
                    return false;
                }
                entryHasTry = true;
                needsRuntimeErrorFactory = true;
                nativeEntryExceptionStack.push_back(pc + static_cast<size_t>(offset));
            } else if (ins.op == VMOperator::TRY_END) {
                if (ins.metadata.tag == "fin_exit_handler" &&
                    !nativeEntryFinExitHandlerStackActive) {
                    nativeEntryFinExitHandlerStack = nativeEntryExceptionStack;
                    nativeEntryFinExitHandlerStackActive = true;
                }
                if (nativeEntryExceptionStack.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] top-level TRY_END without TRY_BEGIN");
                    rollback();
                    return false;
                }
                nativeEntryTryLevel[pc] =
                    static_cast<int>(nativeEntryExceptionStack.size());
                nativeEntryExceptionStack.pop_back();
            } else if (!nativeEntryExceptionStack.empty()) {
                nativeEntryExceptionTarget[pc] =
                    static_cast<int64_t>(nativeEntryExceptionStack.back());
                nativeEntryTryLevel[pc] =
                    static_cast<int>(nativeEntryExceptionStack.size());
            }
            if (ins.op == VMOperator::CATCH_BEGIN) nativeEntryCatchStack.push_back(pc);
            if (ins.op == VMOperator::CATCH_END) {
                if (nativeEntryCatchStack.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] top-level CATCH_END without CATCH_BEGIN");
                    rollback();
                    return false;
                }
                nativeEntryCatchBeginForEnd[pc] = nativeEntryCatchStack.back();
                nativeEntryCatchStack.pop_back();
            }
            if (ins.metadata.tag == "fin_exit_terminator" &&
                nativeEntryFinExitHandlerStackActive) {
                nativeEntryExceptionStack = std::move(nativeEntryFinExitHandlerStack);
                nativeEntryFinExitHandlerStack.clear();
                nativeEntryFinExitHandlerStackActive = false;
            }
        }
        if (!nativeEntryExceptionStack.empty() || !nativeEntryCatchStack.empty() ||
            nativeEntryFinExitHandlerStackActive) {
            failNativeTopLevel(chunk.back(), "[emit-llvm] unterminated top-level try/catch");
            rollback();
            return false;
        }

        std::set<size_t> labels;
        bool exitJump = false;
        if (hasControlFlow) {
            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                const auto& ins = chunk[pc];
                if (ins.op == VMOperator::CATCH_BEGIN) labels.insert(pc);
                if (ins.op != VMOperator::JUMP &&
                    ins.op != VMOperator::JUMP_IF_FALSE &&
                    ins.op != VMOperator::JUMP_IF_TRUE &&
                    ins.op != VMOperator::LOOP_BREAK) {
                    continue;
                }

                int64_t off = 0;
                if (!tryArgInt(ins, 0, off)) return false;
                const long long target = static_cast<long long>(pc) + 1 + off;
                if (target < 0 || target > static_cast<long long>(chunk.size())) return false;

                if (target == static_cast<long long>(chunk.size())) {
                    exitJump = true;
                } else {
                    labels.insert(static_cast<size_t>(target));
                }

                if (ins.op == VMOperator::JUMP_IF_FALSE ||
                    ins.op == VMOperator::JUMP_IF_TRUE) {
                    if (pc + 1 < chunk.size()) {
                        labels.insert(pc + 1);
                    } else {
                        exitJump = true;
                    }
                }
            }
        }

        auto popValue = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (valueStack.empty()) {
                failNativeTopLevel(ins, "[emit-llvm] 原生值栈下溢");
                return false;
            }

            value = valueStack.back();
            valueStack.pop_back();
            if (!value.dynamic && isIntegerLLVMType(value.type) &&
                ZeusBuiltins::findNativeUnsignedIntegerABI(value.sourceType)) {
                std::string boxed;
                if (!emitNativeValueAsDynamic(value, ins, boxed)) return false;
                value = NativeValue{boxed, "ptr", false, true,
                    value.sourceType, true};
                return true;
            }
            if (!value.dynamic && value.sourceType != "char" &&
                isSignedIntegerLLVMType(value.type) && value.type != "i64") {
                value.value = emitSignedIntegerConversion(value.value, value.type, "i64",
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); });
                value.type = "i64";
                value.sourceType = "int";
            }
            return true;
        };

        auto parseLiteral = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (!ins.args.empty() &&
                std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                const std::string nilValue = emitDynamicValueForNull(ins);
                if (nilValue.empty()) return false;
                value = NativeValue{ nilValue, "ptr", false, true, "nil", true };
                return true;
            }
            if (!ins.args.empty() &&
                std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                const auto& bytes = std::get<ZeusType::Bytes>(ins.args[0]);
                const std::string dynamicValue = emitDynamicValueForBytes(bytes, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true, "bytes", true };
                return true;
            }
            if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                failNativeTopLevel(ins, "[emit-llvm] 原生 PUSH 参数不是 Value");
                return false;
            }

            const auto& literal = std::get<LexerType::Value>(ins.args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) {
                int64_t integerValue = 0;
                if (std::get<ZeusType::BigInteger>(literal.data).tryToInt64(integerValue)) {
                    value = NativeValue{ std::to_string(integerValue), "i64", false, false };
                    return true;
                }

                const std::string dynamicValue = emitDynamicValueForLiteral(literal, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true, "bigint", true };
                return true;
            }

            if (std::holds_alternative<ZeusType::BigDecimal>(literal.data)) {
                const std::string dynamicValue = emitDynamicValueForLiteral(literal, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true, "float", true };
                return true;
            }

            if (std::holds_alternative<bool>(literal.data)) {
                value = NativeValue{
                    std::get<bool>(literal.data) ? "1" : "0",
                    "i1",
                    false,
                    false
                };
                return true;
            }

            if (std::holds_alternative<std::string>(literal.data)) {
                const std::string& text = std::get<std::string>(literal.data);
                const std::string symbol = poolStr(text);
                const std::string nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @zeus_string_literal_from_data(ptr " +
                         symbol + ", i64 " + std::to_string(text.size()) + ")");
                value = NativeValue{ nativeValue, "ptr", false, false, "string" };
                return true;
            }

            failNativeTopLevel(ins, "[emit-llvm] 原生值栈暂不支持该字面量类型");
            return false;
        };

        auto pushNull = [&]() {
            const std::string value = freshTmp();
            emitLine(value + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_nil(ptr " + value + ")");
            valueStack.push_back(NativeValue{
                value, "ptr", false, true, std::string(), true
            });
        };

        auto toDynamicValue = [&] (
            const NativeValue& value,
            const IRType::IRInstr& ins,
            std::string& dynamicValue,
            bool& ownsDynamic
        ) {
            if (value.dynamic) {
                dynamicValue = value.value;
                ownsDynamic = value.ownsDynamic;
                return true;
            }
            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) return false;
            ownsDynamic = true;
            return true;
        };

        auto emitVariadicValueCall = [&](const IRType::IRInstr& ins, int64_t argc) {
            if (argc < 0 || static_cast<size_t>(argc + 1) > valueStack.size()) return false;

            std::vector<NativeValue> arguments(static_cast<size_t>(argc));
            for (int64_t i = argc - 1; i >= 0; --i) {
                if (!popValue(ins, arguments[static_cast<size_t>(i)])) return false;
            }

            NativeValue callee;
            if (!popValue(ins, callee) || !callee.functionRef ||
                callee.type != "builtin") {
                failNativeTopLevel(ins, "[emit-llvm] invalid native variadic call target");
                return false;
            }

            if (const auto* nativeDeclaration =
                    findNativeBuiltinDeclaration(callee.value)) {
                if (arguments.size() < static_cast<size_t>(
                        nativeDeclaration->minRequiredParams) ||
                    arguments.size() > nativeDeclaration->parameterTypes.size()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] @native builtin call argument count does not match '" +
                        callee.value + "'");
                    return false;
                }

                std::vector<std::string> nativeArguments;
                std::vector<std::string> dynamicCleanup;
                std::vector<std::string> nativeBytesCleanup;
                std::vector<std::string> nativeStringsCleanup;
                for (size_t i = 0; i < nativeDeclaration->parameterTypes.size(); ++i) {
                    const std::string& parameterType =
                        nativeDeclaration->parameterTypes[i];
                    if (i >= arguments.size()) {
                        NativeValue placeholder;
                        if (!emitOptionalArgumentPlaceholder(
                                parameterType, placeholder,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); })) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] @native builtin parameter has no default ABI placeholder");
                            return false;
                        }
                        nativeArguments.push_back(placeholder.value);
                        if (placeholder.dynamic && placeholder.ownsDynamic) {
                            dynamicCleanup.push_back(placeholder.value);
                        }
                        continue;
                    }

                    std::string nativeArgument;
                    if (arguments[i].dynamic) {
                        const std::string nativeParameterType =
                            llvmType(parameterType);
                        if (nativeParameterType != "i64" &&
                            nativeParameterType != "i1") {
                            emitDynamicArgumentTypeCheck(
                                arguments[i].value,
                                *nativeDeclaration,
                                i,
                                [this](const std::string& line) { emitLine(line); });
                        }
                    }
                    if (usesZeusValueABI(parameterType)) {
                        if (!emitNativeValueAsDynamic(
                                arguments[i], ins, nativeArgument)) return false;
                        if (arguments[i].dynamic) {
                            if (arguments[i].ownsDynamic) {
                                dynamicCleanup.push_back(nativeArgument);
                            }
                        } else {
                            dynamicCleanup.push_back(nativeArgument);
                        }
                    } else {
                        bool ownsNativeStringArgument = false;
                        if (!emitNativeValueAsType(
                                arguments[i], llvmType(parameterType), ins,
                                nativeArgument, parameterType, false,
                                nativeDeclaration, i,
                                &ownsNativeStringArgument)) {
                            return false;
                        }
                        if (parameterType == "bytes" && arguments[i].dynamic) {
                            nativeBytesCleanup.push_back(nativeArgument);
                        }
                        if (ownsNativeStringArgument) {
                            nativeStringsCleanup.push_back(nativeArgument);
                        }
                    }
                    nativeArguments.push_back(std::move(nativeArgument));
                }

                const bool returnsDynamic =
                    usesZeusValueABI(nativeDeclaration->returnType);
                const std::string returnType = returnsDynamic
                    ? "void" : llvmType(nativeDeclaration->returnType);
                if (returnType.empty()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] @native builtin return type has no LLVM ABI");
                    return false;
                }

                const std::string callResult = freshTmp();
                std::string callText;
                if (returnsDynamic) {
                    emitLine(callResult + " = alloca %ZeusValue, align 8");
                    callText = "call void " + llvmCallTarget(*nativeDeclaration) +
                        "(ptr " + callResult;
                } else {
                    callText = callResult + " = call " + returnType + " " +
                        llvmCallTarget(*nativeDeclaration) + "(";
                }
                for (size_t i = 0; i < nativeArguments.size(); ++i) {
                    if (i > 0 || returnsDynamic) callText += ", ";
                    callText += functionParameterABIType(
                        nativeDeclaration->parameterTypes[i]) + " " + nativeArguments[i];
                }
                if (nativeDeclaration->hasDefaultParameters()) {
                    if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
                    callText += "i64 " + std::to_string(providedPrefixMask(arguments.size()));
                }
                callText += ")";
                emitLine(callText);
                std::set<std::string> releasedDynamicArguments;
                for (const auto& cleanupValue : dynamicCleanup) {
                    if (releasedDynamicArguments.insert(cleanupValue).second) {
                        emitRelease(cleanupValue);
                    }
                }
                for (const auto& cleanupValue : nativeBytesCleanup) {
                    emitLine("call void @zeus_bytes_release(ptr " + cleanupValue + ")");
                }
                for (const auto& cleanupValue : nativeStringsCleanup) {
                    emitLine("call void @zeus_string_release(ptr " + cleanupValue + ")");
                }
                for (const auto& argument : arguments) {
                    if (argument.dynamic && argument.ownsDynamic &&
                        releasedDynamicArguments.insert(argument.value).second) {
                        emitRelease(argument.value);
                    }
                    if (isManagedClassValue(argument) && argument.ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 argument.value + ")");
                    }
                    if (argument.ownsNativeBytes) {
                        emitLine("call void @zeus_bytes_release(ptr " +
                                 argument.value + ")");
                    }
                    if (argument.ownsNativeString) {
                        emitLine("call void @zeus_string_release(ptr " +
                                 argument.value + ")");
                    }
                }

                if (returnsDynamic) {
                    valueStack.push_back(NativeValue{
                        callResult, "ptr", false, true,
                        nativeDeclaration->returnType, true
                    });
                    return true;
                }

                const auto* returnABI = ZeusBuiltins::findNativeTypeABI(
                    nativeDeclaration->returnType);
                std::string nativeResult = callResult;
                bool ownsNativeBytesResult = false;
                if (returnABI != nullptr && returnABI->valueABI ==
                        ZeusBuiltins::NativeValueABI::NativeBytes) {
                    ownsNativeBytesResult = true;
                } else if (ZeusBuiltins::isNativePointerABI(returnABI)) {
                    nativeResult = freshTmp();
                    emitLine(nativeResult + " = call ptr @" +
                        std::string(ZeusBuiltins::nativePointerInternHelper(returnABI)) +
                        "(ptr " + callResult + ")");
                }
                NativeValue returned{
                    nativeResult, returnType, false, false,
                    nativeDeclaration->returnType, false
                };
                returned.ownsClass = isClassType(nativeDeclaration->returnType);
                returned.ownsNativeBytes = ownsNativeBytesResult;
                std::string dynamicResult;
                if (!emitNativeValueAsDynamic(returned, ins, dynamicResult)) return false;
                if (returned.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             returned.value + ")");
                }
                if (returned.ownsNativeBytes) {
                    emitLine("call void @zeus_bytes_release(ptr " + returned.value + ")");
                }
                valueStack.push_back(NativeValue{
                    dynamicResult, "ptr", false, true,
                    nativeDeclaration->returnType, true
                });
                return true;
            }

            std::vector<std::string> dynamicArguments(arguments.size());
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (!emitNativeValueAsDynamic(arguments[i], ins, dynamicArguments[i])) return false;
            }
            const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
            if (callSymbol.empty()) {
                failNativeTopLevel(ins, "[emit-llvm] native builtin has no dynamic ABI");
                return false;
            }
            const std::string resultValue = emitDynamicVariadicCall(
                callSymbol,
                callee.value,
                dynamicArguments,
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); }
            );
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (arguments[i].dynamic) releaseNativeValue(arguments[i]);
                else {
                    emitRelease(dynamicArguments[i]);
                    if (arguments[i].ownsNativeBytes) {
                        emitLine("call void @zeus_bytes_release(ptr " +
                                 arguments[i].value + ")");
                    }
                    if (isManagedClassValue(arguments[i]) &&
                        arguments[i].ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 arguments[i].value + ")");
                    }
                }
            }
            valueStack.push_back(NativeValue{ resultValue, "ptr", false, true, "nil", true });
            return true;
        };

        auto emitClosureCallTopLevel = [&] (
            const IRType::IRInstr& ins,
            NativeValue& callee,
            const std::vector<NativeValue>& arguments,
            NativeValue& callResult
        ) {
            if (!callee.dynamic || callee.type != "ptr") {
                failNativeTopLevel(ins, "[emit-llvm] closure call requires a dynamic closure value");
                return false;
            }
            std::vector<std::string> dynamicArguments(arguments.size());
            std::vector<bool> ownsDynamicArguments(arguments.size(), false);
            std::vector<std::string> referenceArguments(arguments.size(), "null");
            std::vector<int> referencedGlobalIndices(arguments.size(), -1);
            const size_t sourceCount = std::min(
                arguments.size(), ins.metadata.callArguments.size());
            for (size_t i = 0; i < sourceCount; ++i) {
                const auto& source = ins.metadata.callArguments[i];
                if (!source.isAssignable || source.source.isConst ||
                    source.source.kind != IRType::SymbolRefKind::Global) {
                    continue;
                }
                if (!flushNativeGlobalForReference(source.source.index, ins)) {
                    return false;
                }
                referencedGlobalIndices[i] = source.source.index;
                referenceArguments[i] = freshTmp();
                emitLine(referenceArguments[i] +
                         " = call ptr @zeus_value_global_slot(i32 " +
                         std::to_string(source.source.index) + ")");
            }
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (referencedGlobalIndices[i] >= 0) {
                    dynamicArguments[i] = freshTmp();
                    emitLine(dynamicArguments[i] + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_init_nil(ptr " +
                             dynamicArguments[i] + ")");
                    emitLine("call void @zeus_value_global_load(ptr " +
                             dynamicArguments[i] + ", i32 " +
                             std::to_string(referencedGlobalIndices[i]) + ")");
                    ownsDynamicArguments[i] = true;
                } else {
                    if (!emitNativeValueAsDynamic(
                            arguments[i], ins, dynamicArguments[i])) {
                        return false;
                    }
                    ownsDynamicArguments[i] = arguments[i].dynamic
                        ? arguments[i].ownsDynamic : true;
                }
            }
            std::string argumentArray = "null";
            std::string argumentReferenceArray = "null";
            std::string argumentNameArray = "null";
            if (!dynamicArguments.empty()) {
                argumentArray = freshTmp();
                argumentReferenceArray = freshTmp();
                argumentNameArray = freshTmp();
                emitLine(argumentArray + " = alloca ptr, i64 " +
                         std::to_string(dynamicArguments.size()) + ", align 8");
                emitLine(argumentReferenceArray + " = alloca ptr, i64 " +
                         std::to_string(dynamicArguments.size()) + ", align 8");
                emitLine(argumentNameArray + " = alloca ptr, i64 " +
                         std::to_string(dynamicArguments.size()) + ", align 8");
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    const std::string slot = freshTmp();
                    const std::string referenceSlot = freshTmp();
                    const std::string nameSlot = freshTmp();
                    emitLine(slot + " = getelementptr inbounds ptr, ptr " +
                             argumentArray + ", i64 " + std::to_string(i));
                    emitLine("store ptr " + dynamicArguments[i] + ", ptr " + slot +
                             ", align 8");
                    emitLine(referenceSlot +
                             " = getelementptr inbounds ptr, ptr " +
                             argumentReferenceArray + ", i64 " +
                             std::to_string(i));
                    emitLine("store ptr " + referenceArguments[i] +
                             ", ptr " + referenceSlot + ", align 8");
                    emitLine(nameSlot + " = getelementptr inbounds ptr, ptr " +
                             argumentNameArray + ", i64 " + std::to_string(i));
                    if (arguments[i].namedArgument.empty()) {
                        emitLine("store ptr null, ptr " + nameSlot + ", align 8");
                    } else {
                        const std::string nameSymbol = poolStr(arguments[i].namedArgument);
                        const std::string namePointer = freshTmp();
                        emitLine(namePointer + " = getelementptr inbounds [" +
                            std::to_string(arguments[i].namedArgument.size() + 1) +
                            " x i8], ptr " + nameSymbol + ", i64 0, i64 0");
                        emitLine("store ptr " + namePointer + ", ptr " + nameSlot +
                                 ", align 8");
                    }
                }
            }
            const std::string output = freshTmp();
            emitLine(output + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_closure_call_named(ptr " + output + ", ptr " +
                     callee.value + ", i32 " +
                     std::to_string(dynamicArguments.size()) + ", ptr " +
                     argumentArray + ", ptr " + argumentReferenceArray +
                     ", ptr " + argumentNameArray + ")");
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) emitRelease(dynamicArguments[i]);
            }
            for (const auto& argument : arguments) {
                if (argument.ownsNativeBytes) {
                    emitLine("call void @zeus_bytes_release(ptr " +
                             argument.value + ")");
                }
                if (argument.ownsNativeString) {
                    emitLine("call void @zeus_string_release(ptr " +
                             argument.value + ")");
                }
                if (isManagedClassValue(argument) && argument.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             argument.value + ")");
                }
            }
            if (callee.ownsDynamic) releaseNativeValue(callee);
            callResult = NativeValue{ output, "ptr", false, true, std::string(), true };
            return true;
        };

        auto emitLogicalNot = [&](const IRType::IRInstr& ins) {
            NativeValue operand;
            if (!popValue(ins, operand) || operand.functionRef) {
                failNativeTopLevel(ins, "[emit-llvm] 一元逻辑非操作数无效");
                return false;
            }

            const std::string resultValue = freshTmp();
            if (!operand.dynamic && operand.type == "i1") {
                emitLine(resultValue + " = xor i1 " + operand.value + ", true");
            } else if (!operand.dynamic && operand.type == "i64") {
                emitLine(resultValue + " = icmp eq i64 " + operand.value + ", 0");
            } else {
                std::string dynamicValue;
                if (!emitNativeValueAsDynamic(operand, ins, dynamicValue)) return false;
                const std::string truthy = freshTmp();
                emitLine(truthy + " = call i32 @zeus_value_truthy(ptr " + dynamicValue + ")");
                if (!operand.dynamic || operand.ownsDynamic) emitRelease(dynamicValue);
                if (isManagedClassValue(operand) && operand.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             operand.value + ")");
                }
                emitLine(resultValue + " = icmp eq i32 " + truthy + ", 0");
            }

            valueStack.push_back(NativeValue{ resultValue, "i1", false, false });
            return true;
        };

        auto emitBinary = [&](const IRType::IRInstr& ins) {
            if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                std::string targetName;
                if (!ins.metadata.readDirectCall(targetName)) {
                    failNativeTopLevel(ins, "[emit-llvm] 静态运算标注缺少函数名");
                    return false;
                }

                const auto target = functionSignatures.find(targetName);
                if (target == functionSignatures.end()) {
                    failNativeTopLevel(ins, "[emit-llvm] 找不到静态运算目标'" +
                        targetName + "'");
                    return false;
                }
                const FunctionSignature& signature = target->second;
                if (signature.captureCount != 0 ||
                    signature.parameterTypes.size() != 2) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] 静态二元运算必须是无捕获的双参数函数");
                    return false;
                }

                NativeValue arguments[2];
                if (!popValue(ins, arguments[1]) || !popValue(ins, arguments[0]) ||
                    arguments[0].functionRef || arguments[1].functionRef) {
                    failNativeTopLevel(ins, "[emit-llvm] 静态运算操作数无效");
                    return false;
                }

                std::vector<std::string> nativeArguments(2);
                std::vector<std::string> dynamicArguments;
                std::vector<bool> ownsDynamicArguments;
                std::vector<std::string> nativeByteArguments;
                std::vector<std::string> nativeStringArguments;
                for (size_t i = 0; i < 2; ++i) {
                    const std::string& parameterType = signature.parameterTypes[i];
                    if (arguments[i].dynamic) {
                        const std::string nativeParameterType =
                            llvmType(parameterType);
                        if (nativeParameterType != "i64" &&
                            nativeParameterType != "i1") {
                            emitDynamicArgumentTypeCheck(
                                arguments[i].value,
                                signature,
                                i,
                                [this](const std::string& line) { emitLine(line); });
                        }
                    }
                    if (usesZeusValueABI(parameterType)) {
                        if (!emitNativeValueAsDynamic(
                                arguments[i], ins, nativeArguments[i])) {
                            return false;
                        }
                        dynamicArguments.push_back(nativeArguments[i]);
                        ownsDynamicArguments.push_back(
                            arguments[i].dynamic ? arguments[i].ownsDynamic : true);
                    } else {
                        bool ownsNativeStringArgument = false;
                        if (!emitNativeValueAsType(
                                arguments[i], llvmType(parameterType), ins,
                                nativeArguments[i], parameterType, false,
                                &signature, i, &ownsNativeStringArgument)) {
                            return false;
                        }
                        if (parameterType == "bytes" && arguments[i].dynamic) {
                            nativeByteArguments.push_back(nativeArguments[i]);
                        }
                        if (ownsNativeStringArgument) {
                            nativeStringArguments.push_back(nativeArguments[i]);
                        }
                    }
                }

                const bool returnsDynamic = usesZeusValueABI(signature.returnType);
                const std::string returnType = returnsDynamic
                    ? "void"
                    : llvmType(signature.returnType);
                if (returnType.empty()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] 静态运算返回类型没有 LLVM ABI 映射");
                    return false;
                }

                const std::string callResult = freshTmp();
                std::string callText;
                if (returnsDynamic) {
                    emitLine(callResult + " = alloca %ZeusValue, align 8");
                    callText = "call void " + llvmCallTarget(signature.name) +
                               "(ptr " + callResult;
                } else {
                    callText = callResult + " = call " + returnType + " " +
                               llvmCallTarget(signature.name) + "(";
                }
                for (size_t i = 0; i < 2; ++i) {
                    if (i > 0 || returnsDynamic) callText += ", ";
                    callText += functionParameterABIType(signature.parameterTypes[i]) +
                                " " + nativeArguments[i];
                }
                callText += ")";
                emitLine(callText);

                std::set<std::string> releasedDynamicArguments;
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    if (ownsDynamicArguments[i] &&
                        releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                        emitRelease(dynamicArguments[i]);
                    }
                }
                for (const auto& nativeBytes : nativeByteArguments) {
                    emitLine("call void @zeus_bytes_release(ptr " + nativeBytes + ")");
                }
                for (const auto& nativeString : nativeStringArguments) {
                    emitLine("call void @zeus_string_release(ptr " + nativeString + ")");
                }
                for (size_t i = 0; i < 2; ++i) {
                    if (arguments[i].dynamic && arguments[i].ownsDynamic &&
                        releasedDynamicArguments.insert(arguments[i].value).second) {
                        emitRelease(arguments[i].value);
                    }
                    if (arguments[i].ownsNativeBytes) {
                        emitLine("call void @zeus_bytes_release(ptr " +
                                 arguments[i].value + ")");
                    }
                    if (isManagedClassValue(arguments[i]) &&
                        arguments[i].ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 arguments[i].value + ")");
                    }
                }

                valueStack.push_back(returnsDynamic
                    ? NativeValue{
                        callResult, "ptr", false, true,
                        signature.returnType, true
                      }
                    : NativeValue{
                        callResult, returnType, false, false,
                        signature.returnType, false
                      });
                valueStack.back().ownsClass =
                    !returnsDynamic && isClassType(signature.returnType);
                valueStack.back().ownsNativeBytes = !returnsDynamic &&
                    signature.returnType == "bytes";
                valueStack.back().ownsNativeString = !returnsDynamic &&
                    signature.returnType == "string";
                pendingDirectCall.clear();
                return true;
            }

            NativeValue right;
            NativeValue left;
            if (!popValue(ins, right) || !popValue(ins, left) ||
                left.functionRef || right.functionRef) {
                return false;
            }

            const auto isNativeString = [](const NativeValue& value) {
                const auto* abi = value.dynamic
                    ? nullptr
                    : ZeusBuiltins::findNativeTypeABI(value.sourceType);
                return abi != nullptr &&
                    abi->valueABI == ZeusBuiltins::NativeValueABI::NativeString;
            };
            if (!left.dynamic && !right.dynamic &&
                isNativeString(left) && isNativeString(right)) {
                if (ins.op == VMOperator::ADD) {
                    const std::string concatenated = freshTmp();
                    emitLine(concatenated +
                        " = call ptr @zeus_native_string_concat(ptr " +
                        left.value + ", ptr " + right.value + ")");
                    if (left.ownsNativeString) {
                        emitLine("call void @zeus_string_release(ptr " + left.value + ")");
                    }
                    if (right.ownsNativeString) {
                        emitLine("call void @zeus_string_release(ptr " + right.value + ")");
                    }
                    valueStack.push_back(NativeValue{
                        concatenated, "ptr", false, false, "string"});
                    valueStack.back().ownsNativeString = true;
                    return true;
                }
                if (ins.op == VMOperator::EQUL ||
                    ins.op == VMOperator::NOT_EQUAL) {
                    const std::string equal = freshTmp();
                    emitLine(equal +
                        " = call i1 @zeus_native_string_equal(ptr " +
                        left.value + ", ptr " + right.value + ")");
                    if (left.ownsNativeString) {
                        emitLine("call void @zeus_string_release(ptr " + left.value + ")");
                    }
                    if (right.ownsNativeString) {
                        emitLine("call void @zeus_string_release(ptr " + right.value + ")");
                    }
                    if (ins.op == VMOperator::NOT_EQUAL) {
                        const std::string unequal = freshTmp();
                        emitLine(unequal + " = xor i1 " + equal + ", true");
                        valueStack.push_back(NativeValue{
                            unequal, "i1", false, false, "bool"});
                    } else {
                        valueStack.push_back(NativeValue{
                            equal, "i1", false, false, "bool"});
                    }
                    return true;
                }
            }

            NativeValue floatingResult;
            const bool leftUnsigned =
                ZeusBuiltins::findNativeUnsignedIntegerABI(left.sourceType) != nullptr;
            const bool rightUnsigned =
                ZeusBuiltins::findNativeUnsignedIntegerABI(right.sourceType) != nullptr;
            if (emitNativeFloatingBinary(ins.op, left, right, floatingResult,
                    leftUnsigned, rightUnsigned,
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); })) {
                std::set<std::string> releasedFloatingOperands;
                for (const NativeValue* operand : { &left, &right }) {
                    if (operand->dynamic && operand->ownsDynamic &&
                        releasedFloatingOperands.insert(operand->value).second) {
                        emitRelease(operand->value);
                    }
                }
                valueStack.push_back(std::move(floatingResult));
                return true;
            }

            const bool isLogical =
                ins.op == VMOperator::LOGICAL_AND || ins.op == VMOperator::LOGICAL_OR;
            const bool isComparison =
                ins.op == VMOperator::EQUL || ins.op == VMOperator::NOT_EQUAL ||
                ins.op == VMOperator::GREATER_THAN || ins.op == VMOperator::LESS_THAN ||
                ins.op == VMOperator::GREATER_EQUAL || ins.op == VMOperator::LESS_EQUAL;

            if (!left.dynamic && !right.dynamic) {
                if (isLogical) {
                    if (left.type != "i1" || right.type != "i1") {
                        failNativeTopLevel(ins, "[emit-llvm] 逻辑运算要求 bool 操作数");
                        return false;
                    }
                } else if (isComparison) {
                    if (left.type != right.type ||
                        (left.type != "i64" && left.type != "i1")) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] 原生比较操作数类型不一致 (left=" +
                            left.type + ", source=" + left.sourceType +
                            ", dynamic=" + (left.dynamic ? "true" : "false") +
                            "; right=" + right.type + ", source=" + right.sourceType +
                            ", dynamic=" + (right.dynamic ? "true" : "false") + ")");
                        return false;
                    }
                } else if (left.type != "i64" || right.type != "i64") {
                    failNativeTopLevel(ins, "[emit-llvm] 原生算术运算要求 int 操作数");
                    return false;
                }

                std::string opcode;
                const std::string operandType = isComparison ? left.type :
                    (isLogical ? "i1" : "i64");
                if (ins.op == VMOperator::ADD) opcode = "add";
                if (ins.op == VMOperator::SUBTRACT) opcode = "sub";
                if (ins.op == VMOperator::MULTIPLY) opcode = "mul";
                if (ins.op == VMOperator::DIVIDE) opcode = "sdiv";
                if (ins.op == VMOperator::MODULUS) opcode = "srem";
                if (ins.op == VMOperator::LOGICAL_AND) opcode = "and";
                if (ins.op == VMOperator::LOGICAL_OR) opcode = "or";
                if (ins.op == VMOperator::EQUL) opcode = "icmp eq";
                if (ins.op == VMOperator::NOT_EQUAL) opcode = "icmp ne";
                if (ins.op == VMOperator::GREATER_THAN) opcode = "icmp sgt";
                if (ins.op == VMOperator::LESS_THAN) opcode = "icmp slt";
                if (ins.op == VMOperator::GREATER_EQUAL) opcode = "icmp sge";
                if (ins.op == VMOperator::LESS_EQUAL) opcode = "icmp sle";

                if (opcode.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] 未知的原生二元运算");
                    return false;
                }

                const std::string temporary = freshTmp();
                if (ins.op == VMOperator::DIVIDE || ins.op == VMOperator::MODULUS) {
                    emitLine("call void @zeus_i64_check_division(i64 " + left.value +
                        ", i64 " + right.value + ", i32 " +
                        std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                }
                emitLine(temporary + " = " + opcode + " " + operandType + " " +
                         left.value + ", " + right.value);
                valueStack.push_back(NativeValue{
                    temporary,
                    isLogical || isComparison ? "i1" : "i64",
                    false,
                    false
                });
                return true;
            }

            const int dynamicOpcode = DynamicBinaryABI::opcodeFor(ins.op);
            if (dynamicOpcode < 0) {
                failNativeTopLevel(ins, "[emit-llvm] dynamic 值暂不支持该二元运算");
                return false;
            }

            std::string leftDynamic;
            std::string rightDynamic;
            if (!emitNativeValueAsDynamic(left, ins, leftDynamic) ||
                !emitNativeValueAsDynamic(right, ins, rightDynamic)) {
                return false;
            }

            const std::string temporary = freshTmp();
            emitLine(temporary + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_dynamic_binary(ptr " + temporary +
                     ", ptr null, i32 " + std::to_string(dynamicOpcode) +
                     ", ptr " + leftDynamic + ", ptr " + rightDynamic + ")");

            emitRelease(leftDynamic);
            emitRelease(rightDynamic);
            for (const NativeValue* operand : { &left, &right }) {
                if (isManagedClassValue(*operand) && operand->ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             operand->value + ")");
                }
                if (operand->ownsNativeString) {
                    emitLine("call void @zeus_string_release(ptr " +
                             operand->value + ")");
                }
            }
            valueStack.push_back(NativeValue{
                temporary, "ptr", false, true, std::string(), true
            });
            return true;
        };

        // Dynamic try-depth bookkeeping for top-level code. See
        // emitNativeFunction for the same scheme on regular functions.
        auto emitEntryTryExits = [&](int count) {
            for (int index = 0; index < count; ++index) {
                body += "  call void @zeus_try_exit()\n";
            }
        };
        auto entryTryLevelBefore = [&](size_t pc) -> int {
            return pc < nativeEntryTryLevel.size() ? nativeEntryTryLevel[pc] : 0;
        };
        auto entryTryExitEdgeTarget = [&](const std::string& realLabel, int exits,
                                          std::string& outBlocks) -> std::string {
            if (exits <= 0) return realLabel;
            const std::string wrapper = freshTmp();
            outBlocks += wrapper.substr(1) + ":\n";
            for (int index = 0; index < exits; ++index) {
                outBlocks += "  call void @zeus_try_exit()\n";
            }
            outBlocks += "  br label " + realLabel + "\n";
            return wrapper;
        };

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            activeEntryExceptionTarget = nativeEntryExceptionTarget[pc];
            activeEntryChunk = ci;

            if ((hasControlFlow && labels.count(pc)) ||
                (pc == 0 && (blkTerminated || blkNeedLabel || ci == 0))) {
                std::string label = labelFor(ci, 0);
                label = labelFor(ci, pc);
                if (!entryLabelEmitted && blkTerminated && pc == 0) {
                    label = "%L0_0";
                    entryLabelEmitted = true;
                }
                emitLabelDef(label);
            }

            if (hasControlFlow && !slotsMaterialized) {
                if (!materializeNativeValues(nativeGlobals, nativeGlobalSlots) ||
                    !materializeNativeValues(nativeLocals, nativeLocalSlots)) {
                    rollback();
                    return false;
                }
                slotsMaterialized = true;
            }

            if (hasControlFlow && blkTerminated) continue;

            std::string comment = "; chunk #" + std::to_string(ci) +
                                  " pc=" + std::to_string(pc);
            if (ins.line >= 0) comment += " [L" + std::to_string(ins.line) + "]";
            comment += " " + detail::escapeLLVMCommentText(ins.toString());
            body += "  " + comment + "\n";

            switch (ins.op) {
                case VMOperator::TRY_BEGIN:
                {
                    int64_t catchOffset = 0;
                    if (tryArgInt(ins, 0, catchOffset) && catchOffset > 0) {
                        entryNativeStringTryBaselines[
                            std::make_pair(ci, pc + static_cast<size_t>(catchOffset))] =
                            entryNativeStringOwnerGeneration;
                    }
                    body += "  call void @zeus_try_enter()\n";
                    break;
                }

                case VMOperator::TRY_END:
                    body += "  call void @zeus_try_exit()\n";
                    break;

                case VMOperator::CATCH_BEGIN: {
                    const auto baseline = entryNativeStringTryBaselines.find(
                        std::make_pair(ci, pc));
                    if (baseline != entryNativeStringTryBaselines.end()) {
                        entryNativeStringOwners.erase(std::remove_if(
                            entryNativeStringOwners.begin(), entryNativeStringOwners.end(),
                            [&](const std::string& owner) {
                                const auto generation =
                                    entryNativeStringOwnerGenerations.find(owner);
                                if (generation != entryNativeStringOwnerGenerations.end() &&
                                    generation->second <= baseline->second) {
                                    return false;
                                }
                                entryNativeStringOwnerGenerations.erase(owner);
                                return true;
                            }), entryNativeStringOwners.end());
                    }
                    const auto captured = entryExceptionSlots.find(std::make_pair(ci, pc));
                    if (captured == entryExceptionSlots.end()) {
                        emitLine("unreachable");
                        blkTerminated = true;
                        blkNeedLabel = true;
                        break;
                    }
                    int64_t slot = -1;
                    if (tryArgInt(ins, 1, slot) && slot >= 0) {
                        nativeLocals[static_cast<int>(slot)] = NativeValue{
                            captured->second, "ptr", false, true, {}, true
                        };
                    } else {
                        emitLine("call void @zeus_value_release(ptr " +
                            captured->second + ")");
                    }
                    break;
                }

                case VMOperator::CATCH_END: {
                    const auto begin = nativeEntryCatchBeginForEnd.find(pc);
                    if (begin != nativeEntryCatchBeginForEnd.end()) {
                        const auto captured = entryExceptionSlots.find(
                            std::make_pair(ci, begin->second));
                        int64_t slot = -1;
                        if (captured != entryExceptionSlots.end() &&
                            tryArgInt(chunk[begin->second], 1, slot) && slot >= 0) {
                            emitLine("call void @zeus_value_release(ptr " +
                                captured->second + ")");
                            nativeLocals.erase(static_cast<int>(slot));
                        }
                    }
                    break;
                }

                case VMOperator::THROW: {
                    NativeValue thrown;
                    if (!popValue(ins, thrown) || thrown.functionRef ||
                        !valueStack.empty()) {
                        failNativeTopLevel(ins, "THROW requires one value on the stack");
                        rollback();
                        return false;
                    }
                    std::string boxed;
                    if (!emitNativeValueAsDynamic(thrown, ins, boxed)) {
                        rollback();
                        return false;
                    }
                    if (isManagedClassValue(thrown) && thrown.ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 thrown.value + ")");
                    }
                    emitLine(std::string("call void @zeus_exception_raise") +
                        (thrown.dynamic && !thrown.ownsDynamic ? "(ptr " : "_owned(ptr ") +
                        boxed + ")");
                    emitLine("unreachable");
                    blkTerminated = true;
                    blkNeedLabel = true;
                    break;
                }

                case VMOperator::IR_ANNOTATION:
                    if (annotationKind(ins, "direct_call")) {
                        std::string directCallName;
                        if (!annotationSymbolName(ins, directCallName)) {
                            failNativeTopLevel(ins, "[emit-llvm] 直接调用标注缺少函数名");
                            rollback();
                            return false;
                        }
                        const auto* target = resolveFunctionSignature(directCallName);
                        pendingDirectCall = target != nullptr &&
                            !hasCompleteFunctionAnnotations(*target)
                                ? std::string()
                                : (target != nullptr ? target->name : directCallName);
                    }
                    break;

                case VMOperator::IMPORT:
                case VMOperator::TYPE_IMPL_BUILD:
                    break;

                case VMOperator::LLVM_SIZEOF: {
                    const std::string size =
                        llvmValueStorageSize(ins.metadata.typeName);
                    if (size.empty()) {
                        failNativeTopLevel(ins,
                            "sizeof(T) requires a native LLVM value representation; type '" +
                            ins.metadata.typeName + "' is dynamically represented");
                        rollback();
                        return false;
                    }
                    valueStack.push_back(NativeValue{
                        size, "i64", false, false, "int"});
                    break;
                }

                case VMOperator::LLVM_ADDR: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "invalid @addr variable locator");
                        rollback();
                        return false;
                    }
                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    int64_t index = -1;
                    if (!tryArgInt(ins, 1, index) || index < 0 ||
                        index > std::numeric_limits<int>::max()) {
                        failNativeTopLevel(ins, "invalid @addr variable index");
                        rollback();
                        return false;
                    }

                    std::string storage;
                    std::string storageABI;
                    std::string sourceType;
                    const bool revealView = ins.args.size() > 3 &&
                        std::holds_alternative<std::string>(ins.args[3]) &&
                        std::get<std::string>(ins.args[3]) == "reveal";
                    if (kind == "function") {
                        if (ins.args.size() < 3 ||
                            !std::holds_alternative<std::string>(ins.args[2])) {
                            failNativeTopLevel(ins, "invalid @addr function locator");
                            rollback();
                            return false;
                        }
                        NativeValue address;
                        std::string error;
                        if (!emitLLVMFunctionAddress(
                                std::get<std::string>(ins.args[2]), address,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); }, error)) {
                            failNativeTopLevel(ins, error);
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(address));
                        break;
                    }
                    if (kind == "global") {
                        const auto nativeViewType =
                            prescanGlobalNativeViewTypes.find(
                                static_cast<int>(index));
                        if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                            (nativeViewType->second == "string" ||
                             nativeViewType->second == "bytes")) {
                            if (!flushNativeGlobalForReference(
                                    static_cast<int>(index), ins)) {
                                rollback();
                                return false;
                            }
                            storage = freshTmp();
                            emitLine(storage +
                                " = call ptr @zeus_global_" +
                                (nativeViewType->second == "string"
                                    ? "string" : "bytes") + "_slot(i32 " +
                                std::to_string(index) + ")");
                            storageABI = "native";
                            sourceType = nativeViewType->second;
                        } else {
                            if (!flushNativeGlobalForReference(
                                    static_cast<int>(index), ins)) {
                                rollback();
                                return false;
                            }
                            storage = freshTmp();
                            emitLine(storage +
                                " = call ptr @zeus_value_global_slot(i32 " +
                                std::to_string(index) + ")");
                            storageABI = "zeusvalue";
                        }
                    } else if (kind == "local") {
                        auto slot = nativeLocalSlots.find(static_cast<int>(index));
                        if (slot != nativeLocalSlots.end()) {
                            storage = slot->second.address;
                            storageABI = slot->second.addressStorageABI.empty()
                                ? "native" : slot->second.addressStorageABI;
                            sourceType = slot->second.sourceType;
                        } else {
                            auto value = nativeLocals.find(static_cast<int>(index));
                            if (value == nativeLocals.end()) {
                                failNativeTopLevel(ins,
                                    "@addr local has no stable native value in this block");
                                rollback();
                                return false;
                            }
                            if (value->second.dynamic) {
                                NativeSlot localSlot;
                                localSlot.address = freshTmp();
                                localSlot.type = "ptr";
                                localSlot.sourceType = value->second.sourceType;
                                localSlot.addressStorageABI = "zeusvalue";
                                localSlot.valueAddressStorageABI =
                                    value->second.addressStorageABI;
                                entryPrologue += localSlot.address +
                                    " = alloca %ZeusValue, align 8\n";
                                entryPrologue +=
                                    "call void @zeus_value_init_nil(ptr " +
                                    localSlot.address + ")\n";
                                emitLine("call void @zeus_value_clone(ptr " +
                                    localSlot.address + ", ptr " +
                                    value->second.value + ")");
                                if (value->second.ownsDynamic) {
                                    releaseNativeValue(value->second);
                                }
                                nativeLocals.erase(value);
                                slot = nativeLocalSlots.emplace(
                                    static_cast<int>(index),
                                    std::move(localSlot)).first;
                            } else if (isNativeScalar(value->second) || isManagedClassValue(value->second) ||
                                       (value->second.type == "ptr" &&
                                        ZeusBuiltins::isNativePointerABI(
                                            ZeusBuiltins::findNativeTypeABI(
                                                value->second.sourceType)))) {
                                NativeSlot localSlot;
                                localSlot.address = freshTmp();
                                localSlot.type = value->second.type;
                                localSlot.sourceType = value->second.sourceType;
                                localSlot.addressStorageABI = "native";
                                localSlot.valueAddressStorageABI =
                                    value->second.addressStorageABI;
                                entryPrologue += localSlot.address + " = alloca " +
                                    localSlot.type + ", align " +
                                    (localSlot.type == "i1" ? "1" : "8") + "\n";
                                if (value->second.type == "ptr" &&
                                    value->second.sourceType == "string") {
                                    emitLine("store ptr null, ptr " + localSlot.address);
                                    emitLine("call void @zeus_string_store_slot(ptr " +
                                        localSlot.address + ", ptr " +
                                        value->second.value + ")");
                                    localSlot.ownsNativeString = true;
                                    if (value->second.ownsNativeString) {
                                        emitLine("call void @zeus_string_release(ptr " +
                                            value->second.value + ")");
                                    }
                                } else {
                                    emitLine("store " + localSlot.type + " " +
                                        value->second.value + ", ptr " + localSlot.address +
                                        (localSlot.type == "i1" ? ", align 1" : ", align 8"));
                                }
                                nativeLocals.erase(value);
                                slot = nativeLocalSlots.emplace(
                                    static_cast<int>(index),
                                    std::move(localSlot)).first;
                            } else {
                                failNativeTopLevel(ins,
                                    "@addr cannot expose this local value representation");
                                rollback();
                                return false;
                            }
                            storage = slot->second.address;
                            storageABI = slot->second.addressStorageABI;
                            sourceType = slot->second.sourceType;
                        }
                    } else if (kind == "shared") {
                        if (ins.args.size() < 3 ||
                            !std::holds_alternative<std::string>(ins.args[2])) {
                            failNativeTopLevel(ins,
                                "@addr shared locator is missing its name");
                            rollback();
                            return false;
                        }
                        storage = freshTmp();
                        if (revealView) {
                            emitLine(storage +
                                " = call ptr @zeus_shared_value_native_address(ptr " +
                                poolStr(std::get<std::string>(ins.args[2])) + ", i32 1)");
                        } else {
                            emitLine(storage +
                                " = call ptr @zeus_shared_value_slot_address(ptr " +
                                poolStr(std::get<std::string>(ins.args[2])) + ", ptr " +
                                poolStr(ins.metadata.typeName) + ", i32 " +
                                (ins.metadata.symbol.isConst ? "0" : "1") + ")");
                        }
                        storageABI = "raw";
                    } else {
                        failNativeTopLevel(ins,
                            "unsupported @addr variable scope '" + kind + "'");
                        rollback();
                        return false;
                    }

                    NativeValue address;
                    std::string error;
                    if (!emitLLVMAddressFromStorage(
                            storage, storageABI, sourceType, revealView, !ins.metadata.symbol.isConst, address,
                            [this](const std::string& line) { emitLine(line); },
                            [this]() { return freshTmp(); }, error)) {
                        failNativeTopLevel(ins, error);
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(address));
                    break;
                }

                case VMOperator::LLVM_FADDR: {
                    NativeValue address;
                    NativeValue stringLength;
                    std::string targetType;
                    std::string error;
                    int64_t integerByteWidth = 0;
                    const bool nativeSlot = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "slot";
                    const bool nativeSlotClear = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "slot_clear";
                    if (!annotationString(ins, 0, targetType) ||
                        !llvmFaddrAccessWidth(ins, integerByteWidth, error)) {
                        failNativeTopLevel(ins, error.empty()
                            ? "invalid @faddr type metadata" : error);
                        rollback();
                        return false;
                    }
                    const bool rawBytesRange = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "raw_range";
                    const bool uncheckedRawFaddr =
                        ins.metadata.tag == "unchecked_raw_faddr";
                    const bool hasStringLength = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        (std::get<std::string>(ins.args[1]) == "length" ||
                         rawBytesRange);
                    if (hasStringLength && !popValue(ins, stringLength)) {
                        failNativeTopLevel(ins, "missing @faddr string length");
                        rollback();
                        return false;
                    }
                    if (!popValue(ins, address)) {
                        failNativeTopLevel(ins, "missing @faddr address");
                        rollback();
                        return false;
                    }
                    NativeValue loaded;
                    if (!emitLLVMFetchAddress(
                            address, targetType, loaded,
                            [this](const std::string& line) { emitLine(line); },
                            [this]() { return freshTmp(); }, error,
                            hasStringLength ? &stringLength : nullptr,
                            integerByteWidth, nativeSlot, nativeSlotClear,
                            rawBytesRange, uncheckedRawFaddr)) {
                        failNativeTopLevel(ins, error);
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(loaded));
                    break;
                }

                case VMOperator::LLVM_FSTORE: {
                    NativeValue value;
                    NativeValue address;
                    NativeValue byteLength;
                    std::string targetType;
                    std::string error;
                    int64_t integerByteWidth = 0;
                    const bool rawBytesRange = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "raw_range";
                    const bool uncheckedRawFaddr =
                        ins.metadata.tag == "unchecked_raw_faddr";
                    const bool ownedBytesLength = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "length";
                    const bool hasByteRange = rawBytesRange || ownedBytesLength;
                    const bool nativeSlot = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1]) &&
                        std::get<std::string>(ins.args[1]) == "slot";
                    if (!annotationString(ins, 0, targetType) ||
                        !llvmFaddrAccessWidth(ins, integerByteWidth, error) ||
                        (ins.args.size() > 1 &&
                         std::holds_alternative<std::string>(ins.args[1]) &&
                         std::get<std::string>(ins.args[1]) == "slot_clear") ||
                        (ownedBytesLength && targetType != "bytes") ||
                        (rawBytesRange && targetType != "&bytes")) {
                        failNativeTopLevel(ins, error.empty()
                            ? "invalid @faddr assignment metadata" : error);
                        rollback();
                        return false;
                    }
                    if (!popValue(ins, value) ||
                        (hasByteRange && !popValue(ins, byteLength)) ||
                        !popValue(ins, address)) {
                        failNativeTopLevel(ins, "missing @faddr assignment operands");
                        rollback();
                        return false;
                    }
                    if (hasByteRange) {
                        if (!emitLLVMByteRangeStore(
                                address, byteLength, targetType, value,
                                rawBytesRange,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); }, error)) {
                            failNativeTopLevel(ins, error);
                            rollback();
                            return false;
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                        break;
                    }
                    emitClassAssignmentCopy(ins, value,
                        [&](const std::string& line) { emitLine(line); },
                        [&]() { return freshTmp(); });

                    auto convertNativeValue = [&](
                        const NativeValue& source,
                        const std::string& declaredType,
                        const std::string& expectedType,
                        std::string& converted,
                        bool& sourceConsumed,
                        bool& ownsConvertedNativeString,
                        std::string& error
                    ) {
                        sourceConsumed = false;
                        ownsConvertedNativeString = false;
                        if (source.functionRef) {
                            error = "function references require a ZeusValue target slot";
                            return false;
                        }
                        const auto* declaredABI =
                            ZeusBuiltins::findNativeTypeABI(declaredType);
                        if (expectedType == "ptr" && isClassType(source.sourceType) &&
                            source.sourceType == "String" && declaredType == "string" &&
                            ZeusBuiltins::isNativePointerABI(declaredABI)) {
                            std::string boxed;
                            if (!emitNativeValueAsDynamic(source, ins, boxed)) {
                                error = "failed to convert String class to native string view";
                                return false;
                            }
                            converted = freshTmp();
                            emitLine(converted +
                                " = call ptr @zeus_value_to_native_string(ptr " +
                                boxed + ")");
                            ownsConvertedNativeString = true;
                            emitLine("call void @zeus_value_release(ptr " + boxed + ")");
                            return true;
                        }
                        if (source.dynamic && ZeusBuiltins::findNativeIntegerABI(declaredType)) {
                            converted = emitNativeIntegerValueConversion(
                                source.value, "ptr", {}, expectedType, declaredType,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); });
                            return true;
                        }
                        if (source.dynamic && expectedType == "i64") {
                            converted = freshTmp();
                            emitLine(converted +
                                " = call i64 @zeus_value_require_i64(ptr " +
                                source.value + ")");
                            return true;
                        }
                        if (source.dynamic && expectedType == "i1") {
                            const std::string raw = freshTmp();
                            emitLine(raw +
                                " = call i32 @zeus_value_require_bool(ptr " +
                                source.value + ")");
                            converted = freshTmp();
                            emitLine(converted + " = icmp ne i32 " + raw + ", 0");
                            return true;
                        }
                        if (source.dynamic && expectedType == "ptr" &&
                            isClassType(declaredType)) {
                            converted = freshTmp();
                            emitLine(converted +
                                " = call ptr @zeus_value_try_class_ptr(ptr " +
                                source.value + ", ptr " + poolStr(declaredType) + ")");
                            return true;
                        }
                        if (source.dynamic && expectedType == "ptr" &&
                            ZeusBuiltins::isNativePointerABI(declaredABI)) {
                            converted = freshTmp();
                            emitLine(converted + " = call ptr @" +
                                std::string(ZeusBuiltins::nativePointerFromDynamicHelper(
                                    declaredABI)) + "(ptr " + source.value + ")");
                            ownsConvertedNativeString = declaredABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::NativeString;
                            return true;
                        }
                        if (!emitNativeValueAsType(
                                source, expectedType, ins, converted, declaredType,
                                true, nullptr, 0, &ownsConvertedNativeString)) {
                            error = "value type does not match @faddr target type '" +
                                declaredType + "'";
                            return false;
                        }
                        return true;
                    };
                    auto boxDynamicValue = [&](const NativeValue& source,
                                               std::string& boxed,
                                               bool& ownsBoxed,
                                               std::string& error) {
                        if (!emitNativeValueAsDynamic(source, ins, boxed)) {
                            error = "failed to create ZeusValue for @faddr assignment";
                            return false;
                        }
                        ownsBoxed = source.dynamic ? source.ownsDynamic : true;
                        return true;
                    };
                    if (!emitLLVMStoreAddress(
                            address, targetType, value,
                            [this](const std::string& line) { emitLine(line); },
                            [this]() { return freshTmp(); },
                            convertNativeValue, boxDynamicValue, error,
                            integerByteWidth, nativeSlot, uncheckedRawFaddr)) {
                        failNativeTopLevel(ins, error);
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::PUSH: {
                    NativeValue value;
                    if (!parseLiteral(ins, value)) {
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(value));
                    break;
                }

                case VMOperator::PUSH_NAMED: {
                    if (valueStack.empty() || ins.args.size() != 1 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "invalid named argument");
                        rollback();
                        return false;
                    }
                    valueStack.back().namedArgument = std::get<std::string>(ins.args[0]);
                    break;
                }

                case VMOperator::POP: {
                    NativeValue value;
                    if (!popValue(ins, value)) {
                        rollback();
                        return false;
                    }
                    releaseNativeValue(value);
                    break;
                }

                case VMOperator::CLEAR:
                    for (const auto& value : valueStack) releaseNativeValue(value);
                    valueStack.clear();
                    break;

                case VMOperator::COPY: {
                    if (valueStack.empty()) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生值栈无法 COPY");
                        rollback();
                        return false;
                    }
                    NativeValue copy = valueStack.back();
                    if (isManagedClassValue(copy) && copy.ownsClass) {
                        emitLine("call void @zeus_class_ref_retain(ptr " + copy.value + ")");
                    } else if (copy.type == "ptr" && copy.sourceType == "bytes") {
                        const std::string cloned = freshTmp();
                        emitLine(cloned + " = call ptr @zeus_bytes_clone(ptr " +
                                 copy.value + ")");
                        copy.value = cloned;
                        copy.ownsNativeBytes = true;
                    } else if (copy.dynamic && copy.ownsDynamic) {
                        const std::string cloned = freshTmp();
                        emitLine(cloned + " = alloca %ZeusValue, align 8");
                        emitLine("call void @zeus_value_clone(ptr " + cloned +
                                 ", ptr " + copy.value + ")");
                        copy.value = cloned;
                    }
                    valueStack.push_back(std::move(copy));
                    break;
                }

                case VMOperator::SWAP:
                    if (valueStack.size() < 2) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生值栈无法 SWAP");
                        rollback();
                        return false;
                    }
                    std::swap(valueStack[valueStack.size() - 1], valueStack[valueStack.size() - 2]);
                    break;

                case VMOperator::LOAD_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] LOAD_VALUE_SHARED 缺少名称");
                        rollback();
                        return false;
                    }
                    const std::string sharedName = std::get<std::string>(ins.args[0]);
                    const auto* sharedABI = ins.metadata.kind ==
                            IRType::IRMetadataKind::SharedBindingType
                        ? ZeusBuiltins::findNativeTypeABI(ins.metadata.typeName)
                        : nullptr;
                    if (sharedABI && sharedABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::Int64) {
                        const std::string value = freshTmp();
                        emitLine(value + " = call i64 @zeus_shared_value_load_i64(ptr " +
                                 poolStr(sharedName) + ")");
                        valueStack.push_back(NativeValue{
                            value, "i64", false, false, ins.metadata.typeName
                        });
                        break;
                    }
                    if (sharedABI && sharedABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::Bool1) {
                        const std::string raw = freshTmp();
                        emitLine(raw + " = call i32 @zeus_shared_value_load_bool(ptr " +
                                 poolStr(sharedName) + ")");
                        const std::string value = freshTmp();
                        emitLine(value + " = icmp ne i32 " + raw + ", 0");
                        valueStack.push_back(NativeValue{
                            value, "i1", false, false, ins.metadata.typeName
                        });
                        break;
                    }
                    const std::string value = freshTmp();
                    emitLine(value + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_shared_value_load(ptr " + value +
                             ", ptr " + poolStr(sharedName) + ")");
                    valueStack.push_back(NativeValue{
                        value, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    if (ins.args.size() == 1 &&
                        std::holds_alternative<LexerType::Value>(ins.args[0]) &&
                        ins.metadata.symbol.kind == IRType::SymbolRefKind::Class) {
                        const std::string& className = ins.metadata.symbol.name;
                        if (findClassLayout(className) == nullptr) {
                            failNativeTopLevel(
                                ins, "[emit-llvm] new 使用了未知类型 '" + className + "'");
                            rollback();
                            return false;
                        }
                        valueStack.push_back(NativeValue{
                            className, "classref", false, false, className
                        });
                        break;
                    }

                    if (ins.args.size() == 1 &&
                        std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        NativeValue value;
                        if (!parseLiteral(ins, value)) {
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(value));
                        break;
                    }

                    if (ins.args.size() == 1 &&
                        std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        if (!dynamicVariadicABISymbol(name).empty()) {
                            valueStack.push_back(NativeValue{
                                name, "builtin", true, false
                            });
                        } else if (classLayouts.find(name) != classLayouts.end()) {
                            valueStack.push_back(NativeValue{
                                name, "classref", false, false, name
                            });
                        } else if (const auto* function = resolveFunctionSignature(name)) {
                            pendingFunctionLoad = function->name;
                            valueStack.push_back(NativeValue{
                                function->name, "function", true, false
                            });
                        } else {
                            failNativeTopLevel(ins, "[emit-llvm] 原生 lowering 无法解析名字 '" + name + "'");
                            rollback();
                            return false;
                        }
                        break;
                    }

                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_VALUE 形态不支持");
                        rollback();
                        return false;
                    }

                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    int64_t index = 0;
                    if (!tryArgInt(ins, 1, index)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_VALUE 索引不是整数");
                        rollback();
                        return false;
                    }

                    if (kind == "global" &&
                        detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                        const auto nativeType = prescanGlobalNativeViewTypes.find(
                            static_cast<int>(index));
                        if (nativeType != prescanGlobalNativeViewTypes.end() &&
                            nativeType->second == "bytes") {
                            const std::string slot = freshTmp();
                            emitLine(slot +
                                " = call ptr @zeus_global_bytes_slot(i32 " +
                                std::to_string(index) + ")");
                            const std::string loaded = freshTmp();
                            emitLine(loaded + " = load ptr, ptr " + slot);
                            valueStack.push_back(NativeValue{
                                loaded, "ptr", false, false, "bytes"
                            });
                            break;
                        }
                    }

                    if (kind == "global") {
                        const auto importedNamespace =
                            namespacePackagesByGlobal.find(static_cast<int>(index));
                        if (importedNamespace != namespacePackagesByGlobal.end()) {
                            valueStack.push_back(NativeValue{
                                importedNamespace->second,
                                "namespace",
                                false,
                                false,
                                importedNamespace->second
                            });
                            break;
                        }
                    }

                    if (kind == "global" && isFunctionGlobal(static_cast<int>(index))) {
                        valueStack.push_back(NativeValue{
                            functionGlobals[static_cast<int>(index)],
                            "function",
                            true,
                            false
                        });
                    } else if (kind == "global" &&
                               classGlobals.find(static_cast<int>(index)) != classGlobals.end()) {
                        const std::string& className =
                            classGlobals[static_cast<int>(index)];
                        valueStack.push_back(NativeValue{
                            className, "classref", false, false, className
                        });
                    } else if (kind == "global") {
                        const auto declaredDouble = nativeGlobalDeclaredDoubleTypes.find(static_cast<int>(index));
                        if (declaredDouble != nativeGlobalDeclaredDoubleTypes.end()) {
                            const auto loaded = freshTmp();
                            const std::string nativeType = llvmType(declaredDouble->second);
                            emitLine(loaded + " = call " + nativeType + " @zeus_global_load_" +
                                std::string(nativeType == "float" ? "float" : "double") +
                                "(i32 " + std::to_string(index) + ")");
                            valueStack.push_back(NativeValue{
                                loaded, nativeType, false, false, declaredDouble->second});
                            break;
                        }
                        const auto declaredInteger = nativeGlobalDeclaredIntegerTypes.find(
                            static_cast<int>(index));
                        if (declaredInteger != nativeGlobalDeclaredIntegerTypes.end()) {
                            const auto* unsignedABI = ZeusBuiltins::findNativeUnsignedIntegerABI(
                                declaredInteger->second);
                            const std::string raw = freshTmp();
                            if (unsignedABI) {
                                emitLine(raw + " = call i64 @zeus_global_load_unsigned_integer(i32 " +
                                    std::to_string(index) + ", i32 " +
                                    std::to_string(unsignedABI->integerBits) + ")");
                            } else {
                                emitLine(raw + " = call i64 @zeus_global_load_i64(i32 " +
                                    std::to_string(index) + ")");
                            }
                            const std::string targetType = llvmType(declaredInteger->second);
                            const std::string rawType = unsignedABI
                                ? "@native.uint64" : "@native.longlong";
                            const std::string nativeValue = emitNativeIntegerValueConversion(
                                raw, "i64", rawType, targetType, declaredInteger->second,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); });
                            valueStack.push_back(NativeValue{nativeValue, targetType,
                                false, false, declaredInteger->second});
                            break;
                        }
                        // A global the slot table owns has to be read back from
                        // it, even when this scope still holds an older copy:
                        // a native function called in between wrote the newer
                        // value there.
                        if (prescanAddressExposedGlobals.find(
                                static_cast<int>(index)) !=
                            prescanAddressExposedGlobals.end()) {
                            NativeValue loaded;
                            if (!boxNativeGlobalValue(
                                    ins, static_cast<int>(index), loaded)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(std::move(loaded));
                            break;
                        }
                        const auto slotBacked =
                            nativeGlobalScalarTypes.find(static_cast<int>(index));
                        auto slot = nativeGlobalSlots.find(static_cast<int>(index));
                        if (slot != nativeGlobalSlots.end()) {
                            if (slot->second.addressStorageABI == "zeusvalue") {
                                const std::string loaded = freshTmp();
                                emitLine(loaded + " = alloca %ZeusValue, align 8");
                                emitLine("call void @zeus_value_slot_load(ptr " +
                                    loaded + ", ptr " + slot->second.address + ")");
                                NativeValue loadedValue{
                                    loaded, "ptr", false, true,
                                    slot->second.sourceType, true
                                };
                                loadedValue.addressStorageABI =
                                    slot->second.valueAddressStorageABI;
                                valueStack.push_back(std::move(loadedValue));
                                break;
                            }
                            const std::string temporary = freshTmp();
                            emitLine(temporary + " = load " + slot->second.type +
                                     ", ptr " + slot->second.address);
                            NativeValue loaded{
                                temporary, slot->second.type, false, false,
                                slot->second.sourceType
                            };
                            const auto* slotABI = ZeusBuiltins::findNativeTypeABI(
                                slot->second.sourceType);
                            if (slotABI != nullptr && slotABI->valueABI ==
                                    ZeusBuiltins::NativeValueABI::NativeBytes) {
                                if (detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                                    loaded.ownsNativeBytes = false;
                                } else {
                                    const std::string copy = freshTmp();
                                    emitLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                             temporary + ")");
                                    loaded.value = copy;
                                    loaded.ownsNativeBytes = true;
                                }
                            }
                            loaded.addressStorageABI =
                                slot->second.valueAddressStorageABI;
                            valueStack.push_back(std::move(loaded));
                            break;
                        }

                        if (slotBacked != nativeGlobalScalarTypes.end()) {
                            NativeValue loaded;
                            if (!boxNativeGlobalValue(
                                    ins, static_cast<int>(index), loaded)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(loaded);
                            break;
                        }

                        auto value = nativeGlobals.find(static_cast<int>(index));
                        if (value != nativeGlobals.end()) {
                            NativeValue borrowed = value->second;
                            borrowed.ownsClass = false;
                            if (!borrowed.dynamic && borrowed.type == "ptr" &&
                                borrowed.sourceType == "bytes") {
                                if (detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                                    borrowed.ownsNativeBytes = false;
                                } else {
                                    const std::string copy = freshTmp();
                                    emitLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                             borrowed.value + ")");
                                    borrowed.value = copy;
                                    borrowed.ownsNativeBytes = true;
                                }
                            }
                            if (borrowed.dynamic && borrowed.sourceType.empty()) {
                                const auto nativeViewType =
                                    prescanGlobalNativeViewTypes.find(
                                        static_cast<int>(index));
                                if (nativeViewType != prescanGlobalNativeViewTypes.end()) {
                                    borrowed.sourceType = nativeViewType->second;
                                }
                            }
                            valueStack.push_back(std::move(borrowed));
                        } else {
                            NativeValue loaded;
                            if (!boxNativeGlobalValue(
                                    ins, static_cast<int>(index), loaded)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(loaded);
                        }
                    } else if (kind == "local") {
                        auto slot = nativeLocalSlots.find(static_cast<int>(index));
                        if (slot != nativeLocalSlots.end()) {
                            if (slot->second.addressStorageABI == "zeusvalue") {
                                const std::string loaded = freshTmp();
                                emitLine(loaded + " = alloca %ZeusValue, align 8");
                                emitLine("call void @zeus_value_slot_load(ptr " +
                                    loaded + ", ptr " + slot->second.address + ")");
                                NativeValue loadedValue{
                                    loaded, "ptr", false, true,
                                    slot->second.sourceType, true
                                };
                                loadedValue.addressStorageABI =
                                    slot->second.valueAddressStorageABI;
                                valueStack.push_back(std::move(loadedValue));
                                break;
                            }
                            const std::string temporary = freshTmp();
                            emitLine(temporary + " = load " + slot->second.type +
                                     ", ptr " + slot->second.address);
                            NativeValue loaded{
                                temporary, slot->second.type, false, false,
                                slot->second.sourceType
                            };
                            const auto* slotABI = ZeusBuiltins::findNativeTypeABI(
                                slot->second.sourceType);
                            if (slotABI != nullptr && slotABI->valueABI ==
                                    ZeusBuiltins::NativeValueABI::NativeBytes) {
                                if (detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                                    loaded.ownsNativeBytes = false;
                                } else {
                                    const std::string copy = freshTmp();
                                    emitLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                             temporary + ")");
                                    loaded.value = copy;
                                    loaded.ownsNativeBytes = true;
                                }
                            }
                            loaded.addressStorageABI =
                                slot->second.valueAddressStorageABI;
                            valueStack.push_back(std::move(loaded));
                            break;
                        }

                        auto value = nativeLocals.find(static_cast<int>(index));
                        if (value != nativeLocals.end()) {
                            if (value->second.dynamic && value->second.ownsDynamic) {
                                const std::string cloned = freshTmp();
                                emitLine(cloned + " = alloca %ZeusValue, align 8");
                                emitLine("call void @zeus_value_clone(ptr " + cloned +
                                    ", ptr " + value->second.value + ")");
                                NativeValue copy = value->second;
                                copy.value = cloned;
                                valueStack.push_back(std::move(copy));
                            } else if (!value->second.dynamic &&
                                       value->second.type == "ptr" &&
                                       value->second.sourceType == "bytes") {
                                if (detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                                    NativeValue borrowed = value->second;
                                    borrowed.ownsNativeBytes = false;
                                    borrowed.ownsClass = false;
                                    valueStack.push_back(std::move(borrowed));
                                } else {
                                    const std::string copy = freshTmp();
                                    emitLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                             value->second.value + ")");
                                    NativeValue ownedBytes{
                                        copy, "ptr", false, false, "bytes"
                                    };
                                    ownedBytes.ownsNativeBytes = true;
                                    valueStack.push_back(std::move(ownedBytes));
                                }
                            } else {
                                NativeValue borrowed = value->second;
                                borrowed.ownsClass = false;
                                valueStack.push_back(std::move(borrowed));
                            }
                        } else {
                            const std::string temporary = emitLocalValueLoad(static_cast<int>(index));
                            valueStack.push_back(NativeValue{
                                temporary, "ptr", false, true, std::string(), true
                            });
                        }
                    } else {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_VALUE 暂不支持 " + kind);
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::OBJ_BUILD: {
                    int64_t argc = 0;
                    const std::string annotatedTarget = annotatedConstructionTarget(ins);
                    const std::string nativeTargetType = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1])
                            ? std::get<std::string>(ins.args[1])
                            : std::string();
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc) +
                            (nativeTargetType.empty() ? 1u : 0u)) {
                        failNativeTopLevel(ins, "[emit-llvm] class 构造参数数量无效");
                        rollback();
                        return false;
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popValue(ins, arguments[static_cast<size_t>(i)])) {
                            rollback();
                            return false;
                        }
                    }

                    NativeValue classReference;
                    bool dynamicClassReference = false;
                    if (nativeTargetType.empty() &&
                        (!popValue(ins, classReference) ||
                         (!classReference.dynamic && classReference.type != "classref" &&
                          annotatedTarget.empty()))) {
                        failNativeTopLevel(ins, "[emit-llvm] new 目标不是静态 class 名称");
                        rollback();
                        return false;
                    }
                    dynamicClassReference = classReference.dynamic && annotatedTarget.empty();
                    std::string constructionClassName = dynamicClassReference
                        ? std::string() : classReference.sourceType;
                    if (!annotatedTarget.empty()) constructionClassName = annotatedTarget;
                    std::string genericClassName;
                    std::vector<std::string> genericTypeArguments;
                    std::string specializedClassName;
                    if (!dynamicClassReference && ins.metadata.readClassInstantiation(
                            genericClassName, genericTypeArguments,
                            &specializedClassName) &&
                        !specializedClassName.empty()) {
                        constructionClassName = specializedClassName;
                    }

                    auto emitConstructionLine = [&](const std::string& line) {
                        emitLine(line);
                        if (line.rfind("br ", 0) == 0 || line == "unreachable") {
                            blkTerminated = true;
                            blkNeedLabel = true;
                        }
                    };
                    auto emitConstructionLabel = [&](const std::string& label) {
                        body += label + ":\n";
                        blkTerminated = false;
                        blkNeedLabel = false;
                    };
                    auto freshConstructionTemporary = [&]() {
                        return freshTmp();
                    };
                    auto convertConstructorArgument = [&] (
                        const NativeValue& argument,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        bool& ownsDynamic,
                        bool& ownsNativeString
                    ) {
                        ownsDynamic = false;
                        ownsNativeString = false;
                        if (usesZeusValueABI(declaredType)) {
                            if (!emitNativeValueAsDynamic(argument, ins, nativeArgument)) {
                                return false;
                            }
                            ownsDynamic = argument.dynamic
                                ? argument.ownsDynamic
                                : true;
                            return true;
                        }
                        return emitNativeValueAsType(
                            argument, llvmType(declaredType), ins,
                            nativeArgument, declaredType, false,
                            nullptr, 0, &ownsNativeString);
                    };
                    auto failConstruction = [&] (
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        failNativeTopLevel(at, message);
                        return false;
                    };

                    NativeValue instance;
                    const bool constructionSucceeded = nativeTargetType.empty()
                        ? (dynamicClassReference
                            ? emitDynamicClassConstruction(
                                ins,
                                classReference,
                                arguments,
                                instance,
                                emitConstructionLine,
                                emitConstructionLabel,
                                freshConstructionTemporary,
                                convertConstructorArgument,
                                failConstruction)
                            : emitClassConstruction(
                            ins,
                            constructionClassName,
                            arguments,
                            instance,
                            emitConstructionLine,
                            emitConstructionLabel,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction))
                        : emitNewOperatorConstruction(
                            ins,
                            nativeTargetType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction);
                    if (!constructionSucceeded) {
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(instance));
                    break;
                }

                case VMOperator::OBJ_LOAD_SAFE_ATTR:
                case VMOperator::OBJ_LOAD_ATTR: {
                    if (ins.args.empty() ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] attribute load is missing its name");
                        rollback();
                        return false;
                    }

                    NativeValue object;
                    if (!popValue(ins, object) || object.functionRef) {
                        rollback();
                        return false;
                    }
                    // Safe navigation always resolves dynamically: the
                    // receiver may be nil, and nil has no native form.
                    const bool safeNavTopAttr =
                        (ins.op == VMOperator::OBJ_LOAD_SAFE_ATTR);
                    if (safeNavTopAttr && !object.dynamic) {
                        const NativeValue originalObject = object;
                        std::string dynamicValue;
                        if (!emitNativeValueAsDynamic(
                                originalObject, ins, dynamicValue)) {
                            rollback();
                            return false;
                        }
                        releaseNativeValue(originalObject);
                        object = NativeValue{
                            dynamicValue, "ptr", false, true,
                            std::string(), true
                        };
                    }
                    const std::string& memberName =
                        std::get<std::string>(ins.args[0]);
                    if (safeNavTopAttr || (object.dynamic && object.type == "ptr")) {
                        dynamicFieldLoads.insert(memberName);
                        const std::string loaded = freshTmp();
                        emitLine(loaded + " = alloca %ZeusValue, align 8");
                        const FunctionSignature* method =
                            findAnyInstanceMethod(memberName);
                        const bool useFieldFastPath =
                            !safeNavTopAttr && method == nullptr;
                        if (!useFieldFastPath) {
                            std::string adapter = "null";
                            if (method != nullptr) {
                                adapter = ensureBoundMethodAdapter(memberName);
                            }
                            const std::string attributeCallSymbol = safeNavTopAttr
                                ? "zeus_value_dynamic_safe_attribute_load"
                                : "zeus_value_dynamic_attribute_load";
                            emitLine(
                                "call void @" + attributeCallSymbol + "(ptr " +
                                loaded + ", ptr " + object.value + ", ptr " +
                                poolStr(memberName) + ", i32 " +
                                std::to_string(ins.line > 0 ? ins.line : 0) +
                                ", ptr " + adapter + ")");
                        } else {
                            emitLine("call void @zeus_value_dynamic_field_load(ptr " +
                                     loaded + ", ptr " + object.value + ", ptr " +
                                     poolStr(memberName) + ", i32 " +
                                     std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        }
                        if (object.ownsDynamic) {
                            emitLine("call void @zeus_value_release(ptr " +
                                     object.value + ")");
                        }
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, true, std::string(), true
                        });
                        break;
                    }
                    if (isManagedClassValue(object)) {
                        emitLine("call void @zeus_class_ref_require_live(ptr " +
                                 object.value + ")");
                    }
                    if (object.type == "namespace") {
                        const auto exports =
                            namespaceExportsByPackage.find(object.sourceType);
                        if (exports != namespaceExportsByPackage.end() &&
                            exports->second.find(memberName) != exports->second.end() &&
                            isClassType(memberName)) {
                            valueStack.push_back(NativeValue{
                                memberName, "classref", false, false, memberName
                            });
                            break;
                        }
                        const FunctionSignature* function =
                            findNamespaceFunction(object.sourceType, memberName);
                        if (function == nullptr || function->isExternal ||
                            function->captureCount != 0) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] unsupported static module member " +
                                object.sourceType + "." + memberName);
                            rollback();
                            return false;
                        }
                        valueStack.push_back(NativeValue{
                            function->name, "function", true, false
                        });
                        break;
                    }

                    if (object.type == "classref") {
                        const FunctionSignature* function =
                            findStaticClassFunction(object.sourceType, memberName);
                        if (function == nullptr || function->isExternal ||
                            function->captureCount != 0) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] unsupported static class member " +
                                object.sourceType + "." + memberName);
                            rollback();
                            return false;
                        }
                        valueStack.push_back(NativeValue{
                            function->name, "function", true, false
                        });
                        break;
                    }

                    if (isManagedClassValue(object) &&
                        shouldUseDynamicFieldSidecar(
                            object.sourceType, memberName)) {
                        std::string dynamicReceiver;
                        bool ownsDynamicReceiver = false;
                        if (!toDynamicValue(
                                object, ins, dynamicReceiver, ownsDynamicReceiver)) {
                            rollback();
                            return false;
                        }
                        const std::string loaded = freshTmp();
                        emitLine(loaded + " = alloca %ZeusValue, align 8");
                        emitLine("call void @zeus_value_dynamic_field_load(ptr " +
                                 loaded + ", ptr " + dynamicReceiver + ", ptr " +
                                 poolStr(memberName) + ", i32 " +
                                 std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        if (ownsDynamicReceiver) emitRelease(dynamicReceiver);
                        if (object.ownsClass && isManagedClassValue(object)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     object.value + ")");
                        }
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, true, std::string(), true
                        });
                        break;
                    }

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] attribute load requires a known class receiver");
                        rollback();
                        return false;
                    }
                    const int fieldIndex = findFieldIndex(*layout, memberName);
                    if (fieldIndex < 0 ||
                        static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        const auto method = functionSignatures.find(
                            object.sourceType + "::" + memberName);
                        if (method != functionSignatures.end() &&
                            method->second.ownerType == object.sourceType &&
                            !method->second.isStaticMethod &&
                            !method->second.parameterTypes.empty() &&
                            method->second.parameterTypes.front() == object.sourceType) {
                            NativeValue boundMethod;
                            auto boxReceiver = [&](const NativeValue& value,
                                                   const IRType::IRInstr& at,
                                                   std::string& dynamicValue,
                                                   bool& ownsDynamic) {
                                return toDynamicValue(
                                    value, at, dynamicValue, ownsDynamic);
                            };
                            if (!emitBoundMethodClosure(
                                    ins, object, method->second, boundMethod,
                                    [&](const std::string& line) {
                                        emitLine(line);
                                    },
                                    [&]() { return freshTmp(); },
                                    boxReceiver)) {
                                failNativeTopLevel(
                                    ins, "[emit-llvm] could not create a bound method closure");
                                rollback();
                                return false;
                            }
                            valueStack.push_back(std::move(boundMethod));
                            break;
                        }
                        failNativeTopLevel(ins,
                            "[emit-llvm] unknown class field '" + memberName + "'");
                        rollback();
                        return false;
                    }
                    const std::string& fieldTypeName =
                        layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    if (fieldType.empty()) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] class field has no LLVM storage type");
                        rollback();
                        return false;
                    }
                    const std::string fieldAddress = freshTmp();
                    emitLine(fieldAddress + " = getelementptr inbounds " +
                             llvmStructType(object.sourceType) + ", ptr " +
                             object.value + ", i32 0, i32 " +
                             std::to_string(fieldIndex));
                    const auto inferred = layout->inferredFieldFlags.find(memberName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshTmp();
                        const std::string initialized = freshTmp();
                        const std::string initialized32 = freshTmp();
                        emitLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitLine(initialized + " = load i1, ptr " + flagAddress);
                        emitLine(initialized32 + " = zext i1 " + initialized + " to i32");
                        emitLine("call void @zeus_class_field_require_initialized(i32 " +
                            initialized32 + ", ptr " + poolStr(object.sourceType) +
                            ", ptr " + poolStr(memberName) + ", i32 " +
                            std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                    }
                    if (usesZeusValueABI(fieldTypeName)) {
                        NativeValue loaded{
                            fieldAddress, "ptr", false, true, fieldTypeName, false
                        };
                        if (object.ownsClass && isManagedClassValue(object)) {
                            loaded.value = freshTmp();
                            emitLine(loaded.value + " = alloca %ZeusValue, align 8");
                            emitLine("call void @zeus_value_clone(ptr " + loaded.value +
                                     ", ptr " + fieldAddress + ")");
                            loaded.ownsDynamic = true;
                        }
                        valueStack.push_back(std::move(loaded));
                    } else {
                        const std::string loaded = freshTmp();
                        emitLine(loaded + " = load " + fieldType + ", ptr " +
                                 fieldAddress);
                        NativeValue loadedValue{
                            loaded, fieldType, false, false, fieldTypeName, false
                        };
                        const auto* loadedFieldABI =
                            ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                        if (loadedFieldABI != nullptr && loadedFieldABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::NativeBytes) {
                            if (detail::isNativeBytesDataAddressReceiver(chunk, pc)) {
                                loadedValue.ownsNativeBytes = false;
                            } else {
                                const std::string copy = freshTmp();
                                emitLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                         loaded + ")");
                                loadedValue.value = copy;
                                loadedValue.ownsNativeBytes = true;
                            }
                        } else if (loadedFieldABI != nullptr &&
                                   loadedFieldABI->valueABI ==
                                       ZeusBuiltins::NativeValueABI::NativeString &&
                                   object.ownsClass && isManagedClassValue(object)) {
                            loadedValue.value = freshTmp();
                            emitLine(loadedValue.value +
                                " = call ptr @zeus_string_retain(ptr " + loaded + ")");
                            loadedValue.ownsNativeString = true;
                        }
                        if (isClassType(fieldTypeName) &&
                            object.ownsClass && isManagedClassValue(object)) {
                            emitLine("call void @zeus_class_ref_retain(ptr " + loaded + ")");
                            loadedValue.ownsClass = true;
                        }
                        valueStack.push_back(std::move(loadedValue));
                    }
                    if (object.ownsClass && isManagedClassValue(object)) {
                        releaseNativeValue(object);
                    }
                    break;
                }

                case VMOperator::OBJ_STORE_ATTR: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] attribute store is missing its name");
                        rollback();
                        return false;
                    }
                    NativeValue value;
                    NativeValue object;
                    if (!popValue(ins, value) || !popValue(ins, object) ||
                        object.functionRef || value.functionRef) {
                        rollback();
                        return false;
                    }
                    emitClassAssignmentCopy(ins, value,
                        [&](const std::string& line) { emitLine(line); },
                        [&]() { return freshTmp(); });
                    if (isManagedClassValue(object)) {
                        emitLine("call void @zeus_class_ref_require_live(ptr " +
                                 object.value + ")");
                    }
                    const std::string& memberName = std::get<std::string>(ins.args[0]);
                    if (object.dynamic && object.type == "ptr") {
                        dynamicFieldStores.insert(memberName);
                        std::string source;
                        bool ownsSource = false;
                        if (!toDynamicValue(value, ins, source, ownsSource)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_value_dynamic_field_store(ptr " +
                                 object.value + ", ptr " + poolStr(memberName) +
                                 ", ptr " + source + ", i32 " +
                                 std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        if (ownsSource) emitRelease(source);
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                        if (object.ownsDynamic) emitRelease(object.value);
                        break;
                    }
                    if (isManagedClassValue(object) &&
                        shouldUseDynamicFieldSidecar(
                            object.sourceType, memberName)) {
                        std::string dynamicReceiver;
                        std::string dynamicValue;
                        bool ownsDynamicReceiver = false;
                        bool ownsDynamicValue = false;
                        if (!toDynamicValue(
                                object, ins, dynamicReceiver, ownsDynamicReceiver) ||
                            !toDynamicValue(value, ins, dynamicValue, ownsDynamicValue)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_value_dynamic_field_store(ptr " +
                                 dynamicReceiver + ", ptr " + poolStr(memberName) +
                                 ", ptr " + dynamicValue + ", i32 " +
                                 std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                        if (ownsDynamicValue) emitRelease(dynamicValue);
                        if (ownsDynamicReceiver) emitRelease(dynamicReceiver);
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                        if (object.ownsClass && isManagedClassValue(object)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     object.value + ")");
                        }
                        break;
                    }
                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    const int fieldIndex = layout ? findFieldIndex(*layout, memberName) : -1;
                    if (fieldIndex < 0 || static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        failNativeTopLevel(ins, "[emit-llvm] attribute store requires a known class field '" + memberName + "'");
                        rollback();
                        return false;
                    }
                    if (static_cast<size_t>(fieldIndex) < layout->fieldConstness.size() &&
                        layout->fieldConstness[static_cast<size_t>(fieldIndex)]) {
                        failNativeTopLevel(ins, "[emit-llvm] cannot assign const class field '" + memberName + "'");
                        rollback();
                        return false;
                    }
                    const std::string& fieldTypeName = layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    if (fieldType.empty()) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] class field has no LLVM storage type");
                        rollback();
                        return false;
                    }
                    const std::string fieldAddress = freshTmp();
                    emitLine(fieldAddress + " = getelementptr inbounds " +
                        llvmStructType(object.sourceType) + ", ptr " + object.value +
                        ", i32 0, i32 " + std::to_string(fieldIndex));
                    if (usesZeusValueABI(fieldTypeName)) {
                        std::string source;
                        bool ownsSource = false;
                        if (!toDynamicValue(value, ins, source, ownsSource)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_value_release(ptr " + fieldAddress + ")");
                        emitLine("call void @zeus_value_clone(ptr " + fieldAddress +
                                 ", ptr " + source + ")");
                        if (ownsSource) {
                            emitLine("call void @zeus_value_release(ptr " + source + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                    } else if (isClassType(fieldTypeName)) {
                        std::string storedValue = value.value;
                        if (value.dynamic) {
                            if (!emitNativeValueAsType(
                                    value, "ptr", ins, storedValue, fieldTypeName,
                                    false)) {
                                rollback();
                                return false;
                            }
                        } else if (!isManagedClassValue(value) ||
                                   value.sourceType != fieldTypeName) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] class field store requires a matching native class value");
                            rollback();
                            return false;
                        }
                        // A class field holds its own reference, including when
                        // the assigned expression currently owns the object.
                        emitLine("call void @zeus_class_ref_retain_edge(ptr " + storedValue + ")");
                        const std::string previous = freshTmp();
                        emitLine(previous + " = load ptr, ptr " + fieldAddress);
                        emitLine("call void @zeus_class_ref_release_edge(ptr " + previous + ")");
                        emitLine("store ptr " + storedValue + ", ptr " + fieldAddress);
                        if (value.dynamic && value.ownsDynamic) {
                            emitRelease(value.value);
                        }
                        if (value.ownsClass) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     storedValue + ")");
                        }
                    } else {
                        const auto* fieldABI =
                            ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                        if (fieldABI != nullptr && fieldABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::NativeString) {
                            std::string stringValue;
                            bool stringValueOwned = false;
                            if (value.dynamic) {
                                stringValue = freshTmp();
                                emitLine(stringValue +
                                    " = call ptr @zeus_value_to_native_string(ptr " +
                                    value.value + ")");
                                stringValueOwned = true;
                                if (value.ownsDynamic) emitRelease(value.value);
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "string") {
                                stringValue = value.value;
                                stringValueOwned = value.ownsNativeString;
                            } else if (isClassType(value.sourceType) &&
                                       value.sourceType == "String") {
                                std::string boxed;
                                bool ownsBoxed = false;
                                if (!toDynamicValue(value, ins, boxed, ownsBoxed)) {
                                    rollback();
                                    return false;
                                }
                                stringValue = freshTmp();
                                emitLine(stringValue +
                                    " = call ptr @zeus_value_to_native_string(ptr " +
                                    boxed + ")");
                                stringValueOwned = true;
                                if (ownsBoxed) emitRelease(boxed);
                            } else {
                                failNativeTopLevel(ins,
                                    "string field assignment requires a string value");
                                rollback();
                                return false;
                            }
                            emitLine("call void @zeus_string_store_slot(ptr " +
                                fieldAddress + ", ptr " + stringValue + ")");
                            if (stringValueOwned) {
                                emitLine("call void @zeus_string_release(ptr " +
                                         stringValue + ")");
                            }
                            if (value.ownsNativeString && value.value != stringValue) {
                                emitLine("call void @zeus_string_release(ptr " +
                                         value.value + ")");
                            }
                            if (value.ownsClass && isManagedClassValue(value)) {
                                emitLine("call void @zeus_class_ref_release(ptr " +
                                         value.value + ")");
                            }
                            if (object.ownsClass && isManagedClassValue(object)) {
                                releaseNativeValue(object);
                            }
                            break;
                        }
                        if (fieldABI != nullptr && fieldABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::NativeBytes) {
                            std::string bytesValue;
                            bool bytesValueOwned = false;
                            if (value.dynamic) {
                                bytesValue = freshTmp();
                                emitLine(bytesValue +
                                    " = call ptr @zeus_value_copy_bytes(ptr " +
                                    value.value + ")");
                                bytesValueOwned = true;
                                if (value.ownsDynamic) emitRelease(value.value);
                            } else if (value.type == "ptr" &&
                                       value.sourceType == "bytes") {
                                bytesValue = value.value;
                                bytesValueOwned = value.ownsNativeBytes;
                            } else {
                                failNativeTopLevel(ins,
                                    "bytes field assignment requires a bytes value");
                                rollback();
                                return false;
                            }
                            emitLine("call void @zeus_bytes_store_slot(ptr " +
                                fieldAddress + ", ptr " + bytesValue + ", i32 " +
                                (bytesValueOwned ? "1" : "0") + ")");
                            if (object.ownsClass && isManagedClassValue(object)) {
                                releaseNativeValue(object);
                            }
                            break;
                        }
                        std::string storedValue = value.value;
                        if (ZeusBuiltins::isNativePointerABI(fieldABI)) {
                            if (value.dynamic) {
                                if (!emitNativeValueAsType(
                                        value, "ptr", ins, storedValue, fieldTypeName,
                                        false)) {
                                    rollback();
                                    return false;
                                }
                            } else {
                                if (value.type != "ptr") {
                                    failNativeTopLevel(ins,
                                        "[emit-llvm] native view field assignment has a mismatched type");
                                    rollback();
                                    return false;
                                }
                                storedValue = freshTmp();
                                emitLine(storedValue + " = call ptr @" +
                                    std::string(ZeusBuiltins::nativePointerInternHelper(fieldABI)) +
                                    "(ptr " + value.value + ")");
                            }
                        } else if (value.dynamic) {
                            if (!emitNativeValueAsType(
                                    value, fieldType, ins, storedValue, fieldTypeName,
                                    false)) {
                                rollback();
                                return false;
                            }
                        } else if (fieldType == "double" && !value.dynamic &&
                                   value.sourceType != "char" && isSignedIntegerLLVMType(value.type)) {
                            storedValue = emitDoubleConversion(value.value, value.type,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); });
                        } else if (value.type != fieldType ||
                                   (isClassType(fieldTypeName) &&
                                    value.sourceType != fieldTypeName)) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] class field assignment has a mismatched type");
                            rollback();
                            return false;
                        }
                        emitLine("store " + fieldType + " " + storedValue +
                                 ", ptr " + fieldAddress);
                        if (value.dynamic && value.ownsDynamic) {
                            emitRelease(value.value);
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                    }
                    const auto inferred = layout->inferredFieldFlags.find(memberName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshTmp();
                        emitLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitLine("store i1 1, ptr " + flagAddress);
                    }
                    if (object.ownsClass && isManagedClassValue(object)) {
                        releaseNativeValue(object);
                    }
                    break;
                }

                case VMOperator::OBJ_CALL_SAFE:
                case VMOperator::OBJ_CALL_METHOD: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] class method call is missing its method name");
                        rollback();
                        return false;
                    }

                    // Safe navigation always dispatches dynamically: the
                    // receiver may be nil, and nil has no native form.
                    const bool safeNavCall =
                        (ins.op == VMOperator::OBJ_CALL_SAFE);
                    const char* dynamicCallSymbol = safeNavCall
                        ? "zeus_value_dynamic_safe_member_call"
                        : "zeus_value_dynamic_member_call";

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 1, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc) + 1) {
                        failNativeTopLevel(ins, "[emit-llvm] class method call has invalid arguments");
                        rollback();
                        return false;
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc) + 1);
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popValue(ins, arguments[static_cast<size_t>(i) + 1])) {
                            rollback();
                            return false;
                        }
                    }
                    if (!popValue(ins, arguments[0])) {
                        rollback();
                        return false;
                    }

                    auto convertArgument = [&] (
                        const NativeValue& value,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        std::string& cleanupValue
                    ) {
                        if (usesZeusValueABI(declaredType)) {
                            if (!emitNativeValueAsDynamic(value, ins, nativeArgument)) {
                                return false;
                            }
                            cleanupValue = value.dynamic
                                ? (value.ownsDynamic ? nativeArgument : std::string())
                                : nativeArgument;
                            return true;
                        }

                        bool ownsNativeString = false;
                        if (!emitNativeValueAsType(
                                value, llvmType(declaredType), ins,
                                nativeArgument, declaredType, false,
                                nullptr, 0, &ownsNativeString)) {
                            return false;
                        }
                        cleanupValue = ownsNativeString
                            ? detail::nativeStringCleanupToken(nativeArgument)
                            : (declaredType == "bytes" && value.dynamic
                                ? detail::nativeBytesCleanupToken(nativeArgument)
                                : (value.dynamic && value.ownsDynamic &&
                                !isClassType(declaredType)
                                ? value.value
                                : std::string()));
                        return true;
                    };
                    auto releaseDynamic = [&](const std::string& value) {
                        emitRelease(value);
                    };
                    auto failMethod = [&](const IRType::IRInstr& at,
                                          const std::string& message) {
                        failNativeTopLevel(at, message);
                        return false;
                    };

                    NativeValue callResult;
                    const std::string methodName = std::get<std::string>(ins.args[0]);
                    const bool hasTypedImplMethod = hasTypedInstanceMethodSignature(
                        arguments[0].sourceType, methodName);
                    const bool staticClassReceiver = arguments[0].type == "classref" &&
                        isClassType(arguments[0].sourceType);
                    std::string resolvedMethod;
                    const bool resolvedTypedMethod = ins.metadata.readDirectCall(resolvedMethod) &&
                        functionSignatures.find(resolvedMethod) != functionSignatures.end();
                    if (safeNavCall || (!resolvedTypedMethod && (arguments[0].dynamic ||
                        (arguments[0].type != "ptr" && !staticClassReceiver) ||
                        (!isClassType(arguments[0].sourceType) && !hasTypedImplMethod)))) {
                        auto toDynamic = [&](const NativeValue& value,
                                             std::string& dynamicValue,
                                             bool& ownsDynamic) {
                            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                                return false;
                            }
                            ownsDynamic = value.dynamic ? value.ownsDynamic : true;
                            return true;
                        };
                        auto releaseOwnedDynamic = [&](const std::string& value) {
                            emitRelease(value);
                        };
                        if (!emitDynamicMemberCall(
                                ins,
                                methodName,
                                arguments,
                                callResult,
                                [&](const std::string& line) { emitLine(line); },
                                [&]() { return freshTmp(); },
                                toDynamic,
                                releaseOwnedDynamic,
                                failMethod,
                                {},
                                dynamicCallSymbol)) {
                            rollback();
                            return false;
                        }
                    } else {
                        auto emitMethodReference = [&] (
                            size_t parameterIndex,
                            const FunctionSignature& called,
                            std::string& nativeArgument
                        ) {
                            const size_t receiverOffset = called.isStaticMethod ? 0 : 1;
                            if (parameterIndex < receiverOffset) {
                                return failMethod(ins,
                                    "[emit-llvm] method receiver cannot bind to a reference parameter");
                            }
                            const size_t userArgumentIndex = parameterIndex - receiverOffset;
                            const std::string parameterName = parameterIndex <
                                    called.parameterNames.size()
                                ? called.parameterNames[parameterIndex]
                                : std::string();
                            const IRType::CallArgumentSource* source = nullptr;
                            for (const auto& candidate : ins.metadata.callArguments) {
                                const bool matches = candidate.namedParameter.empty()
                                    ? candidate.positionalIndex ==
                                        static_cast<int>(userArgumentIndex)
                                    : candidate.namedParameter == parameterName;
                                if (matches) {
                                    source = &candidate;
                                    break;
                                }
                            }
                            if (!source || !source->isAssignable ||
                                source->source.kind != IRType::SymbolRefKind::Global ||
                                source->source.isConst) {
                                return failMethod(ins,
                                    "[emit-llvm] a top-level method reference argument must be a mutable global variable");
                            }
                            if (!flushNativeGlobalForReference(source->source.index, ins)) {
                                return false;
                            }
                            nativeArgument = freshTmp();
                            emitLine(nativeArgument +
                                " = call ptr @zeus_value_global_slot(i32 " +
                                std::to_string(source->source.index) + ")");
                            emitReferenceArgumentTypeCheck(
                                nativeArgument, called, parameterIndex,
                                [this](const std::string& line) { emitLine(line); });
                            if (called.isExternal && called.nativeReferenceABI != "slot") {
                                const std::string slot = nativeArgument;
                                emitExternalReferenceArgument(
                                    called.parameterTypes[parameterIndex], slot,
                                    nativeArgument,
                                    [this](const std::string& line) { emitLine(line); },
                                    [this]() { return freshTmp(); });
                            }
                            return true;
                        };
                        if (!emitKnownClassMethodCall(
                                   ins,
                                   methodName,
                                   arguments,
                                   callResult,
                                   [&](const std::string& line) { emitLine(line); },
                                   [&]() { return freshTmp(); },
                                   convertArgument,
                                   releaseDynamic,
                                   failMethod,
                                   {},
                                   emitMethodReference)) {
                            rollback();
                            return false;
                        }
                    }
                    valueStack.push_back(std::move(callResult));
                    break;
                }

                case VMOperator::MAKE_FUNCTION: {
                    int64_t protoId = 0;
                    if (!tryArgInt(ins, 0, protoId) ||
                        functionSignaturesByChunk.find(static_cast<size_t>(protoId)) ==
                            functionSignaturesByChunk.end()) {
                        failNativeTopLevel(ins, "[emit-llvm] MAKE_FUNCTION 缺少原生函数签名");
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] shared store 缺少名称");
                        rollback();
                        return false;
                    }
                    NativeValue value;
                    if (!popValue(ins, value) || value.functionRef) {
                        rollback();
                        return false;
                    }
                    emitClassAssignmentCopy(ins, value,
                        [&](const std::string& line) { emitLine(line); },
                        [&]() { return freshTmp(); });
                    const std::string sharedName = std::get<std::string>(ins.args[0]);
                    const auto* sharedABI = ins.metadata.kind ==
                            IRType::IRMetadataKind::SharedBindingType
                        ? ZeusBuiltins::findNativeTypeABI(ins.metadata.typeName)
                        : nullptr;
                    if (sharedABI && ZeusBuiltins::findNativeIntegerABI(ins.metadata.typeName) &&
                        !checkDeclaredInteger(value, ins.metadata.typeName, ins)) {
                        rollback();
                        return false;
                    }
                    if (sharedABI && (sharedABI->valueABI ==
                            ZeusBuiltins::NativeValueABI::Int64 ||
                        sharedABI->valueABI == ZeusBuiltins::NativeValueABI::Bool1)) {
                        const std::string expectedType = sharedABI->valueABI ==
                                ZeusBuiltins::NativeValueABI::Int64
                            ? "i64" : "i1";
                        std::string nativeValue;
                        if (value.dynamic) {
                            nativeValue = freshTmp();
                            const std::string expectedABI = expectedType == "i64" ?
                                "i64 @zeus_value_require_i64(ptr " :
                                "i32 @zeus_value_require_bool(ptr ";
                            const std::string checked = freshTmp();
                            emitLine(checked + " = call " + expectedABI + value.value + ")");
                            if (expectedType == "i1") {
                                emitLine(nativeValue + " = icmp ne i32 " + checked + ", 0");
                            } else {
                                nativeValue = checked;
                            }
                            if (value.ownsDynamic) releaseNativeValue(value);
                        } else if (value.type == expectedType) {
                            nativeValue = value.value;
                        } else {
                            failNativeTopLevel(ins,
                                "[emit-llvm] shared scalar store type does not match declaration");
                            rollback();
                            return false;
                        }

                        const std::string operation =
                            ins.op == VMOperator::STORE_VALUE_SHARED ? "store" : "assign";
                        if (expectedType == "i64") {
                            emitLine(std::string("call void @zeus_shared_value_") +
                                operation + "_i64(ptr " + poolStr(sharedName) +
                                ", i64 " + nativeValue + ")");
                        } else {
                            const std::string boolValue = freshTmp();
                            emitLine(boolValue + " = zext i1 " + nativeValue + " to i32");
                            emitLine(std::string("call void @zeus_shared_value_") +
                                operation + "_bool(ptr " + poolStr(sharedName) +
                                ", i32 " + boolValue + ")");
                        }
                    } else {
                        std::string dynamicValue;
                        if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                            rollback();
                            return false;
                        }
                        const char* operation = ins.op == VMOperator::STORE_VALUE_SHARED
                            ? "store" : "assign";
                        emitLine(std::string("call void @zeus_shared_value_") + operation +
                            "(ptr " + poolStr(sharedName) +
                            ", ptr " + dynamicValue + ")");
                        if (value.dynamic) {
                            if (value.ownsDynamic) releaseNativeValue(value);
                        } else {
                            emitLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                            if (value.ownsClass && isManagedClassValue(value)) {
                                releaseNativeValue(value);
                            }
                        }
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_GLOBAL:
                case VMOperator::ASSIGN_VALUE_GLOBAL: {
                    int64_t index = 0;
                    if (!tryArgInt(ins, 0, index)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 STORE_GLOBAL 索引不是整数");
                        rollback();
                        return false;
                    }

                    if (isFunctionGlobal(static_cast<int>(index))) {
                        break;
                    }

                    NativeValue value;
                    if (!popValue(ins, value)) {
                        rollback();
                        return false;
                    }
                    const auto declaredDouble = nativeGlobalDeclaredDoubleTypes.find(static_cast<int>(index));
                    if (declaredDouble != nativeGlobalDeclaredDoubleTypes.end()) {
                        std::string nativeValue;
                        const std::string nativeType = llvmType(declaredDouble->second);
                        if (!emitNativeValueAsType(value, nativeType, ins, nativeValue, declaredDouble->second)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_global_store_" +
                            std::string(nativeType == "float" ? "float" : "double") +
                            "(i32 " + std::to_string(index) + ", " + nativeType +
                            " " + nativeValue + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        break;
                    }
                    const auto integerDeclaration = nativeGlobalDeclaredIntegerTypes.find(static_cast<int>(index));
                    if (integerDeclaration != nativeGlobalDeclaredIntegerTypes.end() &&
                        !checkDeclaredInteger(value, integerDeclaration->second, ins)) {
                        rollback();
                        return false;
                    }
                    emitClassAssignmentCopy(ins, value,
                        [&](const std::string& line) { emitLine(line); },
                        [&]() { return freshTmp(); });
                    if (value.functionRef) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!toDynamicValue(value, ins, dynamicValue, ownsDynamic)) {
                            rollback();
                            return false;
                        }
                        value = NativeValue{
                            dynamicValue, "ptr", false, true, std::string(), ownsDynamic
                        };
                    }

                    const auto nativeViewType =
                        prescanGlobalNativeViewTypes.find(static_cast<int>(index));
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "string") {
                        std::string nativeString;
                        bool ownsNativeString = false;
                        if (!emitNativeValueAsType(
                                value, "ptr", ins, nativeString, "string", false,
                                nullptr, 0, &ownsNativeString)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_global_store_string(i32 " +
                            std::to_string(index) + ", ptr " + nativeString + ")");
                        if (ownsNativeString) {
                            emitLine("call void @zeus_string_release(ptr " + nativeString + ")");
                        }
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        if (value.dynamic && value.ownsDynamic) {
                            releaseNativeValue(value);
                        }
                        if (!value.dynamic) releaseNativeValue(value);
                        if (value.ownsClass && isManagedClassValue(value)) {
                            releaseNativeValue(value);
                        }
                        nativeGlobals.erase(static_cast<int>(index));
                        nativeGlobalSlots.erase(static_cast<int>(index));
                        break;
                    }
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "bytes") {
                        std::string nativeBytes;
                        if (!emitNativeValueAsType(
                                value, "ptr", ins, nativeBytes, "bytes", false)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_global_store_bytes(i32 " +
                            std::to_string(index) + ", ptr " + nativeBytes + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        if (value.dynamic) {
                            emitLine("call void @zeus_bytes_release(ptr " +
                                     nativeBytes + ")");
                        }
                        if ((value.dynamic && value.ownsDynamic) ||
                            value.ownsNativeBytes ||
                            (value.ownsClass && isManagedClassValue(value))) {
                            releaseNativeValue(value);
                        }
                        const auto previousNative =
                            nativeGlobals.find(static_cast<int>(index));
                        if (previousNative != nativeGlobals.end() &&
                            previousNative->second.ownsNativeBytes) {
                            releaseNativeValue(previousNative->second);
                        }
                        const auto previousSlot =
                            nativeGlobalSlots.find(static_cast<int>(index));
                        if (previousSlot != nativeGlobalSlots.end() &&
                            previousSlot->second.addressStorageABI != "zeusvalue" &&
                            previousSlot->second.type == "ptr" &&
                            previousSlot->second.sourceType == "bytes") {
                            const std::string oldBytes = freshTmp();
                            emitLine(oldBytes + " = load ptr, ptr " +
                                     previousSlot->second.address);
                            emitLine("call void @zeus_bytes_release(ptr " + oldBytes + ")");
                            emitLine("store ptr null, ptr " +
                                     previousSlot->second.address);
                        }
                        nativeGlobals.erase(static_cast<int>(index));
                        nativeGlobalSlots.erase(static_cast<int>(index));
                        break;
                    }

                    if (prescanAddressExposedGlobals.find(static_cast<int>(index)) !=
                        prescanAddressExposedGlobals.end()) {
                        const auto scalarType =
                            nativeGlobalScalarTypes.find(static_cast<int>(index));
                        const bool storeNativeScalar =
                            scalarType != nativeGlobalScalarTypes.end() &&
                            !value.dynamic && value.type == scalarType->second;
                        if (storeNativeScalar) {
                            if (value.type == "i64") {
                                emitLine("call void @zeus_global_store_i64(i32 " +
                                    std::to_string(index) + ", i64 " + value.value + ")");
                            } else {
                                const std::string raw = freshTmp();
                                emitLine(raw + " = zext i1 " + value.value + " to i32");
                                emitLine("call void @zeus_global_store_bool(i32 " +
                                    std::to_string(index) + ", i32 " + raw + ")");
                            }
                        } else {
                            std::string dynamicValue;
                            if (value.dynamic && !value.ownsDynamic) {
                                dynamicValue = freshTmp();
                                emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
                                emitLine("call void @zeus_value_init_nil(ptr " +
                                         dynamicValue + ")");
                                emitLine("call void @zeus_value_clone(ptr " +
                                         dynamicValue + ", ptr " + value.value + ")");
                            } else if (!emitNativeValueAsDynamic(
                                           value, ins, dynamicValue)) {
                                rollback();
                                return false;
                            }
                            emitLine("call void @zeus_value_global_store(i32 " +
                                std::to_string(index) + ", ptr " + dynamicValue + ")");
                            if (value.ownsClass && isManagedClassValue(value)) {
                                releaseNativeValue(value);
                            }
                            if (value.ownsNativeBytes) {
                                releaseNativeValue(value);
                            }
                        }

                        const auto previousNative =
                            nativeGlobals.find(static_cast<int>(index));
                        if (previousNative != nativeGlobals.end() &&
                            ((previousNative->second.ownsClass &&
                              isManagedClassValue(previousNative->second)) ||
                             previousNative->second.ownsNativeBytes)) {
                            releaseNativeValue(previousNative->second);
                        }
                        const auto previousSlot =
                            nativeGlobalSlots.find(static_cast<int>(index));
                        if (previousSlot != nativeGlobalSlots.end()) {
                            if (previousSlot->second.addressStorageABI == "zeusvalue" &&
                                previousSlot->second.type == "ptr") {
                                emitLine("call void @zeus_value_release(ptr " +
                                         previousSlot->second.address + ")");
                            } else if (previousSlot->second.type == "ptr" &&
                                       (isClassType(previousSlot->second.sourceType) ||
                                        previousSlot->second.sourceType == "bytes")) {
                                const std::string oldObject = freshTmp();
                                emitLine(oldObject + " = load ptr, ptr " +
                                         previousSlot->second.address);
                                emitLine(std::string("call void @") +
                                    (previousSlot->second.sourceType == "bytes"
                                        ? "zeus_bytes_release(ptr "
                                        : "zeus_class_ref_release(ptr ") +
                                    oldObject + ")");
                            }
                        }
                        nativeGlobals.erase(static_cast<int>(index));
                        nativeGlobalSlots.erase(static_cast<int>(index));
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        if (!storeNativeScalar) {
                            nativeGlobalScalarTypes.erase(static_cast<int>(index));
                        }
                        if (isManagedClassValue(value)) {
                            nativeGlobalClassTypes[static_cast<int>(index)] =
                                value.sourceType;
                            prescanGlobalClassTypes[static_cast<int>(index)] =
                                value.sourceType;
                        } else {
                            nativeGlobalClassTypes.erase(static_cast<int>(index));
                            prescanGlobalClassTypes.erase(static_cast<int>(index));
                        }
                        break;
                    }

                    const auto scalarType =
                        nativeGlobalScalarTypes.find(static_cast<int>(index));
                    const bool runtimeGlobalScalar =
                        scalarType != nativeGlobalScalarTypes.end() &&
                        !value.dynamic && value.type == scalarType->second;
                    const bool unclassifiedScalar = !runtimeGlobalScalar &&
                        !value.dynamic &&
                        (value.type == "i64" || value.type == "i1" || value.type == "double");

                    auto slot = nativeGlobalSlots.find(static_cast<int>(index));
                    auto previousNative = nativeGlobals.find(static_cast<int>(index));
                    const bool releasePreviousClass = previousNative != nativeGlobals.end() &&
                        ((previousNative->second.ownsClass &&
                          isManagedClassValue(previousNative->second)) ||
                         previousNative->second.ownsNativeBytes);
                    if (slot != nativeGlobalSlots.end()) {
                        if (slot->second.addressStorageABI == "zeusvalue") {
                            std::string dynamicValue;
                            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                                rollback();
                                return false;
                            }
                            const bool releaseDynamicValue =
                                !value.dynamic || value.ownsDynamic;
                            emitLine("call void @zeus_value_slot_store(ptr " +
                                slot->second.address + ", ptr " + dynamicValue + ")");
                            if (releaseDynamicValue) {
                                emitLine("call void @zeus_value_release(ptr " +
                                         dynamicValue + ")");
                            }
                            if (value.ownsClass && isManagedClassValue(value)) {
                                releaseNativeValue(value);
                            }
                            break;
                        }
                        NativeValue nativeValue;
                        if (!convertDynamicToNativeSlot(value, slot->second,
                                                        ins, nativeValue)) {
                            rollback();
                            return false;
                        }
                        if (!isNativeSlotValue(nativeValue) ||
                            slot->second.type != nativeValue.type ||
                            slot->second.sourceType != nativeValue.sourceType) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 控制流全局槽位类型不匹配");
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(nativeValue, slot->second);
                        if (value.dynamic && value.ownsDynamic) {
                            releaseNativeValue(value);
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            releaseNativeValue(value);
                        }
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeGlobals.erase(previousNative);
                        }
                    } else if (hasControlFlow && isNativeSlotValue(value) &&
                               !runtimeGlobalScalar) {
                        if (!ensureSlot(nativeGlobalSlots, static_cast<int>(index),
                                        value.type, value.sourceType,
                                        value.addressStorageABI,
                                        ins)) {
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(value,
                            nativeGlobalSlots[static_cast<int>(index)]);
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeGlobals.erase(previousNative);
                        }
                    } else if (value.dynamic || unclassifiedScalar) {
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        std::string dynamicValue = value.value;
                        if (unclassifiedScalar &&
                            !emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_value_global_store(i32 " +
                                 std::to_string(index) + ", ptr " + dynamicValue + ")");
                        nativeGlobalClassTypes.erase(static_cast<int>(index));
                        prescanGlobalClassTypes.erase(static_cast<int>(index));
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        nativeGlobals.erase(static_cast<int>(index));
                    } else {
                        if (isManagedClassValue(value)) {
                            if (!value.ownsClass) {
                                emitLine("call void @zeus_class_ref_retain(ptr " +
                                         value.value + ")");
                                value.ownsClass = true;
                            }
                            std::string dynamicValue;
                            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                                rollback();
                                return false;
                            }
                            emitLine("call void @zeus_value_global_store(i32 " +
                                     std::to_string(index) + ", ptr " + dynamicValue + ")");
                            entryDynamicValueGlobals.insert(static_cast<int>(index));
                            if (releasePreviousClass) {
                                releaseNativeValue(previousNative->second);
                            }
                            releaseNativeValue(value);
                            nativeGlobals.erase(static_cast<int>(index));
                            break;
                        }
                        // A scalar written through a direct global access still
                        // updates the runtime slot, so a later read from a native
                        // function sees the new value.
                        if (runtimeGlobalScalar) {
                            if (value.type == "i64") {
                                emitLine("call void @zeus_global_store_i64(i32 " +
                                         std::to_string(index) + ", i64 " + value.value + ")");
                            } else {
                                const std::string raw = freshTmp();
                                emitLine(raw + " = zext i1 " + value.value + " to i32");
                                emitLine("call void @zeus_global_store_bool(i32 " +
                                         std::to_string(index) + ", i32 " + raw + ")");
                            }
                            if (releasePreviousClass) {
                                releaseNativeValue(previousNative->second);
                            }
                            // The slot table is the only owner now: keeping an
                            // SSA copy here would shadow writes made by native
                            // functions called later.
                            nativeGlobals.erase(static_cast<int>(index));
                        } else {
                            if (value.type == "ptr" && value.sourceType == "bytes" &&
                                !value.ownsNativeBytes) {
                                const std::string copy = freshTmp();
                                emitLine(copy + " = call ptr @zeus_bytes_clone(ptr " +
                                         value.value + ")");
                                value.value = copy;
                                value.ownsNativeBytes = true;
                            }
                            if (releasePreviousClass) {
                                releaseNativeValue(previousNative->second);
                            }
                            nativeGlobals[static_cast<int>(index)] = value;
                        }
                    }
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 ||
                        !std::holds_alternative<std::string>(ins.args[0]) ||
                        !std::holds_alternative<std::string>(ins.args[1])) {
                        failNativeTopLevel(ins, "[emit-llvm] DELETE_VAR 参数无效");
                        rollback();
                        return false;
                    }
                    const std::string& kind = std::get<std::string>(ins.args[1]);
                    if (kind == "shared") {
                        emitLine("call i32 @zeus_shared_value_delete(ptr " +
                            poolStr(std::get<std::string>(ins.args[0])) + ")");
                        break;
                    }
                    if (kind != "global") {
                        failNativeTopLevel(ins, "[emit-llvm] 顶层 delete 仅支持 global/shared");
                        rollback();
                        return false;
                    }

                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0 ||
                        index > std::numeric_limits<int>::max()) {
                        failNativeTopLevel(ins, "[emit-llvm] delete 全局索引无效");
                        rollback();
                        return false;
                    }

                    const int globalIndex = static_cast<int>(index);
                    const auto slot = nativeGlobalSlots.find(globalIndex);
                    const auto value = nativeGlobals.find(globalIndex);
                    const bool slotIsClass = slot != nativeGlobalSlots.end() &&
                        slot->second.type == "ptr" && isClassType(slot->second.sourceType);
                    const bool valueIsClass = value != nativeGlobals.end() &&
                        value->second.type == "ptr" && isClassType(value->second.sourceType);
                    const bool slotIsBytes = slot != nativeGlobalSlots.end() &&
                        slot->second.type == "ptr" && slot->second.sourceType == "bytes";
                    if (!slotIsClass && !valueIsClass) {
                        if (slotIsBytes) {
                            const std::string bytes = freshTmp();
                            emitLine(bytes + " = load ptr, ptr " + slot->second.address);
                            emitLine("call void @zeus_bytes_release(ptr " + bytes + ")");
                        }
                        emitLine("call void @zeus_value_global_delete(i32 " +
                                 std::to_string(globalIndex) + ")");
                        if (value != nativeGlobals.end()) {
                            releaseNativeValue(value->second);
                            nativeGlobals.erase(value);
                        }
                        nativeGlobalSlots.erase(globalIndex);
                        break;
                    }

                    NativeValue object;
                    if (slotIsClass) {
                        object = NativeValue{
                            freshTmp(), "ptr", false, false, slot->second.sourceType
                        };
                        emitLine(object.value + " = load ptr, ptr " + slot->second.address);
                    } else {
                        object = value->second;
                    }

                    auto emitDeleteLine = [&](const std::string& line) {
                        emitLine(line);
                        if (line.rfind("br ", 0) == 0 || line == "unreachable") {
                            blkTerminated = true;
                            blkNeedLabel = true;
                        }
                    };
                    auto emitDeleteLabel = [&](const std::string& label) {
                        emitLabelDef(label);
                    };
                    auto failDelete = [&](
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        failNativeTopLevel(at, message);
                        return false;
                    };
                    if (!emitClassDelete(
                            ins, object, true, emitDeleteLine, emitDeleteLabel,
                            [&]() { return freshTmp(); }, failDelete)) {
                        rollback();
                        return false;
                    }

                    if (slot != nativeGlobalSlots.end()) {
                        emitLine("store ptr null, ptr " + slot->second.address);
                    } else {
                        nativeGlobals.erase(globalIndex);
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || !popValue(ins, value)) {
                        rollback();
                        return false;
                    }

                    const auto integerDeclaration = declaredLocalIntegers.find(static_cast<int>(index));
                    if (integerDeclaration != declaredLocalIntegers.end() &&
                        !checkDeclaredInteger(value, integerDeclaration->second, ins)) {
                        rollback();
                        return false;
                    }
                    emitClassAssignmentCopy(ins, value,
                        [&](const std::string& line) { emitLine(line); },
                        [&]() { return freshTmp(); });
                    if (value.functionRef) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!toDynamicValue(value, ins, dynamicValue, ownsDynamic)) {
                            rollback();
                            return false;
                        }
                        value = NativeValue{
                            dynamicValue, "ptr", false, true,
                            std::string(), ownsDynamic
                        };
                    }

                    auto slot = nativeLocalSlots.find(static_cast<int>(index));
                    auto previousNative = nativeLocals.find(static_cast<int>(index));
                    const bool releasePreviousClass = previousNative != nativeLocals.end() &&
                        previousNative->second.ownsClass &&
                        isManagedClassValue(previousNative->second);
                    if (slot != nativeLocalSlots.end()) {
                        if (slot->second.addressStorageABI == "zeusvalue") {
                            std::string dynamicValue;
                            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                                rollback();
                                return false;
                            }
                            const bool releaseDynamicValue =
                                !value.dynamic || value.ownsDynamic;
                            emitLine("call void @zeus_value_slot_store(ptr " +
                                slot->second.address + ", ptr " + dynamicValue + ")");
                            if (releaseDynamicValue) {
                                emitLine("call void @zeus_value_release(ptr " +
                                         dynamicValue + ")");
                            }
                            if (value.ownsClass && isManagedClassValue(value)) {
                                releaseNativeValue(value);
                            }
                            nativeLocals.erase(static_cast<int>(index));
                            break;
                        }
                        NativeValue nativeValue;
                        if (!convertDynamicToNativeSlot(value, slot->second,
                                                        ins, nativeValue)) {
                            rollback();
                            return false;
                        }
                        if (!isNativeSlotValue(nativeValue) ||
                            slot->second.type != nativeValue.type ||
                            slot->second.sourceType != nativeValue.sourceType) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 控制流局部槽位类型不匹配");
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(nativeValue, slot->second);
                        if (value.dynamic && value.ownsDynamic) {
                            releaseNativeValue(value);
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            releaseNativeValue(value);
                        }
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeLocals.erase(previousNative);
                        }
                    } else if (hasControlFlow && isNativeSlotValue(value)) {
                        if (!ensureSlot(nativeLocalSlots, static_cast<int>(index),
                                        value.type, value.sourceType,
                                        value.addressStorageABI,
                                        ins)) {
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(value,
                            nativeLocalSlots[static_cast<int>(index)]);
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeLocals.erase(previousNative);
                        }
                    } else if (value.dynamic) {
                        emitLine("call void @zeus_value_local_store(i32 " +
                                 std::to_string(index) + ", ptr " + value.value + ")");
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        nativeLocals.erase(static_cast<int>(index));
                    } else if (isManagedClassValue(value)) {
                        if (!value.ownsClass) {
                            emitLine("call void @zeus_class_ref_retain(ptr " +
                                     value.value + ")");
                        }
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        value.ownsClass = true;
                        nativeLocals[static_cast<int>(index)] = value;
                    } else {
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        nativeLocals[static_cast<int>(index)] = value;
                    }
                    break;
                }

                case VMOperator::LIST_BUILD:
                case VMOperator::DICT_BUILD: {
                    int64_t count = 0;
                    if (!tryArgInt(ins, 0, count) || count < 0 ||
                        count > std::numeric_limits<int32_t>::max()) {
                        failNativeTopLevel(ins, "[emit-llvm] 容器构造元素数量无效");
                        rollback();
                        return false;
                    }
                    const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                    const int64_t valueCount = dictionary ? count * 2 : count;
                    std::vector<NativeValue> values(
                        static_cast<std::size_t>(valueCount));
                    for (int64_t i = valueCount - 1; i >= 0; --i) {
                        if (!popValue(ins, values[static_cast<std::size_t>(i)])) {
                            rollback();
                            return false;
                        }
                    }

                    std::vector<std::string> dynamicValues;
                    std::vector<bool> ownsDynamicValues;
                    dynamicValues.reserve(values.size());
                    ownsDynamicValues.reserve(values.size());
                    for (const auto& value : values) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (value.functionRef || !toDynamicValue(
                                value, ins, dynamicValue, ownsDynamic)) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 容器元素不能转换为 ZeusValue");
                            rollback();
                            return false;
                        }
                        dynamicValues.push_back(dynamicValue);
                        ownsDynamicValues.push_back(ownsDynamic);
                    }

                    const std::string resultValue = ContainerABI::emitBuildCall(
                        dictionary, count, dynamicValues,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    for (std::size_t i = 0; i < dynamicValues.size(); ++i) {
                        if (ownsDynamicValues[i]) emitRelease(dynamicValues[i]);
                    }
                    for (const auto& value : values) {
                        if (isManagedClassValue(value) && value.ownsClass) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LENGTH:
                case VMOperator::DICT_KEYS_BUILD: {
                    NativeValue container;
                    if (!popValue(ins, container) || container.functionRef) {
                        failNativeTopLevel(ins, "[emit-llvm] 容器操作需要动态值");
                        rollback();
                        return false;
                    }
                    std::string dynamicContainer;
                    bool ownsDynamic = false;
                    if (!toDynamicValue(container, ins, dynamicContainer, ownsDynamic)) {
                        rollback();
                        return false;
                    }
                    const std::string symbol = ins.op == VMOperator::LIST_LENGTH
                        ? "@zeus_value_array_length"
                        : "@zeus_value_dict_keys";
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, { dynamicContainer },
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    if (ownsDynamic) emitRelease(dynamicContainer);
                    if (isManagedClassValue(container) && container.ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 container.value + ")");
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE: {
                    const bool isStore = ins.op == VMOperator::LIST_STORE_ELEM ||
                        ins.op == VMOperator::DICT_STORE_VALUE;
                    const bool isList = ins.op == VMOperator::LIST_LOAD_ELEM ||
                        ins.op == VMOperator::LIST_STORE_ELEM;
                    NativeValue storedValue;
                    NativeValue keyOrIndex;
                    NativeValue container;
                    if ((isStore && !popValue(ins, storedValue)) ||
                        !popValue(ins, keyOrIndex) || !popValue(ins, container) ||
                        (isStore && storedValue.functionRef) ||
                        keyOrIndex.functionRef || container.functionRef) {
                        failNativeTopLevel(ins, "[emit-llvm] 容器索引操作的栈值无效");
                        rollback();
                        return false;
                    }

                    const bool hasRawBytesRangeIndexMetadata =
                        ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexLoad ||
                        ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexStore;
                    if (hasRawBytesRangeIndexMetadata) {
                        const bool metadataMatchesOperation = !isList &&
                            (isStore
                                ? ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexStore
                                : ins.metadata.kind == IRType::IRMetadataKind::RawBytesRangeIndexLoad);
                        if (!metadataMatchesOperation ||
                            (!container.sourceType.empty() && container.sourceType != "&bytes")) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] raw bytes range index metadata does not match its container");
                            rollback();
                            return false;
                        }
                        container.sourceType = "&bytes";
                    }

                    if (container.sourceType == "&bytes") {
                        std::string address;
                        std::string length;
                        std::string rangeError;
                        if (!emitLLVMRawBytesRangeParts(
                                container,
                                [&](const std::string& line) { emitLine(line); },
                                [&]() { return freshTmp(); },
                                address, length, rangeError)) {
                            failNativeTopLevel(ins, rangeError);
                            rollback();
                            return false;
                        }
                        std::string index;
                        if (!emitNativeValueAsType(
                                keyOrIndex, "i64", ins, index, "int")) {
                            rollback();
                            return false;
                        }
                        if (isStore) {
                            std::string element;
                            if (!emitNativeValueAsType(
                                    storedValue, "i64", ins, element, "int")) {
                                rollback();
                                return false;
                            }
                            emitLine("call void @zeus_raw_bytes_range_set(i64 " +
                                address + ", i64 " + length + ", i64 " + index +
                                ", i64 " + element + ")");
                        } else {
                            const std::string element = freshTmp();
                            emitLine(element +
                                " = call i64 @zeus_raw_bytes_range_get(i64 " +
                                address + ", i64 " + length + ", i64 " + index + ")");
                            valueStack.push_back(NativeValue{
                                element, "i64", false, false, "int"
                            });
                        }
                        if (container.dynamic && container.ownsDynamic) {
                            emitRelease(container.value);
                        }
                        if (container.ownsNativeBytes) {
                            releaseNativeValue(container);
                        }
                        if (isStore) {
                            valueStack.push_back(std::move(storedValue));
                        }
                        break;
                    }

                    const size_t parameterCount = isStore ? 3 : 2;
                    const std::string operatorSymbol = isStore ? "[]=" : "[]";
                    const bool isArrayType = container.sourceType == "Array" ||
                        container.sourceType == "list" ||
                        container.sourceType == "array";
                    const bool isArrayValue = isList || isArrayType;
                    const bool isBuiltinContainer = isArrayType ||
                        container.sourceType == "dict";
                    const auto* indexABI =
                        ZeusBuiltins::findNativeTypeABI(keyOrIndex.sourceType);
                    const bool nativeI64ArrayIndex = isArrayValue &&
                        !keyOrIndex.dynamic && keyOrIndex.type == "i64" &&
                        indexABI != nullptr &&
                        indexABI->valueABI == ZeusBuiltins::NativeValueABI::Int64;
                    const FunctionSignature* operatorSignature = nullptr;
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        std::string targetName;
                        if (!ins.metadata.readDirectCall(targetName)) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 下标运算标注缺少函数名");
                            rollback();
                            return false;
                        }
                        const auto target = functionSignatures.find(targetName);
                        if (target == functionSignatures.end() ||
                            target->second.operatorSymbol != operatorSymbol ||
                            target->second.parameterTypes.size() != parameterCount) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 下标运算标注与目标签名不匹配");
                            rollback();
                            return false;
                        }
                        operatorSignature = &target->second;
                        container.sourceType = operatorSignature->ownerType;
                    } else if (!container.sourceType.empty() &&
                               (isClassType(container.sourceType) ||
                                ZeusBuiltins::findNativeTypeABI(
                                    container.sourceType) != nullptr)) {
                        operatorSignature = findClassOperatorSignature(
                            container.sourceType, operatorSymbol, parameterCount);
                        if (operatorSignature == nullptr && !isBuiltinContainer) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] type 没有匹配的下标operator");
                            rollback();
                            return false;
                        }
                    }

                    if (!isList && operatorSignature != nullptr) {
                        const std::string qualifiedPrefix =
                            operatorSignature->ownerType + "::";
                        const std::string methodName = operatorSignature->name.substr(
                            qualifiedPrefix.size());
                        std::vector<NativeValue> operatorArguments{
                            container, keyOrIndex
                        };
                        if (isStore) operatorArguments.push_back(storedValue);

                        auto convertArgument = [&] (
                            const NativeValue& value,
                            const std::string& declaredType,
                            std::string& nativeArgument,
                            std::string& cleanupValue
                        ) {
                            if (usesZeusValueABI(declaredType)) {
                                bool ownsDynamic = false;
                                if (!toDynamicValue(
                                        value, ins, nativeArgument, ownsDynamic)) {
                                    return false;
                                }
                                cleanupValue = ownsDynamic ? nativeArgument : std::string();
                                return true;
                            }
                            bool ownsNativeString = false;
                            if (!emitNativeValueAsType(
                                    value, llvmType(declaredType), ins,
                                    nativeArgument, declaredType, false,
                                    nullptr, 0, &ownsNativeString)) {
                                return false;
                            }
                            cleanupValue = ownsNativeString
                                ? detail::nativeStringCleanupToken(nativeArgument)
                                : (declaredType == "bytes" && value.dynamic
                                    ? detail::nativeBytesCleanupToken(nativeArgument)
                                    : (value.dynamic && value.ownsDynamic &&
                                    !isClassType(declaredType)
                                    ? value.value
                                    : std::string()));
                            return true;
                        };
                        auto failOperator = [&] (
                            const IRType::IRInstr& at,
                            const std::string& message
                        ) {
                            failNativeTopLevel(at, message);
                            return false;
                        };
                        NativeValue operatorResult;
                        if (!emitKnownClassMethodCall(
                                ins, methodName, operatorArguments, operatorResult,
                                [&](const std::string& line) { emitLine(line); },
                                [&]() { return freshTmp(); },
                                convertArgument,
                                [&](const std::string& value) { emitRelease(value); },
                                failOperator)) {
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(operatorResult));
                        break;
                    }

                    NativeValue operands[] = { container, keyOrIndex, storedValue };
                    std::string dynamicOperands[3];
                    bool ownsDynamicOperands[3] = { false, false, false };
                    const std::size_t operandCount = isStore ? 3 : 2;
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (nativeI64ArrayIndex && i == 1) continue;
                        if (!toDynamicValue(operands[i], ins,
                                            dynamicOperands[i],
                                            ownsDynamicOperands[i])) {
                            rollback();
                            return false;
                        }
                    }

                    std::string symbol;
                    std::vector<std::string> callArguments;
                    if (nativeI64ArrayIndex && !isStore) {
                        symbol = ins.op == VMOperator::DICT_LOAD_VALUE
                            ? "@zeus_value_array_load_i64_checked"
                            : "@zeus_value_array_load_i64";
                    } else if (nativeI64ArrayIndex) {
                        symbol = "@zeus_value_array_store_i64";
                    } else if (isArrayValue && !isStore) {
                        symbol = ins.op == VMOperator::DICT_LOAD_VALUE
                            ? "@zeus_value_array_load_checked"
                            : "@zeus_value_array_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else if (isArrayValue) {
                        symbol = "@zeus_value_array_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    } else if (!isStore) {
                        symbol = "@zeus_value_subscript_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else {
                        symbol = "@zeus_value_subscript_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    }
                    const auto freshTemporary = [&]() { return freshTmp(); };
                    const auto emitCallLine = [&](const std::string& line) {
                        emitLine(line);
                    };
                    const std::string resultValue = nativeI64ArrayIndex
                        ? ContainerABI::emitArrayIndexCall(
                              symbol, dynamicOperands[0], keyOrIndex.value,
                              isStore, isStore ? dynamicOperands[2] : std::string(),
                              freshTemporary, emitCallLine)
                        : ContainerABI::emitResultCall(
                              symbol, callArguments, freshTemporary, emitCallLine);
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (ownsDynamicOperands[i]) emitRelease(dynamicOperands[i]);
                        if (isManagedClassValue(operands[i]) &&
                            operands[i].ownsClass) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     operands[i].value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

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
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR:
                    if (!emitBinary(ins)) {
                        rollback();
                        return false;
                    }
                    break;

                case VMOperator::LOGICAL_NOT:
                    if (!emitLogicalNot(ins)) {
                        rollback();
                        return false;
                    }
                    break;

                case VMOperator::LOAD_NATIVE: {
                    int64_t nativeId = 0;
                    if (!tryArgInt(ins, 0, nativeId)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_NATIVE id 不是整数");
                        rollback();
                        return false;
                    }

                    std::string nativeName;
                    if (!resolveNativeHandle(nativeId, nativeName)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 lowering 暂不支持该 builtin");
                        rollback();
                        return false;
                    }
                    valueStack.push_back(NativeValue{
                        std::move(nativeName), "builtin", true, false
                    });
                    break;
                }

                case VMOperator::CALL_NATIVE: {
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || !emitVariadicValueCall(ins, argc)) {
                        if (result.error.empty()) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生变长调用参数数量或类型无效");
                        }
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            failNativeTopLevel(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
                            rollback();
                            return false;
                        }
                        if (const auto* function = resolveFunctionSignature(pendingDirectCall)) {
                            pendingDirectCall = function->name;
                        }
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 CALL_CLOSURE 参数不是整数");
                        rollback();
                        return false;
                    }

                    if (!pendingDirectCall.empty()) {
                        auto function = functionSignatures.find(pendingDirectCall);
                        if (function == functionSignatures.end()) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生函数调用签名不匹配");
                            rollback();
                            return false;
                        }

                        std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                        for (int64_t i = argc - 1; i >= 0; --i) {
                            if (!popValue(ins, arguments[static_cast<size_t>(i)])) {
                                rollback();
                                return false;
                            }
                        }

                        NativeValue callee;
                        if (!popValue(ins, callee)) {
                            rollback();
                            return false;
                        }
                        // Argument evaluation can load additional function
                        // values, so validate the callee actually popped from
                        // the operand stack instead of the last loaded symbol.
                        if (callee.dynamic &&
                            ins.metadata.tag == "static_function_alias") {
                            releaseNativeValue(callee);
                            callee = NativeValue{
                                pendingDirectCall, "function", true, false
                            };
                        }
                        if (callee.dynamic) {
                            NativeValue closureResult;
                            if (!emitClosureCallTopLevel(ins, callee, arguments, closureResult)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(std::move(closureResult));
                            pendingDirectCall.clear();
                            pendingFunctionLoad.clear();
                            break;
                        }
                        if (!callee.functionRef ||
                            !matchesSpecializedFunctionReference(
                                callee.value, pendingDirectCall)) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生函数调用缺少匹配的函数引用");
                            rollback();
                            return false;
                        }

                        std::vector<NativeValue> orderedArguments;
                        std::vector<bool> providedArguments;
                        std::vector<NativeValue> variadicArguments;
                        std::vector<std::pair<std::string, NativeValue>> keywordArguments;
                        std::string bindingError;
                        if (!bindCallArguments(function->second, arguments,
                                orderedArguments, providedArguments, variadicArguments,
                                keywordArguments, bindingError)) {
                            failNativeTopLevel(ins, "[emit-llvm] " + bindingError);
                            rollback();
                            return false;
                        }

                        const bool callIsVoidReturn = function->second.isExternal
                            ? (function->second.returnType == "void" || function->second.returnType == "nil")
                            : (nativeExportFunctions.count(function->second.name) != 0 &&
                               (function->second.returnType.empty() ||
                                function->second.returnType == "nil" ||
                                function->second.returnType == "void"));
                        const bool returnsDynamic =
                            !callIsVoidReturn && usesZeusValueABI(function->second.returnType);
                        const std::string returnType = returnsDynamic || callIsVoidReturn
                            ? "void"
                            : llvmType(function->second.returnType);
                        const std::string classReturnType =
                            isReferenceTypeName(function->second.returnType)
                                ? referencedTypeName(function->second.returnType)
                                : function->second.returnType;
                        const bool returnsReferenceClass =
                            isReferenceTypeName(function->second.returnType) &&
                            isClassType(classReturnType);
                        if (isReferenceTypeName(function->second.returnType) &&
                            !returnsReferenceClass) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] ->& return requires a managed class value");
                            rollback();
                            return false;
                        }
                        if (returnType.empty()) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生函数返回类型没有 LLVM ABI 映射");
                            rollback();
                            return false;
                        }

                        std::vector<std::string> nativeArguments;
                        std::vector<std::string> dynamicArguments;
                        std::vector<bool> ownsDynamicArguments;
                        std::vector<std::string> nativeBytesArguments;
                        std::vector<std::string> nativeStringArguments;
                        for (size_t i = 0; i < function->second.parameterTypes.size(); ++i) {
                            const bool byReference =
                                i < function->second.parameterIsReference.size() &&
                                function->second.parameterIsReference[i];
                            if (!providedArguments[i]) {
                                if (byReference) {
                                    failNativeTopLevel(ins,
                                        "[emit-llvm] a reference parameter cannot use a default argument");
                                    rollback();
                                    return false;
                                }
                                NativeValue placeholder;
                                if (!emitOptionalArgumentPlaceholder(
                                        function->second.parameterTypes[i], placeholder,
                                        [this](const std::string& line) { emitLine(line); },
                                        [this]() { return freshTmp(); })) {
                                    failNativeTopLevel(ins,
                                        "[emit-llvm] optional argument has no native ABI placeholder");
                                    rollback();
                                    return false;
                                }
                                if (usesZeusValueABI(function->second.parameterTypes[i])) {
                                    dynamicArguments.push_back(placeholder.value);
                                    ownsDynamicArguments.push_back(false);
                                }
                                nativeArguments.push_back(placeholder.value);
                                continue;
                            }
                            std::string nativeArgument;
                            const std::string& parameterType =
                                function->second.parameterTypes[i];
                            if (byReference) {
                                const std::string parameterName = i <
                                        function->second.parameterNames.size()
                                    ? function->second.parameterNames[i]
                                    : std::string();
                                const IRType::CallArgumentSource* source = nullptr;
                                for (const auto& candidate : ins.metadata.callArguments) {
                                    const bool matches = candidate.namedParameter.empty()
                                        ? candidate.positionalIndex == static_cast<int>(i)
                                        : candidate.namedParameter == parameterName;
                                    if (matches) {
                                        source = &candidate;
                                        break;
                                    }
                                }
                                if (!source || !source->isAssignable ||
                                    source->source.kind != IRType::SymbolRefKind::Global ||
                                    source->source.isConst) {
                                    failNativeTopLevel(ins,
                                        "[emit-llvm] a top-level reference argument must be a mutable global variable");
                                    rollback();
                                    return false;
                                }
                                if (!flushNativeGlobalForReference(
                                        source->source.index, ins)) {
                                    rollback();
                                    return false;
                                }
                                nativeArgument = freshTmp();
                                emitLine(nativeArgument +
                                    " = call ptr @zeus_value_global_slot(i32 " +
                                    std::to_string(source->source.index) + ")");
                                emitReferenceArgumentTypeCheck(
                                    nativeArgument,
                                    function->second,
                                    i,
                                    [this](const std::string& line) { emitLine(line); });
                                if (function->second.isExternal &&
                                    function->second.nativeReferenceABI != "slot") {
                                    const std::string slot = nativeArgument;
                                    emitExternalReferenceArgument(
                                        parameterType, slot, nativeArgument,
                                        [this](const std::string& line) { emitLine(line); },
                                        [this]() { return freshTmp(); });
                                }
                            } else {
                                if (orderedArguments[i].dynamic) {
                                    const std::string nativeParameterType =
                                        llvmType(parameterType);
                                    if (nativeParameterType != "i64" &&
                                        nativeParameterType != "i1") {
                                        emitDynamicArgumentTypeCheck(
                                            orderedArguments[i].value,
                                            function->second,
                                            i,
                                            [this](const std::string& line) {
                                                emitLine(line);
                                            });
                                    }
                                }
                                if (usesZeusValueABI(parameterType)) {
                                    if (!emitNativeValueAsDynamic(
                                            orderedArguments[i], ins, nativeArgument)) {
                                        rollback();
                                        return false;
                                    }
                                    dynamicArguments.push_back(nativeArgument);
                                    ownsDynamicArguments.push_back(
                                        orderedArguments[i].dynamic
                                            ? orderedArguments[i].ownsDynamic
                                            : true);
                                } else {
                                    bool ownsNativeString = false;
                                    if (!emitNativeValueAsType(
                                            orderedArguments[i], llvmType(parameterType), ins,
                                            nativeArgument, parameterType, false,
                                            &function->second, i, &ownsNativeString)) {
                                        rollback();
                                        return false;
                                    }
                                    if (parameterType == "bytes" &&
                                        orderedArguments[i].dynamic) {
                                        nativeBytesArguments.push_back(nativeArgument);
                                    }
                                    if (ownsNativeString) {
                                        nativeStringArguments.push_back(nativeArgument);
                                    }
                                }
                            }
                            nativeArguments.push_back(nativeArgument);
                        }

                        const std::string callResult = freshTmp();
                        std::string callText;
                        if (returnsDynamic) {
                            emitLine(callResult + " = alloca %ZeusValue, align 8");
                            callText = "call void " + llvmCallTarget(function->second.name) +
                                       "(ptr " + callResult;
                        } else if (callIsVoidReturn) {
                            callText = "call void " +
                                       llvmCallTarget(function->second.name) + "(";
                        } else {
                            callText = callResult + " = call " + returnType + " " +
                                       llvmCallTarget(function->second.name) + "(";
                        }
                        for (size_t i = 0; i < nativeArguments.size(); ++i) {
                            if (i > 0 || returnsDynamic) callText += ", ";
                            callText += functionParameterABIType(
                                            function->second.parameterTypes[i],
                                            i < function->second.parameterIsReference.size() &&
                                                function->second.parameterIsReference[i]) +
                                        " " + nativeArguments[i];
                        }
                        std::string packedArgs;
                        std::string packedKwargs;
                        if (function->second.hasVariadicArgs || function->second.hasVariadicKwargs) {
                            if (!emitVariadicContainers(
                                    function->second.hasVariadicArgs,
                                    function->second.hasVariadicKwargs,
                                    variadicArguments, keywordArguments,
                                    [&](const NativeValue& value, std::string& out, bool& owns) {
                                        return toDynamicValue(value, ins, out, owns);
                                    }, [this]() { return freshTmp(); },
                                    [this](const std::string& line) { emitLine(line); },
                                    packedArgs, packedKwargs)) {
                                rollback();
                                return false;
                            }
                            if (!packedArgs.empty()) {
                                if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
                                callText += "ptr " + packedArgs;
                            }
                            if (!packedKwargs.empty()) {
                                if (!nativeArguments.empty() || returnsDynamic ||
                                    !packedArgs.empty()) callText += ", ";
                                callText += "ptr " + packedKwargs;
                            }
                        }
                        if (function->second.hasDefaultParameters()) {
                            if (!nativeArguments.empty() || returnsDynamic ||
                                !packedArgs.empty() || !packedKwargs.empty()) callText += ", ";
                            callText += "i64 " + std::to_string(providedArgumentMask(providedArguments));
                        }
                        callText += ")";
                        emitLine(callText);
                        if (!packedArgs.empty()) emitRelease(packedArgs);
                        if (!packedKwargs.empty()) emitRelease(packedKwargs);
                        std::set<std::string> releasedDynamicArguments;
                        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                            if (ownsDynamicArguments[i] &&
                                releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                                emitRelease(dynamicArguments[i]);
                            }
                        }
                        for (const auto& nativeBytes : nativeBytesArguments) {
                            emitLine("call void @zeus_bytes_release(ptr " +
                                     nativeBytes + ")");
                        }
                        for (const auto& nativeString : nativeStringArguments) {
                            emitLine("call void @zeus_string_release(ptr " +
                                     nativeString + ")");
                        }
                        for (const auto& argument : arguments) {
                            if (argument.dynamic && argument.ownsDynamic &&
                                releasedDynamicArguments.insert(argument.value).second) {
                                emitRelease(argument.value);
                            }
                            if (argument.ownsNativeBytes) {
                                emitLine("call void @zeus_bytes_release(ptr " +
                                         argument.value + ")");
                            }
                            if (argument.ownsNativeString) {
                                emitLine("call void @zeus_string_release(ptr " +
                                         argument.value + ")");
                            }
                            if (isManagedClassValue(argument) && argument.ownsClass) {
                                emitLine("call void @zeus_class_ref_release(ptr " +
                                         argument.value + ")");
                            }
                        }
                        valueStack.push_back(returnsDynamic
                            ? NativeValue{
                                callResult, "ptr", false, true,
                                function->second.returnType, true
                              }
                            : NativeValue{
                                callResult, returnType, false, false,
                                classReturnType, false
                              });
                        valueStack.back().ownsClass =
                            !returnsDynamic && isClassType(classReturnType);
                        valueStack.back().ownsNativeBytes = !returnsDynamic &&
                            function->second.returnType == "bytes";
                        valueStack.back().ownsNativeString = !returnsDynamic &&
                            function->second.returnType == "string";
                        pendingDirectCall.clear();
                        pendingFunctionLoad.clear();
                        break;
                    }

                    if (!pendingVariadicCall.empty()) {
                        if (!emitVariadicValueCall(ins, argc)) {
                            if (result.error.empty()) {
                                failNativeTopLevel(ins, "[emit-llvm] 原生变长调用参数数量或类型无效");
                            }
                            rollback();
                            return false;
                        }
                        break;
                    }

                    const size_t calleePosition = valueStack.size() >=
                        static_cast<size_t>(argc) + 1
                            ? valueStack.size() - static_cast<size_t>(argc) - 1
                            : valueStack.size();
                    if (calleePosition < valueStack.size() &&
                        valueStack[calleePosition].functionRef &&
                        valueStack[calleePosition].type == "builtin") {
                        if (!emitVariadicValueCall(ins, argc)) {
                            if (result.error.empty()) {
                                failNativeTopLevel(ins,
                                    "[emit-llvm] 原生内置函数参数数量或类型无效");
                            }
                            rollback();
                            return false;
                        }
                        break;
                    }

                    if (calleePosition < valueStack.size() &&
                        valueStack[calleePosition].dynamic &&
                        valueStack.size() >= static_cast<size_t>(argc) + 1) {
                        std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                        for (int64_t i = argc - 1; i >= 0; --i) {
                            if (!popValue(ins, arguments[static_cast<size_t>(i)])) {
                                rollback();
                                return false;
                            }
                        }
                        NativeValue callee;
                        if (!popValue(ins, callee)) {
                            rollback();
                            return false;
                        }
                        NativeValue closureResult;
                        if (!emitClosureCallTopLevel(ins, callee, arguments, closureResult)) {
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(closureResult));
                        break;
                    }

                    failNativeTopLevel(ins, "[emit-llvm] 原生闭包调用缺少 direct_call metadata");
                    rollback();
                    return false;
                }

                case VMOperator::JUMP:
                case VMOperator::LOOP_BREAK: {
                    if (!valueStack.empty()) {
                        failNativeTopLevel(ins,
                                           "[emit-llvm] 标量控制流出口仍有未消费的 VM 栈值");
                        rollback();
                        return false;
                    }

                    int64_t off = 0;
                    if (!tryArgInt(ins, 0, off)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生跳转偏移不是整数");
                        rollback();
                        return false;
                    }

                    const long long target = static_cast<long long>(pc) + 1 + off;
                    if (target <= static_cast<long long>(pc)) {
                        emitLine("call void @zeus_gc_safepoint()");
                    }
                    const int jumpTargetLevel =
                        target == static_cast<long long>(chunk.size())
                            ? 0
                            : entryTryLevelBefore(static_cast<size_t>(target));
                    emitEntryTryExits(entryTryLevelBefore(pc) - jumpTargetLevel);
                    emitLine(target == static_cast<long long>(chunk.size())
                        ? "br label " + endLabelFor(ci)
                        : "br label " + labelFor(ci, static_cast<size_t>(target)));
                    blkTerminated = true;
                    blkNeedLabel = true;
                    controlFlowStarted = true;
                    break;
                }

                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_TRUE: {
                    NativeValue condition;
                    if (!popValue(ins, condition) || condition.functionRef) {
                        rollback();
                        return false;
                    }

                    if (!valueStack.empty()) {
                        failNativeTopLevel(ins,
                                           "[emit-llvm] 条件分支仍有未消费的 VM 栈值");
                        rollback();
                        return false;
                    }

                    std::string booleanValue;
                    if (condition.dynamic) {
                        const std::string truthy = freshTmp();
                        emitLine(truthy + " = call i32 @zeus_value_truthy(ptr " + condition.value + ")");
                        emitRelease(condition.value);
                        booleanValue = freshTmp();
                        emitLine(booleanValue + " = icmp ne i32 " + truthy + ", 0");
                    } else if (condition.type == "i1") {
                        booleanValue = condition.value;
                    } else if (condition.type == "i64") {
                        booleanValue = freshTmp();
                        emitLine(booleanValue + " = icmp ne i64 " + condition.value + ", 0");
                    } else {
                        failNativeTopLevel(ins, "[emit-llvm] 条件分支要求 bool/int 值");
                        rollback();
                        return false;
                    }

                    int64_t off = 0;
                    if (!tryArgInt(ins, 0, off)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生条件跳转偏移不是整数");
                        rollback();
                        return false;
                    }

                    const long long target = static_cast<long long>(pc) + 1 + off;
                    const size_t fallthrough = pc + 1;
                    const std::string targetLabel =
                        target == static_cast<long long>(chunk.size())
                            ? endLabelFor(ci)
                            : labelFor(ci, static_cast<size_t>(target));
                    const std::string fallthroughLabel =
                        fallthrough == chunk.size()
                            ? endLabelFor(ci)
                            : labelFor(ci, fallthrough);

                    if (target <= static_cast<long long>(pc)) {
                        emitLine("call void @zeus_gc_safepoint()");
                    }
                    const int targetExits = entryTryLevelBefore(pc) -
                        (target == static_cast<long long>(chunk.size())
                            ? 0
                            : entryTryLevelBefore(static_cast<size_t>(target)));
                    const int fallthroughExits = entryTryLevelBefore(pc) -
                        (fallthrough == chunk.size()
                            ? 0
                            : entryTryLevelBefore(fallthrough));
                    std::string edgeBlocks;
                    const std::string takenLabel = entryTryExitEdgeTarget(
                        targetLabel, targetExits, edgeBlocks);
                    const std::string skipLabel = entryTryExitEdgeTarget(
                        fallthroughLabel, fallthroughExits, edgeBlocks);
                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        emitLine("br i1 " + booleanValue + ", label " +
                                 skipLabel + ", label " + takenLabel);
                    } else {
                        emitLine("br i1 " + booleanValue + ", label " +
                                 takenLabel + ", label " + skipLabel);
                    }
                    body += edgeBlocks;

                    blkTerminated = true;
                    blkNeedLabel = true;
                    controlFlowStarted = true;
                    break;
                }

                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO: {
                    NativeValue checked;
                    if (!popValue(ins, checked)) {
                        rollback();
                        return false;
                    }
                    const std::string checkCall = rangeCheckCall(checked, ins);
                    if (!checkCall.empty()) emitLine(checkCall);
                    releaseNativeValue(checked);
                    break;
                }

                default:
                    failNativeTopLevel(ins, "[emit-llvm] 原生 lowering 遇到未处理指令");
                    rollback();
                    return false;
            }
        }

        activeEntryExceptionTarget = -1;
        if (hasControlFlow && exitJump) {
            emitLabelDef(endLabelFor(ci));

            if (!isLast) {
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true;
            }
        }

        for (const auto& value : valueStack) releaseNativeValue(value);
        valueStack.clear();
        if (!pendingVariadicCall.empty() || !pendingDirectCall.empty() ||
            !pendingFunctionLoad.empty()) {
            ZEUS_DEBUG_PRINT(
                "[emit-llvm] native top-level chunk %zu ended with variadic='%s', direct='%s', function='%s'",
                ci, pendingVariadicCall.c_str(),
                pendingDirectCall.c_str(), pendingFunctionLoad.c_str());
            rollback();
            return false;
        }

        return true;
    }

    bool emitChunk(const std::vector<IRType::IRInstr>& chunk, size_t ci, bool isLast) {
        if (!chunk.empty() && chunk.front().op == VMOperator::FUNC_BEGIN) {
            if (!entryLabelEmitted) {
                emitLabelDef("%L0_0");
                entryLabelEmitted = true;
            } else if (blkNeedLabel) {
                emitLabelDef(labelFor(ci, 0));
            }

            // 函数定义不属于 zeus_entry 的执行流；入口只需要跳过该 chunk。
            if (!isLast) {
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true;
            }

            if (reachableFunctionChunks.find(ci) == reachableFunctionChunks.end()) {
                return true;
            }
            return emitNativeFunction(chunk, ci);
        }

        bool classDeclaration = false;
        for (const auto& ins : chunk) {
            if (ins.op == VMOperator::CLASS_BUILD ||
                ins.op == VMOperator::CLASS_BUILD_SHARED) {
                classDeclaration = true;
                break;
            }
        }

        if (classDeclaration) {
            // 静态类函数已经单独生成了 LLVM function；类注册本身属于 VM
            // 运行时协议，AOT 入口不需要再执行 CLASS_BUILD。
            if (!entryLabelEmitted) {
                emitLabelDef("%L0_0");
                entryLabelEmitted = true;
            } else if (blkNeedLabel) {
                emitLabelDef(labelFor(ci, 0));
            }

            if (!isLast) {
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true;
            }
            return true;
        }

        if (chunk.empty()) {
            // 本 chunk 一般无需出码；唯一例外是 chunk 0：entry 块的
            // br %L0_0 要求的 label 必须存在。被跳过的静态类 chunk
            // 后面可能还有的 chunk，也要保留控制流桥接 label。
            if (ci == 0 || blkNeedLabel) {
                emitLabelDef(ci == 0 ? "%L0_0" : labelFor(ci, 0));
                entryLabelEmitted = true;

                if (!isLast) {
                    emitLine("br label " + labelFor(ci + 1, 0));
                    blkTerminated = true;
                    blkNeedLabel = true;
                }
            }

            return result.error.empty();
        }

        if (emitNativeTopLevelChunk(chunk, ci, isLast)) {
            return true;
        }
        if (!result.error.empty()) {
            return false;
        }

        const IRType::IRInstr* llvmAddressInstruction = nullptr;
        for (const auto& ins : chunk) {
            if (ins.op == VMOperator::LLVM_ADDR ||
                ins.op == VMOperator::LLVM_FADDR ||
                ins.op == VMOperator::LLVM_FSTORE ||
                ins.op == VMOperator::LLVM_SIZEOF) {
                llvmAddressInstruction = &ins;
                break;
            }
        }
        if (llvmAddressInstruction != nullptr) {
            if (result.error.empty()) {
                fail(*llvmAddressInstruction,
                     "[emit-llvm] LLVM native block contains an instruction "
                     "unsupported by native LLVM lowering");
            }
            return false;
        }

        std::vector<int64_t> entryExceptionTarget(chunk.size(), -1);
        // Active top-level try regions per pc (see emitNativeFunction).
        std::vector<int> entryTryLevel(chunk.size(), 0);
        std::vector<size_t> entryExceptionStack;
        std::vector<size_t> entryFinExitHandlerStack;
        bool entryFinExitHandlerStackActive = false;
        std::vector<size_t> entryCatchStack;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            if (ins.op == VMOperator::TRY_BEGIN) {
                int64_t offset = 0;
                if (!tryArgInt(ins, 0, offset) || offset <= 0 ||
                    static_cast<uint64_t>(offset) >= chunk.size() - pc ||
                    chunk[pc + static_cast<size_t>(offset)].op != VMOperator::CATCH_BEGIN) {
                    fail(ins, "[emit-llvm] invalid top-level TRY_BEGIN catch target");
                    return false;
                }
                entryHasTry = true;
                needsRuntimeErrorFactory = true;
                entryExceptionStack.push_back(pc + static_cast<size_t>(offset));
            } else if (ins.op == VMOperator::TRY_END) {
                if (ins.metadata.tag == "fin_exit_handler" &&
                    !entryFinExitHandlerStackActive) {
                    entryFinExitHandlerStack = entryExceptionStack;
                    entryFinExitHandlerStackActive = true;
                }
                if (entryExceptionStack.empty()) {
                    fail(ins, "[emit-llvm] top-level TRY_END without TRY_BEGIN");
                    return false;
                }
                entryTryLevel[pc] = static_cast<int>(entryExceptionStack.size());
                entryExceptionStack.pop_back();
            } else if (!entryExceptionStack.empty()) {
                entryExceptionTarget[pc] = static_cast<int64_t>(entryExceptionStack.back());
                entryTryLevel[pc] = static_cast<int>(entryExceptionStack.size());
            }
            if (ins.op == VMOperator::CATCH_BEGIN) entryCatchStack.push_back(pc);
            if (ins.op == VMOperator::CATCH_END) {
                if (entryCatchStack.empty()) {
                    fail(ins, "[emit-llvm] top-level CATCH_END without CATCH_BEGIN");
                    return false;
                }
                entryCatchStack.pop_back();
            }
            if (ins.metadata.tag == "fin_exit_terminator" &&
                entryFinExitHandlerStackActive) {
                entryExceptionStack = std::move(entryFinExitHandlerStack);
                entryFinExitHandlerStack.clear();
                entryFinExitHandlerStackActive = false;
            }
        }
        if (!entryExceptionStack.empty() || !entryCatchStack.empty() ||
            entryFinExitHandlerStackActive) {
            result.error = "[emit-llvm] unterminated top-level try/catch";
            return false;
        }

        // Pass 1：只收集跳转目标。目标 == size 是合法的“离开该 chunk”
        // （如末尾 if 的 false 分支），映射到出口块；条件分支的 fall through
        // size 同理（隐式离开）。直接 fallthrough 不打 label。
        std::set<size_t> labels;
        bool exitJump = false;

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];

            if (ins.op == VMOperator::CATCH_BEGIN) labels.insert(pc);

            if (ins.op == VMOperator::JUMP ||
                ins.op == VMOperator::JUMP_IF_FALSE ||
                ins.op == VMOperator::JUMP_IF_TRUE ||
                ins.op == VMOperator::LOOP_BREAK) {
                int64_t off = 0;

                if (!tryArgInt(ins, 0, off)) {
                    fail(ins, "[emit-llvm] 跳转偏移不是整数");
                    return false;
                }

                long long tgt = (long long)pc + 1 + off;

                if (tgt < 0 || tgt > (long long)chunk.size()) {
                    fail(ins, "[emit-llvm] 跳转目标越界");
                    return false;
                }

                if (tgt == (long long)chunk.size()) {
                    exitJump = true;
                } else {
                    labels.insert((size_t)tgt);
                }

                if ((ins.op == VMOperator::JUMP_IF_FALSE ||
                     ins.op == VMOperator::JUMP_IF_TRUE)) {
                    if (pc + 1 < chunk.size()) {
                        labels.insert(pc + 1); // 条件分支的 fallthrough 后继必须有块
                    } else {
                        exitJump = true; // fall through chunk 尾= 隐式离开
                    }
                }
            }
        }

        // Dynamic try-depth bookkeeping for top-level code (see
        // emitNativeFunction for the same scheme on regular functions).
        auto emitEntryTryExits = [&](int count) {
            for (int index = 0; index < count; ++index) {
                body += "  call void @zeus_try_exit()\n";
            }
        };
        auto entryTryLevelBefore = [&](size_t pc) -> int {
            return pc < entryTryLevel.size() ? entryTryLevel[pc] : 0;
        };
        auto entryTryExitEdgeTarget = [&](const std::string& realLabel, int exits,
                                          std::string& outBlocks) -> std::string {
            if (exits <= 0) return realLabel;
            const std::string wrapper = freshTmp();
            outBlocks += wrapper.substr(1) + ":\n";
            for (int index = 0; index < exits; ++index) {
                outBlocks += "  call void @zeus_try_exit()\n";
            }
            outBlocks += "  br label " + realLabel + "\n";
            return wrapper;
        };

        // Pass 2：逐条翻译（label 只出现在控制流需要处）。
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            activeEntryExceptionTarget = entryExceptionTarget[pc];
            activeEntryChunk = ci;

            // L0_0 无条件存在（entry 的 br 到它），其余 label 只出现在控制流需要处。
            if (labels.count(pc) || blkNeedLabel || (blkTerminated && pc == 0) ||
                (ci == 0 && pc == 0)) {
                std::string label = labelFor(ci, pc);
                if (!entryLabelEmitted && blkTerminated && pc == 0) {
                    label = "%L0_0";
                    entryLabelEmitted = true;
                }
                emitLabelDef(label);
            }

            if (blkTerminated) {
                continue; // 终结后的不可达指令，跳过
            }

            std::string comment = "; chunk #" + std::to_string(ci) + " pc=" + std::to_string(pc);

            if (ins.line >= 0) comment += " [L" + std::to_string(ins.line) + "]";

            comment += " " + detail::escapeLLVMCommentText(ins.toString());
            body += "  " + comment + "\n";

            switch (ins.op) {
                case VMOperator::LLVM_ADDR:
                case VMOperator::LLVM_FADDR:
                case VMOperator::LLVM_FSTORE:
                case VMOperator::LLVM_SIZEOF:
                    fail(ins,
                        "[emit-llvm] native LLVM operation requires the native lowering path");
                    return false;

                case VMOperator::TRY_BEGIN:
                    {
                        int64_t catchOffset = 0;
                        if (tryArgInt(ins, 0, catchOffset) && catchOffset > 0) {
                            entryNativeStringTryBaselines[
                                std::make_pair(ci, pc +
                                    static_cast<size_t>(catchOffset))] =
                                entryNativeStringOwnerGeneration;
                        }
                    }
                    body += "  call void @zeus_try_enter()\n";
                    break;

                case VMOperator::TRY_END:
                    body += "  call void @zeus_try_exit()\n";
                    break;

                case VMOperator::CATCH_END:
                    break;

                case VMOperator::CATCH_BEGIN: {
                    const auto baseline = entryNativeStringTryBaselines.find(
                        std::make_pair(ci, pc));
                    if (baseline != entryNativeStringTryBaselines.end()) {
                        entryNativeStringOwners.erase(std::remove_if(
                            entryNativeStringOwners.begin(), entryNativeStringOwners.end(),
                            [&](const std::string& owner) {
                                const auto generation =
                                    entryNativeStringOwnerGenerations.find(owner);
                                if (generation != entryNativeStringOwnerGenerations.end() &&
                                    generation->second <= baseline->second) {
                                    return false;
                                }
                                entryNativeStringOwnerGenerations.erase(owner);
                                return true;
                            }), entryNativeStringOwners.end());
                    }
                    const auto slot = entryExceptionSlots.find(std::make_pair(ci, pc));
                    if (slot == entryExceptionSlots.end()) {
                        emitLine("unreachable");
                        blkTerminated = true;
                        break;
                    }
                    int64_t localSlot = -1;
                    if (tryArgInt(ins, 1, localSlot) && localSlot >= 0) {
                        emitLine("call void @zeus_value_local_store(i32 " +
                            std::to_string(localSlot) + ", ptr " + slot->second + ")");
                        nativeLocals.erase(static_cast<int>(localSlot));
                        nativeLocalSlots.erase(static_cast<int>(localSlot));
                    } else {
                        emitLine("call void @zeus_value_release(ptr " + slot->second + ")");
                    }
                    break;
                }

                case VMOperator::THROW: {
                    const std::string thrown = emitPop();
                    emitLine("call void @zeus_exception_raise_owned(ptr " + thrown + ")");
                    emitLine("unreachable");
                    blkTerminated = true;
                    break;
                }

                case VMOperator::IR_ANNOTATION: {
                    if (annotationKind(ins, "direct_call")) {
                        std::string directCallName;
                        if (!annotationSymbolName(ins, directCallName)) {
                            fail(ins, "[emit-llvm] 直接调用标注缺少函数名");
                            return false;
                        }
                        const auto* target = resolveFunctionSignature(directCallName);
                        pendingDirectCall = target != nullptr &&
                            !hasCompleteFunctionAnnotations(*target)
                                ? std::string()
                                : (target != nullptr ? target->name : directCallName);
                    }
                    break;
                }

                case VMOperator::IMPORT:
                case VMOperator::TYPE_IMPL_BUILD:
                    break;

                case VMOperator::PUSH: {
                    if (!ins.args.empty() &&
                        std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                        const std::string nilValue = emitDynamicValueForNull(ins);
                        if (nilValue.empty()) return false;
                        emitPushValue(nilValue);
                        break;
                    }
                    if (!ins.args.empty() &&
                        std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                        const std::string bytesValue = emitDynamicValueForBytes(
                            std::get<ZeusType::Bytes>(ins.args[0]), ins);
                        if (bytesValue.empty()) return false;
                        emitPushValue(bytesValue);
                        break;
                    }
                    if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        fail(ins, "[emit-llvm] v0: PUSH 仅支持 Value/字面量");
                        return false;
                    }

                    std::string t = emitDynamicValueForLiteral(std::get<LexerType::Value>(ins.args[0]), ins);

                    if (t.empty()) return false; // emitDynamicValueForLiteral already failed

                    emitPushValue(t);
                    break;
                }

                case VMOperator::PUSH_NAMED:
                    result.error = "[emit-llvm] dynamic stack lowering cannot preserve named argument metadata";
                    return false;

                case VMOperator::LOAD_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        fail(ins, "[emit-llvm] LOAD_VALUE_SHARED 缺少名称");
                        return false;
                    }
                    const std::string value = freshTmp();
                    emitLine(value + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_shared_value_load(ptr " + value +
                             ", ptr " + poolStr(std::get<std::string>(ins.args[0])) + ")");
                    emitPushValue(value);
                    break;
                }

                case VMOperator::POP: {
                    std::string t = emitPop();
                    emitRelease(t);
                    break;
                }

                case VMOperator::SWAP: {
                    const std::string top = emitPop();
                    const std::string below = emitPop();
                    emitPushValue(top);
                    emitPushValue(below);
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    // 形态：单个 Value 或 字面量。
                    if (ins.args.size() == 1 && std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        std::string t = emitDynamicValueForLiteral(std::get<LexerType::Value>(ins.args[0]), ins);

                        if (t.empty()) return false;

                        emitPushValue(t);
                        break;
                    }

                    // 形态：单个 string 、未解析名或已知动态变长 ABI。
                    if (ins.args.size() == 1 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);

                        if (!dynamicVariadicABISymbol(name).empty()) {
                            pendingVariadicCallStack.push_back(name);
                            break;
                        }

                        if (const auto* function = resolveFunctionSignature(name)) {
                            pendingFunctionLoad = function->name;
                            break; // 原生函数引用只作为直接调用的元信息存。
                        }

                        fail(ins, "[emit-llvm] v0: 未知名字 '" + name + "'（freestanding：仅已注册的动态变长 ABI 可用）");
                        return false;
                    }

                    // 形态：scope + index。
                    if (ins.args.size() >= 2 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& kind = std::get<std::string>(ins.args[0]);
                        int64_t idx = 0;

                        if (!tryArgInt(ins, 1, idx)) {
                            fail(ins, "[emit-llvm] LOAD 索引不是整数");
                            return false;
                        }

                        if (kind == "global") {
                            if (isFunctionGlobal(static_cast<int>(idx))) {
                                pendingFunctionLoad = functionGlobals[static_cast<int>(idx)];
                                break;
                            }

                            std::string t;
                            auto slot = nativeGlobalSlots.find(static_cast<int>(idx));
                            if (slot != nativeGlobalSlots.end()) {
                                NativeValue slotValue{
                                    freshTmp(), slot->second.type, false, false,
                                    slot->second.sourceType
                                };
                                emitLine(slotValue.value + " = load " + slotValue.type +
                                         ", ptr " + slot->second.address);
                                if (!emitNativeValueAsDynamic(slotValue, ins, t)) return false;
                            } else {
                                auto native = nativeGlobals.find(static_cast<int>(idx));
                                if (native != nativeGlobals.end()) {
                                    if (!emitNativeValueAsDynamic(native->second, ins, t)) return false;
                                } else {
                                    t = emitGlobalValueLoad(static_cast<int>(idx));
                                }
                            }
                            emitPushValue(t);
                        } else if (kind == "local") {
                            std::string t;
                            auto slot = nativeLocalSlots.find(static_cast<int>(idx));
                            if (slot != nativeLocalSlots.end()) {
                                NativeValue slotValue{
                                    freshTmp(), slot->second.type, false, false,
                                    slot->second.sourceType
                                };
                                emitLine(slotValue.value + " = load " + slotValue.type +
                                         ", ptr " + slot->second.address);
                                if (!emitNativeValueAsDynamic(slotValue, ins, t)) return false;
                            } else {
                                auto native = nativeLocals.find(static_cast<int>(idx));
                                if (native != nativeLocals.end()) {
                                    if (!emitNativeValueAsDynamic(native->second, ins, t)) return false;
                                } else {
                                    t = emitLocalValueLoad(static_cast<int>(idx));
                                }
                            }
                            emitPushValue(t);
                        } else {
                            fail(ins, "[emit-llvm] v0: LOAD_" + kind + " 暂不支持");
                            return false;
                        }
                        break;
                    }

                    fail(ins, "[emit-llvm] v0: LOAD_VALUE 形态暂不支持");
                    return false;
                }

                case VMOperator::MAKE_FUNCTION: {
                    int64_t protoId = 0;
                    if (!tryArgInt(ins, 0, protoId) ||
                        functionSignaturesByChunk.find(static_cast<size_t>(protoId)) == functionSignaturesByChunk.end()) {
                        fail(ins, "[emit-llvm] MAKE_FUNCTION 没有对应的原生函数签名标注");
                        return false;
                    }
                    // 原生函数定义已经直接生成 LLVM function，动态闭包对象无需创建。
                    break;
                }

                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        fail(ins, "[emit-llvm] shared store 缺少名称");
                        return false;
                    }
                    const std::string value = emitPop();
                    const char* operation = ins.op == VMOperator::STORE_VALUE_SHARED
                        ? "store" : "assign";
                    emitLine(std::string("call void @zeus_shared_value_") + operation +
                        "(ptr " + poolStr(std::get<std::string>(ins.args[0])) +
                        ", ptr " + value + ")");
                    emitRelease(value);
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 ||
                        !std::holds_alternative<std::string>(ins.args[0]) ||
                        !std::holds_alternative<std::string>(ins.args[1])) {
                        fail(ins, "[emit-llvm] DELETE_VAR 参数无效");
                        return false;
                    }
                    const std::string& kind = std::get<std::string>(ins.args[1]);
                    if (kind == "shared") {
                        emitLine("call i32 @zeus_shared_value_delete(ptr " +
                                 poolStr(std::get<std::string>(ins.args[0])) + ")");
                        break;
                    }
                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0 ||
                        index > std::numeric_limits<int32_t>::max()) {
                        fail(ins, "[emit-llvm] DELETE_VAR 索引无效");
                        return false;
                    }
                    if (kind == "global") {
                        emitLine("call void @zeus_value_global_delete(i32 " +
                                 std::to_string(index) + ")");
                    } else if (kind == "local") {
                        emitLine("call void @zeus_value_local_delete(i32 " +
                                 std::to_string(index) + ")");
                    } else if (kind == "upvalue") {
                        emitLine("call void @zeus_closure_env_delete(ptr %closure.env, i32 " +
                                 std::to_string(index) + ")");
                    } else {
                        fail(ins, "[emit-llvm] DELETE_VAR 变量类型不支持");
                        return false;
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_GLOBAL: {
                    int64_t idx = 0;

                    if (!tryArgInt(ins, 0, idx)) {
                        fail(ins, "[emit-llvm] STORE_GLOBAL 索引不是整数");
                        return false;
                    }

                    if (isFunctionGlobal(static_cast<int>(idx))) {
                        break;
                    }

                    std::string t = emitPop();
                    emitBoxedGlobalIntegerCheck(static_cast<int>(idx), t);
                    const auto nativeViewType =
                        prescanGlobalNativeViewTypes.find(static_cast<int>(idx));
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "string") {
                        const std::string nativeString = freshTmp();
                        emitLine(nativeString +
                            " = call ptr @zeus_value_to_native_string(ptr " + t + ")");
                        emitLine("call void @zeus_global_store_string(i32 " +
                            std::to_string(idx) + ", ptr " + nativeString + ")");
                        emitLine("call void @zeus_string_release(ptr " + nativeString + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(idx));
                        emitRelease(t);
                        nativeGlobals.erase(static_cast<int>(idx));
                        nativeGlobalSlots.erase(static_cast<int>(idx));
                        break;
                    }
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "bytes") {
                        const std::string nativeBytes = freshTmp();
                        emitLine(nativeBytes +
                            " = call ptr @zeus_value_copy_bytes(ptr " + t + ")");
                        emitLine("call void @zeus_global_store_bytes(i32 " +
                            std::to_string(idx) + ", ptr " + nativeBytes + ")");
                        emitLine("call void @zeus_bytes_release(ptr " + nativeBytes + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(idx));
                        emitRelease(t);
                        nativeGlobals.erase(static_cast<int>(idx));
                        nativeGlobalSlots.erase(static_cast<int>(idx));
                        break;
                    }
                    entryDynamicValueGlobals.insert(static_cast<int>(idx));
                    emitLine("call void @zeus_value_global_store(i32 " + std::to_string(idx) + ", ptr " + t + ")");
                    nativeGlobals.erase(static_cast<int>(idx));
                    nativeGlobalSlots.erase(static_cast<int>(idx));
                    break;
                }

                case VMOperator::ASSIGN_VALUE_GLOBAL: {
                    int64_t idx = 0;

                    if (!tryArgInt(ins, 0, idx)) {
                        fail(ins, "[emit-llvm] ASSIGN_GLOBAL 索引不是整数");
                        return false;
                    }

                    if (isFunctionGlobal(static_cast<int>(idx))) {
                        fail(ins, "[emit-llvm] 原生函数引用不可重新赋值");
                        return false;
                    }

                    std::string t = emitPop();
                    emitBoxedGlobalIntegerCheck(static_cast<int>(idx), t);
                    const auto nativeViewType =
                        prescanGlobalNativeViewTypes.find(static_cast<int>(idx));
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "string") {
                        const std::string nativeString = freshTmp();
                        emitLine(nativeString +
                            " = call ptr @zeus_value_to_native_string(ptr " + t + ")");
                        emitLine("call void @zeus_global_store_string(i32 " +
                            std::to_string(idx) + ", ptr " + nativeString + ")");
                        emitLine("call void @zeus_string_release(ptr " + nativeString + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(idx));
                        emitRelease(t);
                        nativeGlobals.erase(static_cast<int>(idx));
                        nativeGlobalSlots.erase(static_cast<int>(idx));
                        break;
                    }
                    if (nativeViewType != prescanGlobalNativeViewTypes.end() &&
                        nativeViewType->second == "bytes") {
                        const std::string nativeBytes = freshTmp();
                        emitLine(nativeBytes +
                            " = call ptr @zeus_value_copy_bytes(ptr " + t + ")");
                        emitLine("call void @zeus_global_store_bytes(i32 " +
                            std::to_string(idx) + ", ptr " + nativeBytes + ")");
                        emitLine("call void @zeus_bytes_release(ptr " + nativeBytes + ")");
                        entryDynamicValueGlobals.insert(static_cast<int>(idx));
                        emitRelease(t);
                        nativeGlobals.erase(static_cast<int>(idx));
                        nativeGlobalSlots.erase(static_cast<int>(idx));
                        break;
                    }
                    entryDynamicValueGlobals.insert(static_cast<int>(idx));
                    emitLine("call void @zeus_value_global_store(i32 " + std::to_string(idx) + ", ptr " + t + ")");
                    nativeGlobals.erase(static_cast<int>(idx));
                    nativeGlobalSlots.erase(static_cast<int>(idx));
                    break;
                }

                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t idx = 0;

                    if (!tryArgInt(ins, 0, idx)) {
                        fail(ins, "[emit-llvm] STORE_LOCAL 索引不是整数");
                        return false;
                    }

                    std::string t = emitPop();
                    emitLine("call void @zeus_value_local_store(i32 " + std::to_string(idx) + ", ptr " + t + ")");
                    nativeLocals.erase(static_cast<int>(idx));
                    nativeLocalSlots.erase(static_cast<int>(idx));
                    break;
                }

                case VMOperator::LIST_BUILD:
                case VMOperator::DICT_BUILD: {
                    int64_t count = 0;
                    if (!tryArgInt(ins, 0, count) || count < 0 ||
                        count > std::numeric_limits<int32_t>::max()) {
                        fail(ins, "[emit-llvm] 容器构造元素数量无效");
                        return false;
                    }
                    const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                    const int64_t valueCount = dictionary ? count * 2 : count;
                    std::vector<std::string> values(
                        static_cast<std::size_t>(valueCount));
                    for (int64_t i = valueCount - 1; i >= 0; --i) {
                        values[static_cast<std::size_t>(i)] = emitPop();
                    }
                    const std::string resultValue = ContainerABI::emitBuildCall(
                        dictionary, count, values,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    for (const auto& value : values) emitRelease(value);
                    emitPushValue(resultValue);
                    break;
                }

                case VMOperator::LIST_LENGTH:
                case VMOperator::DICT_KEYS_BUILD: {
                    const std::string container = emitPop();
                    const std::string symbol = ins.op == VMOperator::LIST_LENGTH
                        ? "@zeus_value_array_length"
                        : "@zeus_value_dict_keys";
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, { container },
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    emitRelease(container);
                    emitPushValue(resultValue);
                    break;
                }

                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE: {
                    const bool isStore = ins.op == VMOperator::LIST_STORE_ELEM ||
                        ins.op == VMOperator::DICT_STORE_VALUE;
                    const bool isList = ins.op == VMOperator::LIST_LOAD_ELEM ||
                        ins.op == VMOperator::LIST_STORE_ELEM;
                    std::string storedValue;
                    if (isStore) storedValue = emitPop();
                    const std::string keyOrIndex = emitPop();
                    const std::string container = emitPop();

                    std::string symbol;
                    std::vector<std::string> callArguments;
                    if (isList && !isStore) {
                        symbol = "@zeus_value_array_load";
                        callArguments = { container, keyOrIndex };
                    } else if (isList) {
                        symbol = "@zeus_value_array_store";
                        callArguments = { container, keyOrIndex, storedValue };
                    } else if (!isStore) {
                        symbol = "@zeus_value_subscript_load";
                        callArguments = { container, keyOrIndex };
                    } else {
                        symbol = "@zeus_value_subscript_store";
                        callArguments = { container, keyOrIndex, storedValue };
                    }

                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, callArguments,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    emitRelease(container);
                    emitRelease(keyOrIndex);
                    if (isStore) emitRelease(storedValue);
                    emitPushValue(resultValue);
                    break;
                }

                case VMOperator::LOGICAL_NOT: {
                    const std::string operand = emitPop();
                    const std::string truthy = freshTmp();
                    emitLine(truthy + " = call i32 @zeus_value_truthy(ptr " + operand + ")");
                    const std::string resultBool = freshTmp();
                    emitLine(resultBool + " = icmp eq i32 " + truthy + ", 0");
                    const std::string resultRaw = freshTmp();
                    emitLine(resultRaw + " = zext i1 " + resultBool + " to i32");
                    const std::string resultValue = freshTmp();
                    emitLine(resultValue + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_init_bool(ptr " + resultValue +
                             ", i32 " + resultRaw + ")");
                    emitPushValue(resultValue);
                    emitRelease(operand);
                    break;
                }

                case VMOperator::ADD: {
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_int_add(ptr " + t + ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::SUBTRACT:
                case VMOperator::MULTIPLY:
                case VMOperator::DIVIDE:
                case VMOperator::MODULUS:
                case VMOperator::EQUL:
                case VMOperator::NOT_EQUAL:
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR: {
                    const int dynamicOpcode = DynamicBinaryABI::opcodeFor(ins.op);
                    if (dynamicOpcode < 0) {
                        fail(ins, "[emit-llvm] dynamic 值暂不支持该二元运算");
                        return false;
                    }
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_dynamic_binary(ptr " + t +
                             ", ptr null, i32 " + std::to_string(dynamicOpcode) +
                             ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::LESS_THAN: {
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_less_than(ptr " + t + ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::GREATER_THAN: {
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_dynamic_binary(ptr " + t +
                             ", ptr null, i32 " +
                             std::to_string(DynamicBinaryABI::opcodeFor(ins.op)) +
                             ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::JUMP:
                case VMOperator::LOOP_BREAK: {
                    int64_t off = 0;
                    tryArgInt(ins, 0, off); // Pass 1 已验证。
                    long long tgt = (long long)pc + 1 + off;

                    if (tgt <= static_cast<long long>(pc)) {
                        emitLine("call void @zeus_gc_safepoint()");
                    }
                    const int jumpTargetLevel = tgt == (long long)chunk.size()
                        ? 0
                        : entryTryLevelBefore((size_t)tgt);
                    emitEntryTryExits(entryTryLevelBefore(pc) - jumpTargetLevel);
                    if (tgt == (long long)chunk.size()) {
                        emitLine("br label " + endLabelFor(ci));
                    } else {
                        emitLine("br label " + labelFor(ci, (size_t)tgt));
                    }

                    blkTerminated = true;
                    blkNeedLabel = true;
                    break;
                }

                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_TRUE: {
                    int64_t off = 0;
                    tryArgInt(ins, 0, off); // Pass 1 已验证。
                    long long tgt = (long long)pc + 1 + off;
                    size_t fall = pc + 1;

                    std::string c = emitPop();
                    std::string tv = freshTmp();
                    emitLine(tv + " = call i32 @zeus_value_truthy(ptr " + c + ")");
                    emitRelease(c);
                    std::string b = freshTmp();
                    emitLine(b + " = icmp ne i32 " + tv + ", 0");

                    std::string tgtLabel = (tgt == (long long)chunk.size())
                        ? endLabelFor(ci)
                        : labelFor(ci, (size_t)tgt);
                    std::string fallLabel = (fall == chunk.size())
                        ? endLabelFor(ci)
                        : labelFor(ci, fall);

                    if (tgt <= static_cast<long long>(pc)) {
                        emitLine("call void @zeus_gc_safepoint()");
                    }
                    const int dynTargetExits = entryTryLevelBefore(pc) -
                        (tgt == (long long)chunk.size()
                            ? 0
                            : entryTryLevelBefore((size_t)tgt));
                    const int dynFallthroughExits = entryTryLevelBefore(pc) -
                        (fall == chunk.size() ? 0 : entryTryLevelBefore(fall));
                    std::string edgeBlocks;
                    const std::string takenLabel = entryTryExitEdgeTarget(
                        tgtLabel, dynTargetExits, edgeBlocks);
                    const std::string skipLabel = entryTryExitEdgeTarget(
                        fallLabel, dynFallthroughExits, edgeBlocks);
                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        emitLine("br i1 " + b + ", label " + skipLabel + ", label " + takenLabel);
                    } else {
                        emitLine("br i1 " + b + ", label " + takenLabel + ", label " + skipLabel);
                    }
                    body += edgeBlocks;

                    blkTerminated = true;
                    blkNeedLabel = true;
                    break;
                }

                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO: {
                    const int32_t checkKind =
                        ins.op == VMOperator::COUNT_CHECK_NEG
                            ? ZEUS_RANGE_CHECK_NEGATIVE_END
                            : ZEUS_RANGE_CHECK_ZERO_STEP;
                    const int32_t sourceLine = ins.line > 0 ? ins.line : 0;
                    const std::string checked = freshTmp();
                    emitLine(checked + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_pop(ptr " + checked + ")");
                    emitLine("call void @zeus_range_check_value(ptr " + checked +
                             ", i32 " + std::to_string(checkKind) +
                             ", i32 " + std::to_string(sourceLine) + ")");
                    emitLine("call void @zeus_value_push(ptr " + checked + ")");
                    break;
                }

                case VMOperator::LOAD_NATIVE: {
                    int64_t nid = 0;

                    if (!tryArgInt(ins, 0, nid)) {
                        fail(ins, "[emit-llvm] LOAD_NATIVE id 不是整数");
                        return false;
                    }

                    std::string nativeName;
                    if (resolveNativeHandle(nid, nativeName)) {
                        pendingVariadicCallStack.push_back(std::move(nativeName));
                        break;
                    }

                    fail(ins, "[emit-llvm] v0: 原生函数句柄无有效 ABI 声明");
                    return false;
                }

                case VMOperator::CALL_NATIVE: {
                    int64_t argc = 0;

                    if (!tryArgInt(ins, 0, argc)) {
                        fail(ins, "[emit-llvm] CALL_NATIVE 参数不是整数");
                        return false;
                    }

                    if (pendingVariadicCallStack.empty()) {
                        fail(ins, "[emit-llvm] v0: 原生调用需要动态变长 ABI");
                        return false;
                    }

                    if (argc < 0) {
                        fail(ins, "[emit-llvm] v0: 动态变长调用参数数量无效");
                        return false;
                    }

                    {
                        const std::string nativeName = pendingVariadicCallStack.back();
                        pendingVariadicCallStack.pop_back();
                        if (const auto* nativeDeclaration =
                                findNativeBuiltinDeclaration(nativeName)) {
                            pendingDirectCall = nativeDeclaration->name;
                            pendingFunctionLoad.clear();
                            if (!emitDirectCallFromDynamic(ins, argc)) return false;
                        } else {
                            emitDynamicVariadicIntrinsic(argc, nativeName);
                        }
                    }
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    int64_t argc = 0;

                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            fail(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
                            return false;
                        }
                    }

                    if (!tryArgInt(ins, 0, argc)) {
                        fail(ins, "[emit-llvm] CALL_CLOSURE 参数不是整数");
                        return false;
                    }

                    // Preserve the enclosing variadic call state for nested calls.
                    // The native result is wrapped as ZeusValue and pushed to the dynamic stack.
                    if (!pendingDirectCall.empty()) {
                        if (!emitDirectCallFromDynamic(ins, argc)) return false;
                        break;
                    }

                    if (pendingVariadicCallStack.empty()) {
                        fail(ins, "[emit-llvm] v0: 闭包调用需要动态变长 ABI");
                        return false;
                    }

                    if (argc < 0) {
                        fail(ins, "[emit-llvm] v0: 动态变长调用参数数量无效");
                        return false;
                    }

                    {
                        const std::string nativeName = pendingVariadicCallStack.back();
                        pendingVariadicCallStack.pop_back();
                        emitDynamicVariadicIntrinsic(argc, nativeName);
                    }
                    break;
                }

                case VMOperator::OBJ_CALL_SAFE:
                case VMOperator::OBJ_CALL_METHOD: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        fail(ins, "[emit-llvm] 动态成员调用缺少方法名");
                        return false;
                    }
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 1, argc) || argc < 0 ||
                        argc > std::numeric_limits<int32_t>::max()) {
                        fail(ins, "[emit-llvm] 动态成员调用参数数量无效");
                        return false;
                    }

                    DynamicMemberBridge::RuntimeStackCallRules rules;
                    rules.popValue = [this]() { return emitPop(); };
                    rules.emitLine = [this](const std::string& line) { emitLine(line); };
                    rules.freshTemporary = [this]() { return freshTmp(); };
                    rules.poolString = [this](const std::string& text) {
                        return poolStr(text);
                    };
                    rules.releaseValue = [this](const std::string& value) {
                        emitRelease(value);
                    };
                    rules.pushValue = [this](const std::string& value) {
                        emitPushValue(value);
                    };

                    std::string error;
                    if (!DynamicMemberBridge::emitRuntimeStackCall(
                            std::get<std::string>(ins.args[0]),
                            static_cast<int32_t>(argc),
                            rules,
                            error,
                            ins.op == VMOperator::OBJ_CALL_SAFE)) {
                        fail(ins, "[emit-llvm] " + error);
                        return false;
                    }
                    break;
                }

                default: {
                    fail(ins, "[emit-llvm] v0: 不支持的操作 " + VMO_toString(ins.op));
                    return false;
                }
            }
        }

        activeEntryExceptionTarget = -1;
        if (exitJump) {
            // 出口块：跳到 size / fall through size 的边全部落在这里。
            emitLabelDef(endLabelFor(ci));

            if (!isLast) {
                // 非末 chunk：语义上执行流进入下一 chunk（与解释器按序执行一致）。
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true; // 下一 chunk 首条强制起块
            }
            // 最后一个 chunk：epilogue 的 ret/unreachable 直接成为本块终结。
        }

        return result.error.empty();
    }
