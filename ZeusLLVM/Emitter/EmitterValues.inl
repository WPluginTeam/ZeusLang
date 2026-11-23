// Emitter member definitions: IR line emission, value boxing/unboxing, and direct calls; included inside class Emitter.
    std::string poolStr(const std::string& s) {
        auto it = strPool.find(s);

        if (it != strPool.end()) return it->second;

        std::string name = "@.str." + std::to_string(strCount++);
        strPool[s] = name;
        return name;
    }

    std::string ensureBoundMethodAdapter(const std::string& methodName) {
        const auto existing = boundMethodAdapterSymbols.find(methodName);
        if (existing != boundMethodAdapterSymbols.end()) return existing->second;

        const std::string symbol = "@zeus_bound_method_adapter_" +
            std::to_string(boundMethodAdapterSymbols.size());
        const std::string methodString = poolStr(methodName);
        std::string adapter;
        adapter += "define void " + symbol +
            "(ptr %out, ptr %env, i32 %argc, ptr %args, ptr %argument.refs, ptr %argument.names) {\n";
        adapter += "entry:\n";
        adapter += "  %receiver = call ptr @zeus_closure_env_value_slot(ptr %env, i32 0)\n";
        adapter += "  %method.name = getelementptr inbounds [" +
            std::to_string(methodName.size() + 1) +
            " x i8], ptr " + methodString + ", i64 0, i64 0\n";
        adapter += "  call void @zeus_value_dynamic_member_call(ptr %out, ptr %receiver, ptr %method.name, i32 %argc, ptr %args)\n";
        adapter += "  ret void\n";
        adapter += "}\n\n";
        boundMethodAdapters += adapter;
        boundMethodAdapterSymbols.emplace(methodName, symbol);
        return symbol;
    }

    template <typename EmitLine, typename FreshTemporary, typename BoxValue>
    bool emitBoundMethodClosure(
        const IRType::IRInstr& ins,
        const NativeValue& receiver,
        const FunctionSignature& method,
        NativeValue& closureValue,
        EmitLine&& emitLine,
        FreshTemporary&& freshTemporary,
        BoxValue&& boxValue
    ) {
        const std::string expectedOwnerRef = "&" + method.ownerType;
        if (method.ownerType.empty() || method.isStaticMethod ||
            method.parameterTypes.empty() ||
            (method.parameterTypes.front() != method.ownerType &&
             method.parameterTypes.front() != expectedOwnerRef)) {
            return false;
        }

        const size_t parameterCount = method.parameterTypes.size() - 1;
        if (parameterCount > static_cast<size_t>(
                std::numeric_limits<int32_t>::max())) {
            return false;
        }
        const int32_t minArity = static_cast<int32_t>(
            std::max(0, method.minRequiredParams - 1));
        const bool unboundedArity =
            method.hasVariadicArgs || method.hasVariadicKwargs;
        const int32_t maxArity = unboundedArity
            ? std::numeric_limits<int32_t>::max()
            : static_cast<int32_t>(parameterCount);
        if (minArity > maxArity) return false;

        std::string dynamicReceiver;
        bool ownsDynamicReceiver = false;
        if (!boxValue(receiver, ins, dynamicReceiver, ownsDynamicReceiver)) {
            return false;
        }

        const std::string captureCell = freshTemporary();
        emitLine(captureCell + " = call ptr @zeus_capture_cell_create(ptr " +
                 dynamicReceiver + ")");
        if (ownsDynamicReceiver) {
            emitLine("call void @zeus_value_release(ptr " + dynamicReceiver + ")");
        }

        const std::string captureArray = freshTemporary();
        emitLine(captureArray + " = alloca ptr, i64 1, align 8");
        const std::string captureSlot = freshTemporary();
        emitLine(captureSlot + " = getelementptr inbounds ptr, ptr " +
                 captureArray + ", i64 0");
        emitLine("store ptr " + captureCell + ", ptr " + captureSlot + ", align 8");

        const std::string closure = freshTemporary();
        emitLine(closure + " = alloca %ZeusValue, align 8");
        const std::string adapter =
            ensureBoundMethodAdapter(methodNameForSignature(method));
        if (minArity == maxArity) {
            emitLine("call void @zeus_closure_create(ptr " + closure + ", ptr " +
                     adapter + ", i32 " + std::to_string(maxArity) +
                     ", i32 1, ptr " + captureArray + ")");
        } else {
            emitLine("call void @zeus_closure_create_range(ptr " + closure +
                     ", ptr " + adapter + ", i32 " + std::to_string(minArity) +
                     ", i32 " + std::to_string(maxArity) +
                     ", i32 1, ptr " + captureArray + ")");
        }
        emitLine("call void @zeus_capture_cell_release(ptr " + captureCell + ")");
        if (receiver.ownsClass && isManagedClassValue(receiver)) {
            emitLine("call void @zeus_class_ref_release(ptr " + receiver.value + ")");
        }

        closureValue = NativeValue{
            closure, "ptr", false, true, std::string(), true
        };
        return true;
    }

    void emitLine(const std::string& s) {
        const size_t callAt = s.rfind("call ", 0) == 0 ? 0 : s.find(" = call ");
        const bool isRuntimeCall = callAt != std::string::npos &&
            s.find("@llvm.", callAt) == std::string::npos;
        auto nativeStringOwnerProducer = [&](const std::string& line) {
            if (line.find(" = call ptr @") == std::string::npos) return false;
            const size_t symbolStart = line.find('@');
            const size_t symbolEnd = line.find('(', symbolStart);
            if (symbolStart == std::string::npos || symbolEnd == std::string::npos) {
                return false;
            }
            const std::string emittedSymbol = line.substr(
                symbolStart + 1, symbolEnd - symbolStart - 1);
            std::string symbol = emittedSymbol;
            if (symbol.size() >= 2 && symbol.front() == '"' &&
                symbol.back() == '"') {
                symbol = symbol.substr(1, symbol.size() - 2);
            }
            if (symbol == "zeus_value_to_native_string" ||
                symbol == "zeus_native_string_concat" ||
                symbol == "zeus_string_from_data" ||
                symbol == "zeus_string_from_cstr" ||
                symbol == "zeus_string_copy_from_data" ||
                symbol == "zeus_string_retain" ||
                symbol == "zeus_global_load_string") {
                return true;
            }
            for (const auto& entry : functionSignatures) {
                if ((entry.second.name == symbol ||
                     entry.second.nativeSymbol == symbol ||
                     (!entry.second.nativeSymbol.empty() &&
                      llvmExternalFunctionName(entry.second.nativeSymbol) ==
                          "@" + emittedSymbol)) &&
                    entry.second.returnType == "string") {
                    return true;
                }
            }
            return false;
        };
        auto ownerResult = [&]() {
            if (!nativeStringOwnerProducer(s)) return std::string();
            const size_t assignmentEnd = s.find(" = call ptr ");
            return assignmentEnd == std::string::npos
                ? std::string() : s.substr(0, assignmentEnd);
        };
        const std::string producedOwner = ownerResult();

        if (!isRuntimeCall || isMCUMode()) {
            body += "  " + s + "\n";
        } else if (activeEntryExceptionTarget >= 0) {
            const size_t catchPc = static_cast<size_t>(activeEntryExceptionTarget);
            const auto key = std::make_pair(activeEntryChunk, catchPc);
            auto slot = entryExceptionSlots.find(key);
            if (slot == entryExceptionSlots.end()) {
                const std::string valueSlot = freshTmp();
                body += "  " + valueSlot + " = alloca %ZeusValue, align 8\n";
                // The slot is only filled by zeus_exception_capture on the
                // unwind path. The normal path may still release it (the
                // exit cleanup trusts nativeLocals, which keeps a stale
                // catch-var entry when the catch block ends with `throw`
                // and has no CATCH_END to retire it). Zero it so such a
                // release is a no-op instead of reading garbage flags.
                body += "  call void @zeus_value_init_nil(ptr " + valueSlot + ")\n";
                slot = entryExceptionSlots.emplace(key, valueSlot).first;
            }
            const std::string stem = std::to_string(entryExceptionPads.size());
            const EntryExceptionPad pad{
                "%entry_eh_switch_" + stem, "%entry_eh_pad_" + stem,
                "%entry_eh_resume_" + stem, slot->second,
                activeEntryChunk, catchPc
            };
            EntryExceptionPad catchPad = pad;
            const auto baseline = entryNativeStringTryBaselines.find(key);
            for (const auto& owner : entryNativeStringOwners) {
                const auto generation = entryNativeStringOwnerGenerations.find(owner);
                if (baseline == entryNativeStringTryBaselines.end() ||
                    generation == entryNativeStringOwnerGenerations.end() ||
                    generation->second > baseline->second) {
                    catchPad.cleanupCalls.push_back(
                        "call void @zeus_string_release(ptr " + owner + ")");
                }
            }
            entryExceptionPads.push_back(std::move(catchPad));
            const std::string continuation = "%entry_eh_continue_" + stem;
            std::string invoke = s;
            invoke.replace(callAt == 0 ? 0 : callAt + 3, 4, "invoke");
            body += "  " + invoke + " to label " + continuation +
                " unwind label " + pad.switchLabel + "\n";
            body += continuation.substr(1) + ":\n";
        } else {
            std::vector<std::string> cleanupCalls = entryExceptionCleanupCalls();
            if (cleanupCalls.empty()) {
                body += "  " + s + "\n";
            } else {
                const std::string stem = std::to_string(entryExceptionPads.size());
                EntryExceptionPad pad;
                pad.switchLabel = "%entry_eh_switch_" + stem;
                pad.padLabel = "%entry_eh_pad_" + stem;
                pad.resumeLabel = "%entry_eh_resume_" + stem;
                pad.cleanupOnly = true;
                pad.cleanupCalls = std::move(cleanupCalls);
                entryExceptionPads.push_back(std::move(pad));
                const std::string continuation = "%entry_eh_continue_" + stem;
                std::string invoke = s;
                invoke.replace(callAt == 0 ? 0 : callAt + 3, 4, "invoke");
                body += "  " + invoke + " to label " + continuation +
                    " unwind label %entry_eh_switch_" + stem + "\n";
                body += continuation.substr(1) + ":\n";
            }
        }

        if (!producedOwner.empty() &&
            std::find(entryNativeStringOwners.begin(), entryNativeStringOwners.end(),
                      producedOwner) == entryNativeStringOwners.end()) {
            entryNativeStringOwners.push_back(producedOwner);
            entryNativeStringOwnerGenerations[producedOwner] =
                ++entryNativeStringOwnerGeneration;
        }
        if (s.rfind("call void @zeus_string_release(ptr ", 0) == 0) {
            const size_t begin = std::string("call void @zeus_string_release(ptr ").size();
            const size_t end = s.find(')', begin);
            if (end != std::string::npos) {
                const std::string released = s.substr(begin, end - begin);
                entryNativeStringOwners.erase(std::remove(
                    entryNativeStringOwners.begin(), entryNativeStringOwners.end(),
                    released), entryNativeStringOwners.end());
                entryNativeStringOwnerGenerations.erase(released);
            }
        } else if (s.rfind("store ptr ", 0) == 0) {
            const size_t begin = std::string("store ptr ").size();
            const size_t end = s.find(", ptr ", begin);
            if (end != std::string::npos) {
                const std::string transferred = s.substr(begin, end - begin);
                entryNativeStringOwners.erase(std::remove(
                    entryNativeStringOwners.begin(), entryNativeStringOwners.end(),
                    transferred), entryNativeStringOwners.end());
                entryNativeStringOwnerGenerations.erase(transferred);
            }
        }
        return;
    }

    // 新 label 定义（含 % 前缀传入，如 "%L1_10"）。
    // 铁律：label 之前必须有终结指令；若当前块未终结（fallthrough 位置），
    // 先补一条 br 跳过去（语义等价于 fallthrough，clang 实测只认这种形状）。
    void emitLabelDef(const std::string& pct) {
        if (!blkTerminated) {
            emitLine("br label " + pct);
        }

        body += pct.substr(1) + ":\n";
        blkTerminated = false;
        blkNeedLabel = false;
    }

    void fail(const IRType::IRInstr& ins, const std::string& msg) {
        result.ok = false;
        result.error = msg + " (" + ins.toString() + ")";
        result.errorLine = ins.line;
    }

    // PUSH/LOAD-字面量 的值部分（不含 push）：返回 box 的 SSA 名，失败返回 ""。
    std::string emitDynamicValueForLiteral(const LexerType::Value& v, const IRType::IRInstr& ins) {
        if (std::holds_alternative<std::string>(v.data)) {
            const std::string& s = std::get<std::string>(v.data);

            std::string sym = poolStr(s);
            std::string t = freshTmp();
            emitLine(t + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_string_data(ptr " + t +
                     ", ptr " + sym + ", i64 " + std::to_string(s.size()) + ")");
            return t;
        }

        if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
            const auto& integer = std::get<ZeusType::BigInteger>(v.data);
            int64_t integer64 = 0;
            if (integer.tryToInt64(integer64)) {
                return emitScalarAsValue("i64", std::to_string(integer64));
            }

            const std::string symbol = poolStr(integer.toString());
            std::string dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_bigint_decimal(ptr " +
                     dynamicValue + ", ptr " + symbol + ")");
            return dynamicValue;
        }

        if (std::holds_alternative<ZeusType::BigDecimal>(v.data)) {
            const std::string symbol = poolStr(std::get<ZeusType::BigDecimal>(v.data).toString());
            std::string dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_bigdecimal_decimal(ptr " +
                     dynamicValue + ", ptr " + symbol + ")");
            return dynamicValue;
        }

        if (std::holds_alternative<bool>(v.data)) {
            return emitScalarAsValue("i1", std::get<bool>(v.data) ? "true" : "false");
        }

        fail(ins, "[emit-llvm] v0: 该字面量类型暂不支持（仅 int/string/nil）");
        return "";
    }

    std::string emitDynamicValueForNull(const IRType::IRInstr& ins) {
        std::string value = freshTmp();
        emitLine(value + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_init_nil(ptr " + value + ")");
        (void)ins;
        return value;
    }

    std::string emitDynamicValueForBytes(
        const ZeusType::Bytes& bytes,
        const IRType::IRInstr& ins
    ) {
        const std::string contents(bytes.data.begin(), bytes.data.end());
        const std::string symbol = poolStr(contents);
        const std::string dynamicValue = freshTmp();
        emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_from_bytes(ptr " + dynamicValue +
                 ", ptr " + symbol + ", i64 " +
                 std::to_string(bytes.data.size()) + ")");
        (void)ins;
        return dynamicValue;
    }

    void emitPushValue(const std::string& valueTmp) {
        emitLine("call void @zeus_value_push(ptr " + valueTmp + ")");
    }

    std::string emitPop() {
        std::string t = freshTmp();
        emitLine(t + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_pop(ptr " + t + ")");
        return t;
    }

    void emitRelease(const std::string& valueTmp) {
        emitLine("call void @zeus_value_release(ptr " + valueTmp + ")");
    }

    template <typename EmitLine, typename FreshTemporary>
    std::string emitDynamicVariadicCall(
        const std::string& callSymbol,
        const std::string& nativeName,
        const std::vector<std::string>& values,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary
    ) {
        if (callSymbol.empty()) return {};

        std::string argumentArray = "null";
        if (!values.empty()) {
            argumentArray = freshTemporary();
            emit(argumentArray + " = alloca ptr, i64 " +
                 std::to_string(values.size()) + ", align 8");
            for (size_t i = 0; i < values.size(); ++i) {
                const std::string slot = freshTemporary();
                emit(slot + " = getelementptr inbounds ptr, ptr " + argumentArray +
                     ", i64 " + std::to_string(i));
                emit("store ptr " + values[i] + ", ptr " + slot + ", align 8");
            }
        }

        const std::string result = freshTemporary();
        emit(result + " = alloca %ZeusValue, align 8");
        if (callSymbol == "@zeus_value_native_call") {
            const std::string nameSymbol = poolStr(nativeName);
            const std::string namePointer = freshTemporary();
            emit(namePointer + " = getelementptr inbounds [" +
                 std::to_string(nativeName.size() + 1) + " x i8], ptr " +
                 nameSymbol + ", i64 0, i64 0");
            emit("call void " + callSymbol + "(ptr " + result + ", ptr " +
                 namePointer + ", i32 " + std::to_string(values.size()) +
                 ", ptr " + argumentArray + ")");
        } else {
            emit("call void " + callSymbol + "(ptr " + result + ", i32 " +
                 std::to_string(values.size()) + ", ptr " + argumentArray + ")");
        }
        return result;
    }

    void emitBoxedGlobalIntegerCheck(int index, const std::string& value) {
        if (nativeGlobalDeclaredDoubleTypes.count(index)) {
            const auto declaration = nativeGlobalDeclaredDoubleTypes.find(index);
            const bool isFloat = declaration != nativeGlobalDeclaredDoubleTypes.end() &&
                ZeusBuiltins::nativeFloatingPointBits(declaration->second) == 32;
            const auto converted = emitNativeFloatingValueConversion(value, "ptr", {},
                isFloat ? "float" : "double",
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); });
            emitRelease(value);
            if (isFloat) {
                emitLine("call void @zeus_value_init_float(ptr " + value + ", float " + converted + ")");
            } else {
                emitLine("call void @zeus_value_init_double(ptr " + value + ", double " + converted + ")");
            }
            return;
        }
        const auto declaration = nativeGlobalDeclaredIntegerTypes.find(index);
        if (declaration == nativeGlobalDeclaredIntegerTypes.end()) return;

        if (const auto* abi = ZeusBuiltins::findNativeSignedIntegerABI(declaration->second)) {
            emitLine("call i64 @zeus_value_require_signed_integer(ptr " + value +
                     ", i32 " + std::to_string(abi->integerBits) + ")");
        } else if (const auto* abi =
                       ZeusBuiltins::findNativeUnsignedIntegerABI(declaration->second)) {
            emitLine("call i64 @zeus_value_require_unsigned_integer(ptr " + value +
                     ", i32 " + std::to_string(abi->integerBits) + ")");
        }
    }

    std::string emitScalarAsValue(const std::string& type, const std::string& value,
                                  const std::string& declaredType = {}) {
        if (ZeusBuiltins::findNativeUnsignedIntegerABI(declaredType)) {
            std::string wide = value;
            if (type != "i64") {
                wide = freshTmp();
                emitLine(wide + " = zext " + type + " " + value + " to i64");
            }
            const std::string boxed = freshTmp();
            emitLine(boxed + " = alloca %ZeusValue, align 8");
            const auto* abi = ZeusBuiltins::findNativeUnsignedIntegerABI(declaredType);
            emitLine("call void @zeus_value_init_unsigned_integer(ptr " + boxed +
                     ", i64 " + wide + ", i32 " + std::to_string(abi->integerBits) + ")");
            return boxed;
        }
        if (isIntegerLLVMType(type) && type != "i64") {
            const auto wide = emitNativeIntegerValueConversion(value, type, declaredType,
                "i64", "int",
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); });
            return emitScalarAsValue("i64", wide);
        }
        if (type == "float") {
            const std::string boxed = freshTmp();
            emitLine(boxed + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_float(ptr " + boxed + ", float " + value + ")");
            return boxed;
        }
        if (type == "double") {
            const std::string boxed = freshTmp();
            emitLine(boxed + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_double(ptr " + boxed + ", double " + value + ")");
            return boxed;
        }
        if (type != "i64" && type != "i1") return "";

        std::string dynamicValue = freshTmp();
        emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
        if (type == "i64") {
            emitLine("call void @zeus_value_init_i64(ptr " + dynamicValue +
                     ", i64 " + value + ")");
        } else {
            std::string raw = freshTmp();
            emitLine(raw + " = zext i1 " + value + " to i32");
            emitLine("call void @zeus_value_init_bool(ptr " + dynamicValue +
                     ", i32 " + raw + ")");
        }
        return dynamicValue;
    }

    std::string emitValueAsScalar(
        const std::string& dynamicValue,
        const std::string& type,
        bool releaseDynamicValue = true,
        const FunctionSignature* argumentSignature = nullptr,
        size_t parameterIndex = 0,
        const std::string& declaredType = std::string()
    ) {
        if (isIntegerLLVMType(type) && (type != "i64" ||
            ZeusBuiltins::findNativeIntegerABI(declaredType))) {
            const auto converted = emitNativeIntegerValueConversion(
                dynamicValue, "ptr", {}, type, declaredType,
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); });
            if (releaseDynamicValue) emitRelease(dynamicValue);
            return converted;
        }
        if (type == "float") {
            const auto converted = emitNativeFloatingValueConversion(
                dynamicValue, "ptr", {}, "float",
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); });
            if (releaseDynamicValue) emitRelease(dynamicValue);
            return converted;
        }
        if (type == "double") {
            const auto converted = emitDoubleConversion(dynamicValue, "ptr",
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); });
            if (releaseDynamicValue) emitRelease(dynamicValue);
            return converted;
        }
        if (type != "i64" && type != "i1") return "";

        std::string nativeValue;
        if (type == "i64") {
            nativeValue = freshTmp();
            if (argumentSignature && parameterIndex <
                    argumentSignature->parameterTypes.size()) {
                const size_t receiverOffset =
                    !argumentSignature->ownerType.empty() &&
                    !argumentSignature->isStaticMethod ? 1 : 0;
                const size_t userArgumentIndex = parameterIndex > receiverOffset
                    ? parameterIndex - receiverOffset
                    : 0;
                emitLine(nativeValue +
                    " = call i64 @zeus_value_require_argument_i64(ptr " +
                    dynamicValue + ", ptr " + poolStr(argumentSignature->name) +
                    ", i32 " + std::to_string(userArgumentIndex) + ", ptr " +
                    poolStr(declaredType) + ")");
            } else {
                emitLine(nativeValue + " = call i64 @zeus_value_require_i64(ptr " +
                         dynamicValue + ")");
            }
        } else {
            std::string raw = freshTmp();
            if (argumentSignature && parameterIndex <
                    argumentSignature->parameterTypes.size()) {
                const size_t receiverOffset =
                    !argumentSignature->ownerType.empty() &&
                    !argumentSignature->isStaticMethod ? 1 : 0;
                const size_t userArgumentIndex = parameterIndex > receiverOffset
                    ? parameterIndex - receiverOffset
                    : 0;
                emitLine(raw +
                    " = call i32 @zeus_value_require_argument_bool(ptr " +
                    dynamicValue + ", ptr " + poolStr(argumentSignature->name) +
                    ", i32 " + std::to_string(userArgumentIndex) + ", ptr " +
                    poolStr(declaredType) + ")");
            } else {
                emitLine(raw + " = call i32 @zeus_value_require_bool(ptr " +
                         dynamicValue + ")");
            }
            nativeValue = freshTmp();
            emitLine(nativeValue + " = icmp ne i32 " + raw + ", 0");
        }
        if (releaseDynamicValue) emitRelease(dynamicValue);
        return nativeValue;
    }

    template <typename EmitLine>
    void emitDynamicArgumentTypeCheck(
        const std::string& dynamicValue,
        const FunctionSignature& signature,
        size_t parameterIndex,
        EmitLine&& emit
    ) {
        if (dynamicValue.empty() || parameterIndex >= signature.parameterTypes.size() ||
            signature.parameterTypes[parameterIndex].empty()) {
            return;
        }
        const size_t receiverOffset =
            !signature.ownerType.empty() && !signature.isStaticMethod ? 1 : 0;
        const size_t userArgumentIndex = parameterIndex > receiverOffset
            ? parameterIndex - receiverOffset
            : 0;
        emit("call void @zeus_value_validate_argument_type(ptr " + dynamicValue +
             ", ptr " + poolStr(signature.name) + ", i32 " +
             std::to_string(userArgumentIndex) + ", ptr " +
             poolStr(signature.parameterTypes[parameterIndex]) + ")");
    }

    template <typename EmitLine>
    void emitReferenceArgumentTypeCheck(
        const std::string& slot,
        const FunctionSignature& signature,
        size_t parameterIndex,
        EmitLine&& emit
    ) {
        if (slot.empty() || parameterIndex >= signature.parameterTypes.size() ||
            signature.parameterTypes[parameterIndex].empty()) {
            return;
        }
        const size_t receiverOffset =
            !signature.ownerType.empty() && !signature.isStaticMethod ? 1 : 0;
        const size_t userArgumentIndex = parameterIndex > receiverOffset
            ? parameterIndex - receiverOffset
            : 0;
        emit("call void @zeus_value_slot_validate_argument_type(ptr " + slot +
             ", ptr " + slot + ", ptr " + poolStr(signature.name) + ", i32 " +
             std::to_string(userArgumentIndex) + ", ptr " +
             poolStr(signature.parameterTypes[parameterIndex]) + ")");
    }

    // Global/local slot access has to be emitted into whichever function body
    // is currently being built. The legacy top-level path uses emitLine, while a
    // native function must write into its own functionText: leaking the load
    // into the entry block would leave the use without a dominating definition.
    template <typename EmitLine>
    std::string emitSlotLoad(int index, bool global, EmitLine&& emit) {
        std::string value = freshTmp();
        emit(value + " = alloca %ZeusValue, align 8");
        emit(std::string("call void @zeus_value_") + (global ? "global" : "local") +
             "_load(ptr " + value + ", i32 " + std::to_string(index) + ")");
        return value;
    }

    // Reads a global that the AST walker did not describe, straight out of the
    // runtime slot table, and returns it in whatever representation the rest of
    // the native path expects: an unboxed scalar, a native class pointer, or a
    // boxed ZeusValue.
    bool boxNativeGlobalValue(const IRType::IRInstr& ins, int index, NativeValue& out) {
        const auto classType = prescanGlobalClassTypes.find(index);
        if (classType != prescanGlobalClassTypes.end() &&
            findClassLayout(classType->second) != nullptr) {
            const std::string boxed = emitGlobalValueLoad(index);
            const std::string unboxed = freshTmp();
            emitLine(unboxed + " = call ptr @zeus_value_require_class_ptr(ptr " +
                     boxed + ", ptr " + poolStr(classType->second) + ")");
            emitLine("call void @zeus_class_ref_retain(ptr " + unboxed + ")");
            emitLine("call void @zeus_value_release(ptr " + boxed + ")");
            out = NativeValue{ unboxed, "ptr", false, false, classType->second };
            out.ownsClass = true;
            return true;
        }

        const auto scalarType = nativeGlobalScalarTypes.find(index);
        if (scalarType != nativeGlobalScalarTypes.end()) {
            const bool isBool = scalarType->second == "i1";
            const std::string loaded = freshTmp();
            emitLine(loaded + " = call " + std::string(isBool ? "i32" : "i64") +
                     " @zeus_global_load_" + (isBool ? "bool" : "i64") +
                     "(i32 " + std::to_string(index) + ")");
            if (isBool) {
                const std::string flag = freshTmp();
                emitLine(flag + " = icmp ne i32 " + loaded + ", 0");
                out = NativeValue{ flag, "i1", false, false };
            } else {
                out = NativeValue{ loaded, "i64", false, false };
            }
            return true;
        }

        const auto nativeViewType = prescanGlobalNativeViewTypes.find(index);
        if (nativeViewType != prescanGlobalNativeViewTypes.end()) {
            if (nativeViewType->second == "string") {
                const std::string loaded = freshTmp();
                emitLine(loaded + " = call ptr @zeus_global_load_string(i32 " +
                         std::to_string(index) + ")");
                out = NativeValue{loaded, "ptr", false, false, "string"};
                out.ownsNativeString = true;
                return true;
            }
            if (nativeViewType->second == "bytes") {
                const std::string loaded = freshTmp();
                emitLine(loaded + " = call ptr @zeus_global_load_bytes(i32 " +
                         std::to_string(index) + ")");
                out = NativeValue{loaded, "ptr", false, false, "bytes"};
                out.ownsNativeBytes = true;
                return true;
            }
            out = NativeValue{
                emitGlobalValueLoad(index), "ptr", false, true,
                nativeViewType->second, true
            };
            return true;
        }

        out = NativeValue{
            emitGlobalValueLoad(index), "ptr", false, true, std::string(), true
        };
        return true;
    }

    std::string emitGlobalValueLoad(int index) {
        return emitSlotLoad(index, true,
            [this](const std::string& line) { emitLine(line); });
    }

    std::string emitLocalValueLoad(int index) {
        return emitSlotLoad(index, false,
            [this](const std::string& line) { emitLine(line); });
    }

    void emitPushNull() {
        std::string t = freshTmp();
        emitLine(t + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_init_nil(ptr " + t + ")");
        emitPushValue(t);
    }

    // Invoke a dynamic variadic function with values popped from the VM ABI stack.
    void emitDynamicVariadicIntrinsic(int64_t argc, const std::string& name) {
        std::vector<std::string> values(static_cast<size_t>(argc));
        for (int64_t i = argc - 1; i >= 0; --i) {
            values[static_cast<size_t>(i)] = emitPop();
        }
        const std::string result = emitDynamicVariadicCall(
            dynamicVariadicABISymbol(name),
            name,
            values,
            [this](const std::string& line) { emitLine(line); },
            [this]() { return freshTmp(); }
        );
        for (const auto& value : values) emitRelease(value);
        emitPushValue(result);
        pendingVariadicCall.clear();
    }

    // Convert dynamic ABI arguments to native scalars, then wrap the result.
    bool emitDirectCallFromDynamic(const IRType::IRInstr& ins, int64_t argc) {
        if (pendingDirectCall.empty()) {
            fail(ins, "[emit-llvm] 直接调用缺少目标函数标注");
            return false;
        }

        auto function = functionSignatures.find(pendingDirectCall);
        if (function == functionSignatures.end()) {
            fail(ins, "[emit-llvm] 找不到直接调用目标'" + pendingDirectCall + "'");
            return false;
        }

        const auto& called = function->second;
        if (called.captureCount != 0) {
            fail(ins, "[emit-llvm] 直接调用目标包含捕获变量，不能走原生 ABI");
            return false;
        }

        if (argc < called.minRequiredParams ||
            argc > static_cast<int64_t>(called.parameterTypes.size())) {
            fail(ins, "[emit-llvm] 原生函数调用参数数量不匹配");
            return false;
        }

        // The source-level load names the generic template; AOT dispatch uses
        // its concrete clone. Accept that pairing only for generated clone names.
        const bool specializedTemplateCall = pendingDirectCall.rfind(
            pendingFunctionLoad + "$__zeus_type_", 0) == 0;
        if (!pendingFunctionLoad.empty() && pendingFunctionLoad != pendingDirectCall &&
            !specializedTemplateCall) {
            fail(ins, "[emit-llvm] 函数引用与直接调用标注不一致: loaded='" +
                pendingFunctionLoad + "', direct='" + pendingDirectCall +
                "', tag='" + ins.metadata.tag + "'");
            return false;
        }

        std::vector<std::string> dynamicArguments(static_cast<size_t>(argc));
        for (int64_t i = argc - 1; i >= 0; --i) {
            dynamicArguments[static_cast<size_t>(i)] = emitPop();
        }

        if (ins.metadata.tag == "static_function_alias") {
            const std::string dynamicCallee = emitPop();
            emitRelease(dynamicCallee);
        }

        std::vector<std::string> nativeArguments;
        nativeArguments.reserve(dynamicArguments.size());
        std::vector<std::string> nativeArgumentTypes;
        std::vector<std::string> nativeBytesArguments;

        for (size_t i = 0; i < called.parameterTypes.size(); ++i) {
            if (i >= dynamicArguments.size()) {
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        called.parameterTypes[i], placeholder,
                        [this](const std::string& line) { emitLine(line); },
                        [this]() { return freshTmp(); })) {
                    fail(ins, "[emit-llvm] optional argument has no native ABI placeholder");
                    return false;
                }
                nativeArguments.push_back(placeholder.value);
                nativeArgumentTypes.push_back(
                    functionParameterABIType(called.parameterTypes[i]));
                continue;
            }
            const std::string argumentType = functionParameterABIType(called.parameterTypes[i]);
            emitDynamicArgumentTypeCheck(
                dynamicArguments[i], called, i,
                [this](const std::string& line) { emitLine(line); });
            if (usesZeusValueABI(called.parameterTypes[i])) {
                nativeArguments.push_back(dynamicArguments[i]);
                nativeArgumentTypes.push_back(argumentType);
                continue;
            }
            if (called.parameterTypes[i] == "&bytes") {
                const std::string storage = freshTmp();
                emitLine(storage + " = alloca %ZeusRawBytesRange, align 8");
                emitLine("call void @zeus_value_require_raw_bytes_range(ptr " +
                         storage + ", ptr " + dynamicArguments[i] + ")");
                const std::string range = freshTmp();
                emitLine(range + " = load %ZeusRawBytesRange, ptr " + storage +
                         ", align 8");
                nativeArguments.push_back(range);
            } else if (isReferenceTypeName(called.parameterTypes[i])) {
                const std::string targetType = referencedTypeName(called.parameterTypes[i]);
                if (targetType == "int" ||
                    targetType == "int64" || targetType == "bool") {
                    const std::string pointer = freshTmp();
                    emitLine(pointer + " = call ptr @zeus_value_require_" +
                             (targetType == "bool" ? "bool" : "i64") +
                             "_ptr(ptr " + dynamicArguments[i] + ")");
                    nativeArguments.push_back(pointer);
                } else if (isClassType(targetType)) {
                    const std::string pointer = freshTmp();
                    const std::string typeName = poolStr(targetType);
                    emitLine(pointer + " = call ptr @zeus_value_require_class_ptr(ptr " +
                             dynamicArguments[i] + ", ptr " + typeName + ")");
                    nativeArguments.push_back(pointer);
                } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                           ZeusBuiltins::isNativePointerABI(abi)) {
                    const std::string pointer = freshTmp();
                    emitLine(pointer + " = call ptr @" +
                             std::string(ZeusBuiltins::nativePointerFromDynamicHelper(abi)) + "(ptr " +
                             dynamicArguments[i] + ")");
                    nativeArguments.push_back(pointer);
                } else {
                    fail(ins, "[emit-llvm] 该引用类型无法从 ZeusValue 解箱: &" + targetType);
                    return false;
                }
            } else if (isIntegerLLVMType(argumentType) || argumentType == "i1" ||
                       argumentType == "float" || argumentType == "double") {
                nativeArguments.push_back(emitValueAsScalar(dynamicArguments[i], argumentType,
                    true, &called, i, called.parameterTypes[i]));
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(
                           called.parameterTypes[i]);
                       ZeusBuiltins::isNativePointerABI(abi)) {
                const std::string nativeView = freshTmp();
                emitLine(nativeView + " = call ptr @" +
                         std::string(ZeusBuiltins::nativePointerFromDynamicHelper(abi)) + "(ptr " +
                         dynamicArguments[i] + ")");
                nativeArguments.push_back(nativeView);
                if (abi->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes) {
                    nativeBytesArguments.push_back(nativeView);
                }
            } else if (isClassType(called.parameterTypes[i])) {
                const std::string typeName = poolStr(called.parameterTypes[i]);
                const std::string classArgument = freshTmp();
                emitLine(classArgument + " = call ptr @zeus_value_require_class_ptr(ptr " +
                         dynamicArguments[i] + ", ptr " + typeName + ")");
                nativeArguments.push_back(classArgument);
            } else {
                fail(ins, "[emit-llvm] ZeusValue 无法转换为原生 class/struct 参数");
                return false;
            }
            nativeArgumentTypes.push_back(argumentType);
        }

        // `@native func` 定义导出的是 C 链接函数：未标注返回类型（IR 里落成
        // "nil"）或 `-> void` 时按 C 的 void 调用，不传 out 槽。调用完成后仍要
        // 压入一个 nil，保持顶层栈机的栈平衡（随后的 POP 会释放它）。
        const bool calledIsVoidReturn = called.isExternal
            ? (called.returnType == "void" || called.returnType == "nil")
            : (nativeExportFunctions.count(called.name) != 0 &&
               (called.returnType.empty() || called.returnType == "nil" ||
                called.returnType == "void"));
        const bool returnsDynamic =
            !calledIsVoidReturn && usesZeusValueABI(called.returnType);
        const std::string returnType = returnsDynamic
            ? "void"
            : (calledIsVoidReturn ? "void" : llvmType(called.returnType));
        const bool returnsClassPointer = !returnsDynamic &&
            !isReferenceTypeName(called.returnType) && isClassType(called.returnType);
        const std::string referenceTargetType = referencedTypeName(called.returnType);
        const auto* returnABI = ZeusBuiltins::findNativeTypeABI(called.returnType);
        const auto* referenceTargetABI = ZeusBuiltins::findNativeTypeABI(referenceTargetType);
        const bool returnsRawBytesRange = called.returnType == "&bytes";
        const bool returnsNativeBytes = !isReferenceTypeName(called.returnType) &&
            returnABI != nullptr && returnABI->valueABI ==
                ZeusBuiltins::NativeValueABI::NativeBytes;
        const bool returnsNativeView = !returnsRawBytesRange &&
            (ZeusBuiltins::isNativePointerABI(returnABI) ||
             ZeusBuiltins::isNativePointerABI(referenceTargetABI));
        const auto* nativeViewReturnABI = ZeusBuiltins::isNativePointerABI(returnABI)
            ? returnABI : referenceTargetABI;
        const bool returnsReferenceClassPointer = isReferenceTypeName(called.returnType) &&
            isClassType(referenceTargetType);
        if (isReferenceTypeName(called.returnType) &&
            !returnsReferenceClassPointer && !returnsRawBytesRange) {
            fail(ins, "->& return requires a managed class value");
            return false;
        }
        if (returnType.empty() || (!returnsDynamic && !calledIsVoidReturn &&
                                   !isIntegerLLVMType(returnType) &&
                                   returnType != "float" &&
                                   returnType != "i1" && !returnsClassPointer &&
                                   !returnsReferenceClassPointer &&
                                     !(returnType == "ptr" && returnsNativeView) &&
                                     !returnsRawBytesRange)) {
            fail(ins, "[emit-llvm] 动态栈调用暂不支持原生 class/struct 返回值");
            return false;
        }

        std::string callResult = freshTmp();
        std::string callText;
        if (returnsDynamic) {
            emitLine(callResult + " = alloca %ZeusValue, align 8");
            callText = "call void " + llvmCallTarget(called.name) + "(ptr " + callResult;
        } else if (calledIsVoidReturn) {
            callText = "call void " + llvmCallTarget(called.name) + "(";
        } else {
            callText = callResult + " = call " + returnType + " " +
                       llvmCallTarget(called.name) + "(";
        }
        for (size_t i = 0; i < nativeArguments.size(); ++i) {
            if (i > 0 || returnsDynamic) callText += ", ";
            callText += nativeArgumentTypes[i] + " " + nativeArguments[i];
        }
        if (called.hasDefaultParameters()) {
            if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
            callText += "i64 " + std::to_string(providedPrefixMask(static_cast<size_t>(argc)));
        }
        callText += ")";
        emitLine(callText);

        if (calledIsVoidReturn) {
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_nil(ptr " + dynamicResult + ")");
            emitPushValue(dynamicResult);
        } else if (returnsDynamic) {
            emitPushValue(callResult);
        } else if (returnsClassPointer || returnsReferenceClassPointer) {
            emitLine("call void @zeus_class_ref_require_managed_live(ptr " +
                     callResult + ")");
            const std::string dynamicResult = freshTmp();
            const std::string typeName = poolStr(
                returnsReferenceClassPointer ? referenceTargetType : called.returnType);
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_class_ptr(ptr " + dynamicResult +
                     ", ptr " + callResult + ", ptr " + typeName + ")");
            emitLine("call void @zeus_class_ref_release(ptr " + callResult + ")");
            emitPushValue(dynamicResult);
        } else if (returnsRawBytesRange) {
            const std::string addressPointer = freshTmp();
            const std::string address = freshTmp();
            const std::string length = freshTmp();
            emitLine(addressPointer + " = extractvalue %ZeusRawBytesRange " +
                     callResult + ", 0");
            emitLine(address + " = ptrtoint ptr " + addressPointer + " to i64");
            emitLine(length + " = extractvalue %ZeusRawBytesRange " +
                     callResult + ", 1");
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_raw_bytes_range(ptr " + dynamicResult +
                     ", i64 " + address + ", i64 " + length + ")");
            emitPushValue(dynamicResult);
        } else if (returnsNativeBytes) {
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_native_bytes(ptr " + dynamicResult +
                     ", ptr " + callResult + ")");
            emitLine("call void @zeus_bytes_release(ptr " + callResult + ")");
            emitPushValue(dynamicResult);
        } else if (returnsNativeView) {
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @" +
                     std::string(ZeusBuiltins::nativePointerToDynamicHelper(nativeViewReturnABI)) +
                     "(ptr " + dynamicResult +
                     ", ptr " + callResult + ")");
            if (nativeViewReturnABI != nullptr && nativeViewReturnABI->valueABI ==
                    ZeusBuiltins::NativeValueABI::NativeString) {
                emitLine("call void @zeus_string_release(ptr " + callResult + ")");
            }
            emitPushValue(dynamicResult);
        } else {
            std::string dynamicResult = emitScalarAsValue(
                returnType, callResult, called.returnType);
            emitPushValue(dynamicResult);
        }
        for (const auto& argument : nativeBytesArguments) {
            emitLine("call void @zeus_bytes_release(ptr " + argument + ")");
        }
        for (const auto& argument : dynamicArguments) emitRelease(argument);

        pendingDirectCall.clear();
        pendingFunctionLoad.clear();
        return true;
    }

    bool emitNativeValueAsDynamic(
        const NativeValue& value,
        const IRType::IRInstr& ins,
        std::string& dynamicValue
    ) {
        if (value.functionRef) {
            std::string error;
            if (!emitFunctionReferenceAsDynamic(
                    value,
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); },
                    dynamicValue,
                    error)) {
                fail(ins, error);
                return false;
            }
            return true;
        }

        if (value.dynamic) {
            dynamicValue = value.value;
            return true;
        }

        if (value.type == "classref") {
            const int32_t classId = classHandleIndex(value.sourceType);
            if (classId < 0) {
                fail(ins, "[emit-llvm] class value has no AOT class layout");
                return false;
            }
            dynamicValue = freshTmp();
            const std::string className = poolStr(value.sourceType);
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_named_class_handle(ptr " + dynamicValue +
                     ", i32 " + std::to_string(classId) + ", ptr " + className + ")");
            return true;
        }

        if (!value.functionRef && value.type == "%ZeusRawBytesRange" &&
            value.sourceType == "&bytes") {
            const std::string address = freshTmp();
            const std::string rawAddress = freshTmp();
            const std::string length = freshTmp();
            dynamicValue = freshTmp();
            emitLine(address + " = extractvalue %ZeusRawBytesRange " +
                     value.value + ", 0");
            emitLine(rawAddress + " = ptrtoint ptr " + address + " to i64");
            emitLine(length + " = extractvalue %ZeusRawBytesRange " +
                     value.value + ", 1");
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_raw_bytes_range(ptr " +
                     dynamicValue + ", i64 " + rawAddress + ", i64 " + length + ")");
            return true;
        }

        if (value.type == "ptr" && isReferenceTypeName(value.sourceType)) {
            const std::string targetType = referencedTypeName(value.sourceType);
            dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            if (targetType == "int" ||
                targetType == "int64" || targetType == "bool") {
                emitLine(std::string("call void @zeus_value_from_") +
                         (targetType == "bool" ? "bool" : "i64") +
                         "_ptr(ptr " + dynamicValue + ", ptr " + value.value + ")");
            } else if (isClassType(targetType)) {
                const std::string typeName = poolStr(targetType);
                emitLine("call void @zeus_value_from_class_ptr(ptr " + dynamicValue +
                         ", ptr " + value.value + ", ptr " + typeName + ")");
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                       ZeusBuiltins::isNativePointerABI(abi)) {
                emitLine("call void @" +
                         std::string(ZeusBuiltins::nativePointerToDynamicHelper(abi)) +
                         "(ptr " + dynamicValue +
                         ", ptr " + value.value + ")");
            } else {
                fail(ins, "[emit-llvm] 该引用类型不能装箱为 ZeusValue: &" + targetType);
                return false;
            }
            return true;
        }

        const auto* sourceABI = ZeusBuiltins::findNativeTypeABI(value.sourceType);
        if (value.type == "ptr" && ZeusBuiltins::isNativePointerABI(sourceABI)) {
            dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @" +
                     std::string(ZeusBuiltins::nativePointerToDynamicHelper(sourceABI)) +
                     "(ptr " + dynamicValue +
                     ", ptr " + value.value + ")");
            return true;
        }

        if (value.type == "ptr" && isClassType(value.sourceType)) {
            dynamicValue = freshTmp();
            const std::string typeName = poolStr(value.sourceType);
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_class_ptr(ptr " + dynamicValue +
                     ", ptr " + value.value + ", ptr " + typeName + ")");
            return true;
        }

        dynamicValue = emitScalarAsValue(value.type, value.value, value.sourceType);
        if (dynamicValue.empty()) {
            fail(ins, "[emit-llvm] 原生值没有可用的动态 ABI 表示");
            return false;
        }
        return true;
    }

    void releaseNativeValue(const NativeValue& value) {
        if (value.dynamic && value.ownsDynamic && !value.value.empty()) {
            emitRelease(value.value);
        } else if (value.ownsClass && !value.value.empty()) {
            emitLine("call void @zeus_class_ref_release(ptr " + value.value + ")");
        } else if (value.ownsNativeBytes && !value.value.empty()) {
            emitLine("call void @zeus_bytes_release(ptr " + value.value + ")");
        } else if (value.ownsNativeString && !value.value.empty()) {
            emitLine("call void @zeus_string_release(ptr " + value.value + ")");
        }
    }

    bool emitNativeValueAsType(
        NativeValue value,
        const std::string& expectedType,
        const IRType::IRInstr& ins,
        std::string& nativeValue,
        const std::string& declaredType = std::string(),
        bool releaseDynamicSource = true,
        const FunctionSignature* argumentSignature = nullptr,
        size_t parameterIndex = 0,
        bool* ownsNativeString = nullptr
    ) {
        if (ownsNativeString != nullptr) *ownsNativeString = false;
        if (value.functionRef) {
            fail(ins, "[emit-llvm] 函数引用不能作为原生值");
            return false;
        }

        const auto* targetABI = ZeusBuiltins::findNativeTypeABI(declaredType);
        if (expectedType == "%ZeusRawBytesRange" && declaredType == "&bytes" &&
            value.dynamic) {
            const std::string storage = freshTmp();
            nativeValue = freshTmp();
            emitLine(storage + " = alloca %ZeusRawBytesRange, align 8");
            emitLine("call void @zeus_value_require_raw_bytes_range(ptr " +
                     storage + ", ptr " + value.value + ")");
            emitLine(nativeValue + " = load %ZeusRawBytesRange, ptr " +
                     storage + ", align 8");
            if (releaseDynamicSource && value.ownsDynamic) {
                emitRelease(value.value);
            }
            return true;
        }
        if (expectedType == "ptr" && targetABI != nullptr &&
            targetABI->valueABI == ZeusBuiltins::NativeValueABI::NativeString &&
            !value.dynamic && value.type == "ptr" &&
            isClassType(value.sourceType)) {
            if (value.sourceType != "String") {
                fail(ins, "[emit-llvm] only String instances can convert to native string");
                return false;
            }
            std::string boxed;
            if (!emitNativeValueAsDynamic(value, ins, boxed)) return false;
            nativeValue = freshTmp();
            emitLine(nativeValue + " = call ptr @zeus_value_to_native_string(ptr " +
                     boxed + ")");
            emitRelease(boxed);
            if (ownsNativeString != nullptr) *ownsNativeString = true;
            return true;
        }

        if (!value.dynamic) {
            if ((expectedType == "double" || expectedType == "float") &&
                value.sourceType != "char") {
                nativeValue = emitNativeFloatingValueConversion(value.value, value.type,
                    value.sourceType, expectedType,
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); });
                if (!nativeValue.empty()) return true;
            }
            if (isIntegerLLVMType(value.type) &&
                isIntegerLLVMType(expectedType)) {
                nativeValue = emitNativeIntegerValueConversion(
                    value.value, value.type, value.sourceType,
                    expectedType, declaredType,
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); });
                return true;
            }
            if (value.type != expectedType ||
                (isClassType(declaredType) && value.sourceType != declaredType)) {
                fail(ins, "[emit-llvm] 原生值类型不匹配");
                return false;
            }

            nativeValue = value.value;
            return true;
        }

        if (expectedType == "ptr" && isClassType(declaredType)) {
            nativeValue = freshTmp();
            const std::string typeName = poolStr(declaredType);
            emitLine(nativeValue + " = call ptr @zeus_value_require_class_ptr(ptr " +
                     value.value + ", ptr " + typeName + ")");
            return true;
        }

        if (expectedType == "ptr" && isReferenceTypeName(declaredType)) {
            const std::string targetType = referencedTypeName(declaredType);
            if (targetType == "int" ||
                targetType == "int64" || targetType == "bool") {
                nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @zeus_value_require_" +
                         (targetType == "bool" ? "bool" : "i64") +
                         "_ptr(ptr " + value.value + ")");
                return true;
            }
            if (isClassType(targetType)) {
                nativeValue = freshTmp();
                const std::string typeName = poolStr(targetType);
                emitLine(nativeValue + " = call ptr @zeus_value_require_class_ptr(ptr " +
                         value.value + ", ptr " + typeName + ")");
                return true;
            }
            const auto* referenceABI = ZeusBuiltins::findNativeTypeABI(targetType);
            if (ZeusBuiltins::isNativePointerABI(referenceABI)) {
                nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @" +
                         std::string(ZeusBuiltins::nativePointerFromDynamicHelper(referenceABI)) +
                         "(ptr " +
                         value.value + ")");
                if (ownsNativeString != nullptr && referenceABI != nullptr &&
                    referenceABI->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                    *ownsNativeString = true;
                }
                return true;
            }
        }

        const auto* declaredABI = ZeusBuiltins::findNativeTypeABI(declaredType);
        if (expectedType == "ptr" && ZeusBuiltins::isNativePointerABI(declaredABI)) {
            nativeValue = freshTmp();
            emitLine(nativeValue + " = call ptr @" +
                     std::string(ZeusBuiltins::nativePointerFromDynamicHelper(declaredABI)) +
                     "(ptr " +
                     value.value + ")");
            if (ownsNativeString != nullptr && declaredABI != nullptr &&
                declaredABI->valueABI == ZeusBuiltins::NativeValueABI::NativeString) {
                *ownsNativeString = true;
            }
            return true;
        }

        nativeValue = emitValueAsScalar(
            value.value, expectedType, releaseDynamicSource,
            argumentSignature, parameterIndex, declaredType);
        if (nativeValue.empty()) {
            fail(ins, "[emit-llvm] 目标函数参数类型不支持原生 int/bool");
            return false;
        }
        return true;
    }

    template <typename EmitLine, typename FreshTemporary,
              typename ConvertArgument, typename Fail>
    bool emitNewOperatorConstruction(
        const IRType::IRInstr& ins,
        const std::string& typeName,
        const std::vector<NativeValue>& arguments,
        NativeValue& result,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        Fail&& failAt
    ) {
        const FunctionSignature* factory = findNewOperatorSignature(typeName);
        if (factory == nullptr) {
            return failAt(ins, "type '" + typeName + "' has no operator[new] factory");
        }
        if (factory->captureCount != 0) {
            return failAt(ins, "operator[new] cannot capture variables");
        }
        if (arguments.size() < static_cast<size_t>(factory->minRequiredParams) ||
            arguments.size() > factory->parameterTypes.size()) {
            return failAt(ins, "operator[new] argument count does not match its signature");
        }

        const bool returnsDynamic = usesZeusValueABI(factory->returnType);
        const std::string returnType = returnsDynamic
            ? "void"
            : llvmType(factory->returnType);
        if (returnType.empty() || (!returnsDynamic && returnType == "void")) {
            return failAt(ins,
                "operator[new] must return a value with a valid LLVM ABI mapping");
        }

        const std::string callResult = freshTemporary();
        std::string callText;
        if (returnsDynamic) {
            emit(callResult + " = alloca %ZeusValue, align 8");
            callText = "call void " + llvmCallTarget(factory->name) + "(ptr " + callResult;
        } else {
            callText = callResult + " = call " + returnType + " " +
                llvmCallTarget(factory->name) + "(";
        }
        bool hasCallArgument = returnsDynamic;

        std::vector<std::string> dynamicArguments;
        std::vector<bool> ownsDynamicArguments;
        std::vector<std::string> nativeBytesArguments;
        std::vector<std::string> nativeStringArguments;
        for (size_t i = 0; i < factory->parameterTypes.size(); ++i) {
            std::string argument;
            bool ownsDynamic = false;
            bool ownsNativeString = false;
            const std::string& declaredType = factory->parameterTypes[i];
            if (i < arguments.size()) {
                if (!convertArgument(
                        arguments[i], declaredType, argument, ownsDynamic,
                        ownsNativeString)) {
                    return false;
                }
            } else {
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        declaredType, placeholder, emit, freshTemporary)) {
                    return failAt(ins,
                        "operator[new] has no ABI placeholder for an omitted argument");
                }
                argument = placeholder.value;
            }

            const std::string parameterABIType =
                functionParameterABIType(declaredType);
            if (parameterABIType.empty()) {
                return failAt(ins,
                    "operator[new] parameter has no LLVM ABI mapping");
            }
            if (hasCallArgument) callText += ", ";
            callText += parameterABIType + " " + argument;
            hasCallArgument = true;
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
        if (factory->hasDefaultParameters()) {
            if (hasCallArgument) callText += ", ";
            callText += "i64 " + std::to_string(providedPrefixMask(arguments.size()));
        }
        callText += ")";
        emit(callText);

        if (!returnsDynamic && isClassType(factory->returnType)) {
            // A native factory hands out a live instance, so the class has
            // live instances even without a static `new` in this program.
            constructedClassNames.insert(factory->returnType);
            emit("call void @zeus_class_ref_register(ptr " + callResult +
                 ", ptr " + classLifetimeFinalizer(factory->returnType) +
                 ", ptr " + poolStr(factory->returnType) + ")");
        }

        std::set<std::string> releasedDynamicArguments;
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

        result = NativeValue{
            callResult,
            returnsDynamic ? "ptr" : returnType,
            false,
            returnsDynamic,
            factory->returnType,
            returnsDynamic
        };
        result.ownsClass = !returnsDynamic && isClassType(factory->returnType);
        result.ownsNativeBytes = !returnsDynamic && factory->returnType == "bytes";
        result.ownsNativeString = !returnsDynamic && factory->returnType == "string";
        return true;
    }

