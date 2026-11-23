// Emitter member definitions: closures, callable resolution, ABI helpers, and metadata collection; included inside class Emitter.
    static std::string closureAdapterSymbol(size_t chunkIndex) {
        return "@zeus_closure_adapter_" + std::to_string(chunkIndex);
    }

    std::vector<ClosureCaptureDescriptor> captureDescriptors(size_t chunkIndex) const {
        std::vector<ClosureCaptureDescriptor> result;
        if (!programChunks || chunkIndex >= programChunks->size()) return result;
        for (const auto& ins : (*programChunks)[chunkIndex]) {
            if (ins.op != VMOperator::CAPTURE_OUTSIDE_VAR || ins.args.size() < 5 ||
                !std::holds_alternative<std::string>(ins.args[0]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[1]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[2]) ||
                !std::holds_alternative<std::string>(ins.args[3]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[4])) {
                continue;
            }
            ClosureCaptureDescriptor capture;
            capture.sourceKind = std::get<std::string>(ins.args[0]);
            capture.sourceIndex = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(ins.args[1]));
            capture.destinationIndex = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(ins.args[2]));
            capture.name = std::get<std::string>(ins.args[3]);
            capture.byReference = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(ins.args[4])) != 0;
            result.push_back(std::move(capture));
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.destinationIndex < rhs.destinationIndex;
        });
        return result;
    }

    bool needsClosureAdapter(size_t chunkIndex) const {
        const auto namespaceFunction = functionSignaturesByChunk.find(chunkIndex);
        if (namespaceFunction != functionSignaturesByChunk.end() &&
            functionValueReferences.find(namespaceFunction->second.name) !=
                functionValueReferences.end()) {
            return true;
        }

        if (!programChunks) return false;
        for (size_t callerChunkIndex = 0;
             callerChunkIndex < programChunks->size();
             ++callerChunkIndex) {
            const auto& chunk = (*programChunks)[callerChunkIndex];
            if (functionSignaturesByChunk.find(callerChunkIndex) ==
                functionSignaturesByChunk.end()) {
                continue;
            }
            for (const auto& ins : chunk) {
                if (ins.op == VMOperator::MAKE_FUNCTION && !ins.args.empty() &&
                    std::holds_alternative<LexerType::Value>(ins.args[0]) &&
                    VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(ins.args[0])) ==
                        static_cast<int>(chunkIndex)) {
                    return true;
                }

                if (ins.op != VMOperator::LOAD_VALUE) continue;

                const FunctionSignature* referencedFunction = nullptr;
                if (ins.args.size() == 1 &&
                    std::holds_alternative<std::string>(ins.args[0])) {
                    referencedFunction = resolveFunctionSignature(
                        std::get<std::string>(ins.args[0]));
                } else if (ins.args.size() >= 2 &&
                           std::holds_alternative<std::string>(ins.args[0]) &&
                           std::get<std::string>(ins.args[0]) == "global" &&
                           std::holds_alternative<LexerType::Value>(ins.args[1])) {
                    const int globalIndex = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(ins.args[1]));
                    const auto global = functionGlobals.find(globalIndex);
                    if (global != functionGlobals.end()) {
                        referencedFunction = resolveFunctionSignature(global->second);
                    }
                }

                if (referencedFunction != nullptr &&
                    !referencedFunction->isExternal &&
                    referencedFunction->captureCount == 0 &&
                    referencedFunction->chunkIndex == chunkIndex) {
                    return true;
                }
            }
        }
        return false;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitFunctionReferenceAsDynamic(
        const NativeValue& value,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        std::string& dynamicValue,
        std::string& error
    ) const {
        if (!value.functionRef || value.type != "function") {
            error = "[emit-llvm] only a Zeus function reference can be boxed as a closure";
            return false;
        }

        const auto function = functionSignatures.find(value.value);
        if (function == functionSignatures.end() || function->second.isExternal ||
            function->second.isGenericTemplate ||
            function->second.captureCount != 0 ||
            functionSignaturesByChunk.find(function->second.chunkIndex) ==
                functionSignaturesByChunk.end() ||
            !needsClosureAdapter(function->second.chunkIndex)) {
            error = "[emit-llvm] function reference has no compatible closure adapter: '" +
                value.value + "'";
            return false;
        }

        const FunctionSignature& signature = function->second;
        dynamicValue = freshTemporary();
        emit(dynamicValue + " = alloca %ZeusValue, align 8");
        const std::string adapter = closureAdapterSymbol(signature.chunkIndex);
        if (signature.hasDefaultParameters() || signature.hasVariadicArgs ||
            signature.hasVariadicKwargs) {
            const int32_t maxArguments = signature.hasVariadicArgs ||
                    signature.hasVariadicKwargs
                ? std::numeric_limits<int32_t>::max()
                : static_cast<int32_t>(signature.parameterTypes.size());
            emit("call void @zeus_closure_create_range(ptr " + dynamicValue +
                 ", ptr " + adapter + ", i32 " +
                 std::to_string(signature.minRequiredParams) + ", i32 " +
                 std::to_string(maxArguments) + ", i32 0, ptr null)");
        } else {
            emit("call void @zeus_closure_create(ptr " + dynamicValue +
                 ", ptr " + adapter + ", i32 " +
                 std::to_string(signature.parameterTypes.size()) +
                 ", i32 0, ptr null)");
        }
        return true;
    }

    std::string freshTmp() {
        return "%t" + std::to_string(tmpCount++);
    }

    static std::string llvmFunctionName(const std::string& name) {
        std::string result = "@zeus_";

        for (unsigned char c : name) {
            if (std::isalnum(c) || c == '_') {
                result += static_cast<char>(c);
            } else {
                result += '_';
            }
        }

        return result;
    }

    static std::string llvmExternalFunctionName(const std::string& name) {
        std::string result = "@\"";
        for (unsigned char c : name) {
            if (std::isalnum(c) || c == '_' || c == '.' || c == '$' || c == '-') {
                result += static_cast<char>(c);
            } else {
                char escaped[5];
                snprintf(escaped, sizeof(escaped), "\\%02X", static_cast<unsigned>(c));
                result += escaped;
            }
        }
        result += '\"';
        return result;
    }

    std::string llvmCallTarget(const FunctionSignature& signature) const {
        if (!signature.nativeLibraryPath.empty()) {
            const auto thunk = dynamicNativeImportThunkSymbols.find(signature.name);
            return thunk != dynamicNativeImportThunkSymbols.end()
                ? thunk->second
                : llvmFunctionName("__zeus_native_import_" + signature.name);
        }
        if (!signature.nativeSymbol.empty()) {
            const std::string fastTarget = fastBytesCallTarget(signature.nativeSymbol);
            if (!fastTarget.empty()) return fastTarget;
            return llvmExternalFunctionName(signature.nativeSymbol);
        }

        std::string target = llvmFunctionName(signature.name);
        const auto count = internalFunctionSymbolCounts.find(target);
        if (count != internalFunctionSymbolCounts.end() && count->second > 1) {
            target += "_chunk" + std::to_string(signature.chunkIndex);
        }
        return target;
    }

    // `@native(addr) decl func f(fn: Int64, ...)` has no C symbol. Each call
    // goes through a private thunk that turns parameter 0 into a function
    // pointer and forwards the remaining arguments with the C ABI.
    bool buildIndirectNativeThunk(
        const FunctionSignature& signature,
        std::string& thunk,
        std::string& error
    ) const {
        const std::string label = "@native(addr) '" + signature.name + "'";
        if (signature.parameterTypes.empty() ||
            (!signature.parameterIsReference.empty() && signature.parameterIsReference[0]) ||
            functionParameterABIType(signature.parameterTypes[0]) != "i64") {
            error = label + " requires an Int64 callee address as its first parameter";
            return false;
        }
        const bool returnsVoid =
            signature.returnType == "void" || signature.returnType == "nil";
        if (!returnsVoid && usesZeusValueABI(signature.returnType)) {
            error = label + " must return a C scalar type or '-> void'";
            return false;
        }
        const std::string returnType =
            returnsVoid ? "void" : llvmType(signature.returnType);
        if (returnType.empty()) {
            error = label + " return type has no LLVM ABI mapping";
            return false;
        }

        std::vector<std::string> parameterTypes;
        for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
            const bool byReference = i < signature.parameterIsReference.size() &&
                signature.parameterIsReference[i];
            const std::string type =
                functionParameterABIType(signature.parameterTypes[i], byReference);
            if (type.empty()) {
                error = label + " parameter type has no LLVM ABI mapping";
                return false;
            }
            parameterTypes.push_back(type);
        }

        thunk += "define internal " + returnType + " " + llvmCallTarget(signature) + "(";
        for (size_t i = 0; i < parameterTypes.size(); ++i) {
            if (i > 0) thunk += ", ";
            thunk += parameterTypes[i] + " %arg" + std::to_string(i);
        }
        thunk += ") {\nentry:\n  %callee = inttoptr i64 %arg0 to ptr\n  ";
        if (!returnsVoid) {
            thunk += "%result = call ";
            if (signature.returnType == "bool") thunk += "zeroext ";
        } else {
            thunk += "call ";
        }
        thunk += returnType + " %callee(";
        for (size_t i = 1; i < parameterTypes.size(); ++i) {
            if (i > 1) thunk += ", ";
            thunk += parameterTypes[i];
            if (signature.parameterTypes[i] == "bool") thunk += " zeroext";
            thunk += " %arg" + std::to_string(i);
        }
        thunk += ")\n";
        thunk += returnsVoid ? "  ret void\n" : "  ret " + returnType + " %result\n";
        thunk += "}\n\n";
        return true;
    }

    void assignDynamicNativeImportThunkSymbols() {
        dynamicNativeImportThunkSymbols.clear();
        std::set<std::string> occupiedSymbols;
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (!signature.nativeLibraryPath.empty()) continue;

            if (!signature.nativeSymbol.empty()) {
                occupiedSymbols.insert(llvmExternalFunctionName(signature.nativeSymbol));
            } else if (signature.isExternal) {
                occupiedSymbols.insert(llvmFunctionName(signature.name));
            } else {
                occupiedSymbols.insert(llvmCallTarget(signature));
            }
        }

        // functionSignatures is ordered by source name, so conflict resolution
        // is deterministic across runs. Reserve all non-thunk symbols first.
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (signature.nativeLibraryPath.empty()) continue;

            const std::string base = llvmFunctionName(
                "__zeus_native_import_" + signature.name);
            std::string symbol = base;
            size_t suffix = 0;
            while (occupiedSymbols.find(symbol) != occupiedSymbols.end()) {
                symbol = base + "_thunk" + std::to_string(++suffix);
            }
            dynamicNativeImportThunkSymbols.emplace(signature.name, symbol);
            occupiedSymbols.insert(std::move(symbol));
        }
    }

    std::string llvmCallTarget(const std::string& name) const {
        const auto signature = functionSignatures.find(name);
        if (signature != functionSignatures.end()) return llvmCallTarget(signature->second);
        return llvmFunctionName(name);
    }

    const FunctionSignature* findNativeBuiltinDeclaration(
        const std::string& builtinName
    ) const {
        const FunctionSignature* match = nullptr;
        for (const auto& entry : functionSignatures) {
            const auto& signature = entry.second;
            if (!signature.isExternal || signature.nativeSymbol.empty()) continue;

            const std::string suffix = "." + builtinName;
            const bool sameName = signature.name == builtinName;
            const bool qualifiedName = signature.name.size() > suffix.size() &&
                signature.name.compare(
                    signature.name.size() - suffix.size(), suffix.size(), suffix
                ) == 0;
            if (!sameName && !qualifiedName) continue;

            if (match != nullptr &&
                (match->nativeSymbol != signature.nativeSymbol ||
                 match->nativeLibraryPath != signature.nativeLibraryPath)) {
                return nullptr;
            }
            match = &signature;
        }
        return match;
    }

    bool resolveNativeHandle(int64_t encodedHandle, std::string& name) const {
        if (encodedHandle < 0) {
            if (encodedHandle < std::numeric_limits<int>::min()) return false;
            const auto* injection = ZeusBuiltins::nativeInjectionFromHandle(
                static_cast<int>(encodedHandle));
            if (injection) {
                name = std::string(injection->declarationName);
                return findNativeBuiltinDeclaration(name) != nullptr;
            }
            const auto* dynamicImport = ZeusBuiltins::dynamicNativeImportFromHandle(
                static_cast<int>(encodedHandle));
            if (!dynamicImport) return false;
            name = dynamicImport->declarationName;
            return findNativeBuiltinDeclaration(name) != nullptr;
        }

        if (encodedHandle > std::numeric_limits<int>::max()) return false;
        const auto* builtin = ZeusBuiltins::get(static_cast<int>(encodedHandle));
        if (!builtin) return false;
        name = builtin->name;
        return !dynamicVariadicABISymbol(name).empty() ||
            findNativeBuiltinDeclaration(name) != nullptr;
    }

    // A bytes value that reaches a native function has to stay alive for the
    // duration of the call, so the emitter hands over an owned copy of the
    // buffer. The read only builtins never retain the pointer, so for them
    // the copy is unnecessary and the buffer can be borrowed instead.
    //
    // The whitelist is on the native symbol rather than on the Zeus name,
    // because a program may bind its own declaration to a symbol.
    static bool isReadOnlyBytesNativeSymbol(const std::string& symbol) {
        return symbol == "zeus_bytes_get" || symbol == "zeus_bytes_len";
    }

    // Only an instruction that pushes exactly one value may separate the
    // load from the call. Anything else could consume the loaded value.
    static bool isSingleValuePush(VMOperator op) {
        return op == VMOperator::PUSH || op == VMOperator::PUSH_NAMED ||
            op == VMOperator::LOAD_VALUE || op == VMOperator::OBJ_LOAD_ATTR;
    }

    // Reports whether the bytes value loaded at pc is the first argument of a
    // call to a read only bytes builtin. Tracks net operand stack depth after
    // the loaded bytes value so arithmetic index expressions (such as idx + 1)
    // still preserve the borrow without consuming the bytes value itself.
    bool isBorrowedBytesArgument(
        const std::vector<IRType::IRInstr>& chunk,
        size_t pc
    ) const {
        size_t stackDepthAfterBytes = 0;
        size_t callPc = 0;
        bool found = false;
        // The value is often stored into a temporary local first and read
        // back from there, so the scan follows that detour.
        int64_t temporaryLocal = -1;
        for (size_t next = pc + 1; next < chunk.size(); ++next) {
            const auto& instruction = chunk[next];
            if (instruction.op == VMOperator::IR_ANNOTATION) continue;
            if (stackDepthAfterBytes == 0 &&
                instruction.op == VMOperator::STORE_VALUE_LOCAL &&
                temporaryLocal < 0) {
                int64_t index = -1;
                if (!tryArgInt(instruction, 1, index) || index < 0) return false;
                temporaryLocal = index;
                continue;
            }
            if (stackDepthAfterBytes == 0 &&
                instruction.op == VMOperator::LOAD_VALUE && temporaryLocal >= 0) {
                int64_t index = -1;
                if (!tryArgInt(instruction, 1, index) || index != temporaryLocal) {
                    return false;
                }
                temporaryLocal = -1;
                continue;
            }
            if (temporaryLocal >= 0) return false;
            if (isSingleValuePush(instruction.op)) {
                if (++stackDepthAfterBytes > 16) return false;
                continue;
            }
            if (instruction.op == VMOperator::ADD ||
                instruction.op == VMOperator::SUBTRACT ||
                instruction.op == VMOperator::MULTIPLY ||
                instruction.op == VMOperator::DIVIDE ||
                instruction.op == VMOperator::MODULUS) {
                if (stackDepthAfterBytes < 2) return false;
                --stackDepthAfterBytes;
                continue;
            }
            if (instruction.op != VMOperator::CALL_NATIVE) return false;
            callPc = next;
            found = true;
            break;
        }
        if (!found || temporaryLocal >= 0) return false;

        int64_t argc = 0;
        if (!tryArgInt(chunk[callPc], 0, argc) || argc < 1) return false;
        // The loaded value is the first argument, so the call pops one more
        // value than the remaining operand depth above it.
        if (static_cast<size_t>(argc) != stackDepthAfterBytes + 1) return false;

        // The callee is pushed before the arguments, so the native load sits
        // right before the argument expressions. Walking back from the load
        // therefore finds the callee, and only simple instructions may
        // separate the two.
        for (size_t back = pc; back > 0; --back) {
            const auto& instruction = chunk[back - 1];
            if (instruction.op == VMOperator::IR_ANNOTATION) continue;
            if (isSingleValuePush(instruction.op)) continue;
            if (instruction.op != VMOperator::LOAD_NATIVE) return false;
            int64_t handle = 0;
            std::string nativeName;
            if (!tryArgInt(instruction, 0, handle)) return false;
            if (!resolveNativeHandle(handle, nativeName)) return false;
            const auto* declaration = findNativeBuiltinDeclaration(nativeName);
            if (declaration == nullptr) return false;
            if (!isReadOnlyBytesNativeSymbol(declaration->nativeSymbol)) return false;
            if (declaration->parameterTypes.empty() ||
                declaration->parameterTypes.front() != "bytes") {
                return false;
            }
            return true;
        }
        return false;
    }

    const FunctionSignature* resolveFunctionSignature(
        const std::string& name
    ) const {
        const auto exact = functionSignatures.find(name);
        if (exact != functionSignatures.end()) return &exact->second;
        return findNativeBuiltinDeclaration(name);
    }

    // Inside its own package a function keeps its bare name in the IR, while the
    // signature table keys module functions by their package-qualified name.
    // A call like Buffer::utf16 -> wideByteLength then misses the table even
    // though the function is emitted. Accept a single qualified match; two
    // candidates mean the bare name is genuinely ambiguous, so stay unresolved.
    const FunctionSignature* resolveFunctionSignatureByUniqueSuffix(
        const std::string& name
    ) const {
        if (name.empty()) return nullptr;
        const std::string suffix = "." + name;
        const FunctionSignature* match = nullptr;
        for (const auto& entry : functionSignatures) {
            const std::string& key = entry.first;
            if (key.size() <= suffix.size()) continue;
            if (key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            if (match != nullptr) return nullptr;
            match = &entry.second;
        }
        return match;
    }

    // A type implementation exposes its static methods through the type
    // name: int.twice(3). The call site only carries the qualified name
    // "TypeName::method", so resolve it to the real signature first.
    std::string resolveCallableName(const std::string& name) const {
        if (functionSignatures.find(name) != functionSignatures.end()) {
            return name;
        }
        const auto typeStatic = typeImplStaticSignatures.find(name);
        if (typeStatic != typeImplStaticSignatures.end()) {
            return typeStatic->second;
        }
        return name;
    }

    const FunctionSignature* resolveAddressableFunctionSignature(
        const std::string& functionName
    ) const {
        if (const auto* exact = resolveFunctionSignature(
                resolveCallableName(functionName))) {
            return exact;
        }
        return resolveFunctionSignatureByUniqueSuffix(functionName);
    }

    bool matchesSpecializedFunctionReference(
        const std::string& referenceName,
        const std::string& targetName
    ) const {
        if (referenceName == targetName) return true;
        const auto target = functionSignatures.find(targetName);
        return target != functionSignatures.end() &&
            target->second.genericOrigin == referenceName;
    }

    static uint64_t providedPrefixMask(size_t count) {
        return count >= 64 ? UINT64_MAX : ((uint64_t{1} << count) - 1);
    }

    static uint64_t providedArgumentMask(const std::vector<bool>& provided) {
        uint64_t mask = 0;
        for (size_t i = 0; i < provided.size() && i < 64; ++i) {
            if (provided[i]) mask |= uint64_t{1} << i;
        }
        return mask;
    }

    static bool bindCallArguments(
        const FunctionSignature& signature,
        const std::vector<NativeValue>& arguments,
        std::vector<NativeValue>& ordered,
        std::vector<bool>& provided,
        std::vector<NativeValue>& variadic,
        std::vector<std::pair<std::string, NativeValue>>& keywordVariadic,
        std::string& error
    ) {
        const size_t regular = signature.parameterTypes.size();
        ordered.resize(regular);
        provided.assign(regular, false);
        size_t positional = 0;
        std::set<std::string> seenNames;
        for (const auto& argument : arguments) {
            if (argument.namedArgument.empty()) {
                if (positional < regular) {
                    if (provided[positional]) {
                        error = "positional argument duplicates a named argument";
                        return false;
                    }
                    ordered[positional] = argument;
                    provided[positional] = true;
                } else if (signature.hasVariadicArgs) {
                    variadic.push_back(argument);
                } else {
                    error = "too many positional arguments";
                    return false;
                }
                ++positional;
                continue;
            }
            if (!seenNames.insert(argument.namedArgument).second) {
                error = "duplicate named argument '" + argument.namedArgument + "'";
                return false;
            }
            const auto found = std::find(signature.parameterNames.begin(),
                                         signature.parameterNames.end(), argument.namedArgument);
            if (found == signature.parameterNames.end()) {
                if (signature.hasVariadicKwargs) {
                    keywordVariadic.emplace_back(argument.namedArgument, argument);
                    continue;
                }
                error = "unexpected named argument '" + argument.namedArgument + "'";
                return false;
            }
            const size_t index = static_cast<size_t>(found - signature.parameterNames.begin());
            if (provided[index]) {
                error = "multiple values for parameter '" + argument.namedArgument + "'";
                return false;
            }
            ordered[index] = argument;
            provided[index] = true;
        }
        for (size_t i = 0; i < regular; ++i) {
            if (!provided[i] && !signature.defaultParameterMask[i]) {
                error = "missing required argument '" +
                    (i < signature.parameterNames.size() ? signature.parameterNames[i]
                                                          : std::to_string(i)) + "'";
                return false;
            }
        }
        return true;
    }

    bool emitVariadicContainers(
        bool argsNeeded,
        bool kwargsNeeded,
        const std::vector<NativeValue>& variadic,
        const std::vector<std::pair<std::string, NativeValue>>& keywordVariadic,
        const std::function<bool(const NativeValue&, std::string&, bool&)>& toDynamic,
        const ContainerABI::FreshTemporary& fresh,
        const ContainerABI::EmitLine& emit,
        std::string& argsArray,
        std::string& kwargsDict
    ) {
        std::vector<std::string> values;
        std::vector<std::string> owned;
        for (const auto& argument : variadic) {
            std::string converted;
            bool owns = false;
            if (!toDynamic(argument, converted, owns)) return false;
            values.push_back(converted);
            if (owns) owned.push_back(converted);
        }
        if (argsNeeded) {
            argsArray = ContainerABI::emitBuildCall(false,
                static_cast<int64_t>(variadic.size()), values, fresh, emit);
        }
        for (const auto& value : owned) emit("call void @zeus_value_release(ptr " + value + ")");
        values.clear();
        owned.clear();
        for (const auto& [name, argument] : keywordVariadic) {
            const std::string nameValue = fresh();
            const std::string pooled = poolStr(name);
            emit(nameValue + " = alloca %ZeusValue, align 8");
            emit("call void @zeus_value_from_string_data(ptr " + nameValue +
                 ", ptr " + pooled + ", i64 " + std::to_string(name.size()) + ")");
            values.push_back(nameValue);
            owned.push_back(nameValue);
            std::string converted;
            bool owns = false;
            if (!toDynamic(argument, converted, owns)) return false;
            values.push_back(converted);
            if (owns) owned.push_back(converted);
        }
        if (kwargsNeeded) {
            kwargsDict = ContainerABI::emitBuildCall(true,
                static_cast<int64_t>(keywordVariadic.size()), values, fresh, emit);
        }
        for (const auto& value : owned) emit("call void @zeus_value_release(ptr " + value + ")");
        return true;
    }

    static bool usesDynamicZeusValueSignature(const FunctionSignature& signature) {
        if (!signature.returnType.empty()) return false;
        return std::all_of(
            signature.parameterTypes.begin(), signature.parameterTypes.end(),
            [](const std::string& typeName) { return typeName.empty(); }
        );
    }

    static bool annotationKind(
        const IRType::IRInstr& ins,
        const std::string& expected
    ) {
        if (ins.op != VMOperator::IR_ANNOTATION) return false;
        if (!ins.metadata.tag.empty()) return ins.metadata.tag == expected;

        return !ins.args.empty() &&
               std::holds_alternative<std::string>(ins.args[0]) &&
               std::get<std::string>(ins.args[0]) == expected;
    }

    static bool annotationString(
        const IRType::IRInstr& ins,
        size_t index,
        std::string& value
    ) {
        if (index >= ins.args.size() || !std::holds_alternative<std::string>(ins.args[index])) {
            return false;
        }

        value = std::get<std::string>(ins.args[index]);
        return true;
    }

    static bool annotationSymbolName(
        const IRType::IRInstr& ins,
        std::string& value
    ) {
        if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall ||
            ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
            return ins.metadata.readDirectCall(value);
        }

        return annotationString(ins, 1, value);
    }

    static bool readDirectCallAt(
        const std::vector<IRType::IRInstr>& chunk,
        size_t instructionIndex,
        std::string& targetName
    ) {
        if (instructionIndex >= chunk.size()) return false;
        const auto& instruction = chunk[instructionIndex];
        if (instruction.metadata.readDirectCall(targetName)) return true;

        if (instruction.op != VMOperator::CALL_CLOSURE &&
            instruction.op != VMOperator::OBJ_CALL_METHOD &&
            instruction.op != VMOperator::OBJ_CALL_SAFE) {
            return false;
        }
        if (instructionIndex == 0) return false;

        const auto& annotation = chunk[instructionIndex - 1];
        return annotationKind(annotation, "direct_call") &&
               annotationSymbolName(annotation, targetName);
    }

    static bool annotationInteger(
        const IRType::IRInstr& ins,
        size_t index,
        int64_t& value
    ) {
        return tryArgInt(ins, index, value);
    }

    static bool isNativeType(const std::string& typeName) {
        const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName);
        return abi != nullptr &&
               abi->valueABI != ZeusBuiltins::NativeValueABI::ZeusValue;
    }

    static bool isBuiltinTypeName(const std::string& typeName) {
        if (!typeName.empty() && typeName.front() == '&') {
            return isBuiltinTypeName(typeName.substr(1));
        }
        if (typeName == "any" || typeName == "unknown") return true;
        return ZeusBuiltins::findNativeTypeABI(typeName) != nullptr ||
               ZeusBuiltins::findTypeImplementation(typeName) != nullptr;
    }

    static bool isReferenceTypeName(const std::string& typeName) {
        return !typeName.empty() && typeName.front() == '&' &&
            typeName != "&bytes";
    }

    static std::string referencedTypeName(const std::string& typeName) {
        return isReferenceTypeName(typeName) ? typeName.substr(1) : std::string();
    }

    bool isClassType(const std::string& typeName) const {
        if (classLayouts.find(typeName) == classLayouts.end()) return false;

        // A runtime class value can share a spelling with a built-in value
        // type. Keep the built-in ABI authoritative for annotations and
        // native calls; only implementation types with class-backed storage
        // use their class layout in LLVM lowering.
        if (ZeusBuiltins::findNativeTypeABI(typeName) != nullptr) {
            const auto* implementation =
                ZeusBuiltins::findTypeImplementation(typeName);
            return implementation != nullptr &&
                implementation->category != ZeusBuiltins::TypeCategory::String &&
                implementation->category != ZeusBuiltins::TypeCategory::Bytes;
        }

        return true;
    }

    bool isManagedClassValue(const NativeValue& value) const {
        return !value.dynamic && !value.functionRef && value.type == "ptr" &&
            isClassType(value.sourceType);
    }

    std::string llvmStructType(const std::string& typeName) const {
        std::string name = "%zeus_";
        for (unsigned char c : typeName) {
            name += std::isalnum(c) || c == '_' ? static_cast<char>(c) : '_';
        }
        return name;
    }

    std::string classLifetimeFinalizer(const std::string& typeName) const {
        return llvmFunctionName("__class_finalize_" + typeName);
    }

    std::string classEmbeddedSlotStore(const std::string& typeName) const {
        return llvmFunctionName("__zeus_embedded_slot_store_" + typeName);
    }

    std::string llvmType(const std::string& typeName) const {
        // Raw native pointers are represented as integer addresses in ZeusValue
        // and use the same 64-bit scalar slot as the VM native-import dispatcher.
        if (ZeusBuiltins::isNativePointerTypeName(typeName)) return "i64";
        if (const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName)) {
            return std::string(abi->llvmType);
        }
        if (!typeName.empty() && typeName.front() == '&') return "ptr";
        if (isClassType(typeName)) return "ptr";
        if (structTypes.find(typeName) != structTypes.end()) {
            return llvmStructType(typeName);
        }
        return "";
    }

    template <typename Emit, typename Fresh>
    std::string emitNativeIntegerValueConversion(
        const std::string& value,
        const std::string& sourceLLVMType,
        const std::string& sourceDeclaredType,
        const std::string& targetLLVMType,
        const std::string& targetDeclaredType,
        Emit&& emit,
        Fresh&& fresh
    ) const {
        const bool sourceUnsigned =
            ZeusBuiltins::findNativeUnsignedIntegerABI(sourceDeclaredType) != nullptr;
        const bool targetUnsigned =
            ZeusBuiltins::findNativeUnsignedIntegerABI(targetDeclaredType) != nullptr;
        return ZeusLLVM::emitNativeIntegerConversion(
            value, sourceLLVMType, targetLLVMType, sourceUnsigned, targetUnsigned,
            std::forward<Emit>(emit), std::forward<Fresh>(fresh));
    }

    template <typename Emit, typename Fresh>
    std::string emitNativeFloatingValueConversion(
        const std::string& value,
        const std::string& sourceLLVMType,
        const std::string& sourceDeclaredType,
        const std::string& targetLLVMType,
        Emit&& emit,
        Fresh&& fresh
    ) const {
        const bool sourceUnsigned =
            ZeusBuiltins::findNativeUnsignedIntegerABI(sourceDeclaredType) != nullptr;
        return ZeusLLVM::emitNativeFloatingConversion(
            value, sourceLLVMType, targetLLVMType, sourceUnsigned,
            std::forward<Emit>(emit), std::forward<Fresh>(fresh));
    }

    std::string llvmTypeStorageSize(const std::string& llvmTypeName) const {
        if (llvmTypeName == "i1" || llvmTypeName == "i8") return "1";
        if (llvmTypeName == "i16") return "2";
        if (llvmTypeName == "float" || llvmTypeName == "i32") return "4";
        if (llvmTypeName == "i64" || llvmTypeName == "double") return "8";
        if (llvmTypeName == "ptr") return isMCUMode() ? "4" : "8";
        if (llvmTypeName.empty()) return {};
        return "ptrtoint (ptr getelementptr (" + llvmTypeName +
            ", ptr null, i64 1) to i64)";
    }

    std::string llvmValueStorageSize(const std::string& typeName) const {
        if (typeName.rfind("__zeus_pack:", 0) == 0) {
            const std::string content = typeName.substr(std::string("__zeus_pack:").size());
            uint64_t maxStorage = 0;
            std::stringstream ss(content);
            std::string subType;
            while (std::getline(ss, subType, ',')) {
                if (subType.empty()) continue;
                const std::string sz = llvmValueStorageSize(subType);
                if (sz.empty()) return {};
                try {
                    uint64_t bytes = std::stoull(sz);
                    if (bytes > maxStorage) maxStorage = bytes;
                } catch (...) {
                    uint64_t ptrBytes = isMCUMode() ? 4 : 8;
                    if (ptrBytes > maxStorage) maxStorage = ptrBytes;
                }
            }
            return std::to_string(maxStorage);
        }
        if (isClassType(typeName)) {
            const auto* layout = findClassLayout(typeName);
            if (layout && layout->fieldNames.empty()) return "1";
            return llvmTypeStorageSize(llvmStructType(typeName));
        }
        if (const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName)) {
            switch (abi->valueABI) {
                case ZeusBuiltins::NativeValueABI::Int64:
                case ZeusBuiltins::NativeValueABI::FloatingPoint:
                case ZeusBuiltins::NativeValueABI::SignedInteger:
                case ZeusBuiltins::NativeValueABI::UnsignedInteger:
                    return std::to_string(abi->integerBits / 8);
                case ZeusBuiltins::NativeValueABI::Bool1:
                    return "1";
                case ZeusBuiltins::NativeValueABI::NativeString:
                case ZeusBuiltins::NativeValueABI::NativeBytes:
                    return llvmTypeStorageSize("ptr");
                case ZeusBuiltins::NativeValueABI::RawBytesRange:
                    return llvmTypeStorageSize("%ZeusRawBytesRange");
                case ZeusBuiltins::NativeValueABI::ZeusValue:
                    return {};
            }
        }

        const std::string valueType = llvmType(typeName);
        if (valueType == "i1" || valueType == "i64") {
            return llvmTypeStorageSize(valueType);
        }
        if (valueType == "ptr" && usesZeusValueABI(typeName)) return {};
        return llvmTypeStorageSize(valueType);
    }

    bool isFunctionSignatureType(const std::string& typeName) const {
        return typeName.rfind("func(", 0) == 0;
    }

    bool usesZeusValueABI(const std::string& typeName) const {
        if (!typeName.empty() && typeName.front() == '&') return false;
        // Nullable (`?`) erases to dynamic in phase 1: nil has no native
        // representation, so every `T?` value lives in a ZeusValue box.
        if (!typeName.empty() && typeName.back() == '?') return true;
        if (typeName.empty() || typeName == "nil") return true;
        if (isFunctionSignatureType(typeName)) return true;
        if (ZeusBuiltins::isNativePointerTypeName(typeName)) return false;

        if (const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName)) {
            return abi->valueABI == ZeusBuiltins::NativeValueABI::ZeusValue;
        }

        const std::string type = llvmType(typeName);
        if (type == "i1" || type == "i64") return false;
        if (isClassType(typeName) || type.rfind("%zeus_", 0) == 0) return false;
        return true;
    }

    std::string functionParameterABIType(
        const std::string& typeName,
        bool byReference = false
    ) const {
        if (byReference) return "ptr";
        return usesZeusValueABI(typeName) ? "ptr" : llvmType(typeName);
    }

    template <typename EmitLine, typename FreshTemporary>
    void emitExternalReferenceArgument(
        const std::string& typeName,
        const std::string& slot,
        std::string& argument,
        EmitLine&& emitLine,
        FreshTemporary&& freshTemporary
    ) {
        const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName);
        if (abi && abi->valueABI == ZeusBuiltins::NativeValueABI::Int64) {
            argument = freshTemporary();
            emitLine(argument + " = call ptr @zeus_value_slot_i64_pointer(ptr " +
                     slot + ")");
            return;
        }
        if (abi && abi->valueABI == ZeusBuiltins::NativeValueABI::Bool1) {
            argument = freshTemporary();
            emitLine(argument + " = call ptr @zeus_value_slot_bool_pointer(ptr " +
                     slot + ")");
            return;
        }

        // Managed values keep the ZeusValue slot ABI. A raw C++ pointer to a
        // string/class payload would bypass Zeus ownership and type metadata.
        argument = slot;
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitOptionalArgumentPlaceholder(
        const std::string& typeName,
        NativeValue& value,
        EmitLine&& emitLine,
        FreshTemporary&& freshTemporary
    ) {
        const std::string abiType = functionParameterABIType(typeName);
        if (abiType.empty()) return false;
        if (usesZeusValueABI(typeName)) {
            const std::string nilValue = freshTemporary();
            emitLine(nilValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_nil(ptr " + nilValue + ")");
            value = NativeValue{ nilValue, "ptr", false, true, typeName, false };
            return true;
        }
        if (abiType == "double") {
            value = NativeValue{ "0.000000e+00", abiType, false, false, typeName, false };
            return true;
        }
        if (abiType == "float") {
            value = NativeValue{ "0.000000e+00", abiType, false, false, typeName, false };
            return true;
        }
        if (isIntegerLLVMType(abiType) || abiType == "i1") {
            value = NativeValue{ "0", abiType, false, false, typeName, false };
            return true;
        }
        if (abiType == "ptr") {
            value = NativeValue{ "null", "ptr", false, false, typeName, false };
            return true;
        }
        return false;
    }

    std::string llvmFieldType(const std::string& typeName) const {
        if (usesZeusValueABI(typeName)) return "%ZeusValue";

        const std::string nativeType = llvmType(typeName);
        if (!nativeType.empty()) return nativeType;

        // 未完成静态 ABI 的字段先保留为指针槽位，避免类布局再次退。
        // { i8 } 占位；真正的字符串 容器字段 lowering 仍需动态 ABI。
        return "ptr";
    }

    const ClassLayout* findClassLayout(const std::string& typeName) const {
        auto layout = classLayouts.find(typeName);
        return layout == classLayouts.end() ? nullptr : &layout->second;
    }

    int32_t classHandleIndex(const std::string& typeName) const {
        if (classLayouts.size() > static_cast<size_t>(
                std::numeric_limits<int32_t>::max())) {
            return -1;
        }
        int32_t index = 0;
        for (const auto& entry : classLayouts) {
            if (entry.first == typeName) return index;
            ++index;
        }
        return -1;
    }

    int findFieldIndex(
        const ClassLayout& layout,
        const std::string& fieldName
    ) const {
        for (size_t i = 0; i < layout.fieldNames.size(); ++i) {
            if (layout.fieldNames[i] == fieldName) return static_cast<int>(i);
        }
        return -1;
    }

    bool supportsDynamicTypedFieldSetter(const std::string& typeName) const {
        if (!typeName.empty() && typeName.front() == '&') {
            const auto* targetABI = ZeusBuiltins::findNativeTypeABI(
                typeName.substr(1));
            return targetABI &&
                (targetABI->valueABI == ZeusBuiltins::NativeValueABI::Int64 ||
                 targetABI->valueABI == ZeusBuiltins::NativeValueABI::Bool1);
        }
        if (usesZeusValueABI(typeName) || isClassType(typeName)) return true;
        const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName);
        return abi &&
            (abi->valueABI == ZeusBuiltins::NativeValueABI::Int64 ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::SignedInteger ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint ||
             abi->valueABI == ZeusBuiltins::NativeValueABI::Bool1 ||
             ZeusBuiltins::isNativePointerABI(abi));
    }

    bool shouldUseDynamicFieldSidecar(
        const std::string& className,
        const std::string& fieldName
    ) const {
        if (dynamicFieldStores.find(fieldName) == dynamicFieldStores.end()) {
            return false;
        }
        const ClassLayout* layout = findClassLayout(className);
        if (!layout) return true;
        const int fieldIndex = findFieldIndex(*layout, fieldName);
        if (fieldIndex < 0) return true;
        const size_t index = static_cast<size_t>(fieldIndex);
        const bool explicitType = index < layout->fieldTypeExplicit.size() &&
            layout->fieldTypeExplicit[index];
        return !explicitType || index >= layout->fieldTypes.size() ||
            !supportsDynamicTypedFieldSetter(layout->fieldTypes[index]);
    }

    const FunctionSignature* findClassOperatorSignature(
        const std::string& ownerType,
        const std::string& operatorSymbol,
        size_t parameterCount
    ) const {
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (signature.ownerType == ownerType &&
                signature.operatorSymbol == operatorSymbol &&
                signature.parameterTypes.size() == parameterCount &&
                !signature.parameterTypes.empty() &&
                signature.parameterTypes.front() == ownerType) {
                return &signature;
            }
        }
        return nullptr;
    }

    const FunctionSignature* findNewOperatorSignature(
        const std::string& ownerType
    ) const {
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (signature.ownerType == ownerType &&
                signature.operatorSymbol == "new") {
                return &signature;
            }
        }
        return nullptr;
    }

    std::string annotatedConstructionTarget(const IRType::IRInstr& instruction) const {
        std::string className;
        std::vector<std::string> typeArguments;
        std::string specializedClassName;
        if (!instruction.metadata.readClassInstantiation(
                className, typeArguments, &specializedClassName)) {
            return {};
        }
        return specializedClassName.empty() ? className : specializedClassName;
    }

    bool isFunctionGlobal(int index) const {
        return functionGlobals.find(index) != functionGlobals.end();
    }

    bool collectMetadata(const ZeusCompile::Result& program) {
        globalNames = program.globalNames;
        functionSignatures.clear();
        functionSignaturesByChunk.clear();
        internalFunctionSymbolCounts.clear();
        dynamicNativeImportThunkSymbols.clear();
        externalDeclarationByFunction.clear();
        reachableFunctionChunks.clear();
        reachableExternalFunctions.clear();
        functionValueReferences.clear();
        keepAllExternalDeclarations = false;
        programUsesSharedValues = false;
        programCallsPrintOrStr = false;
        constructedClassNames.clear();
        programHasDynamicClassConstruction = false;
        functionGlobals.clear();
        classGlobals.clear();
        namespacePackagesByGlobal.clear();
        namespaceExportsByPackage.clear();
        dynamicMemberMethodNames.clear();
        dynamicMemberBridge = {};
        dynamicFieldBridge = {};
        dynamicFieldLoads.clear();
        dynamicFieldStores.clear();
        structTypes.clear();
        classLayouts.clear();
        reachableClassLayouts.clear();
        externalDeclarations.clear();

        for (size_t ci = 0; ci < program.program.size(); ++ci) {
            const auto& chunk = program.program[ci];
            bool isGenericTemplate = false;
            std::string genericOrigin;
            for (const auto& marker : chunk) {
                std::string markerName;
                if (!annotationString(marker, 0, markerName)) continue;
                if (markerName == "generic_type_parameters" ||
                    markerName == "generic_class_template") {
                    isGenericTemplate = true;
                } else if (markerName == "generic_static_specialization") {
                    annotationString(marker, 1, genericOrigin);
                }
            }

            for (const auto& ins : chunk) {
                if (ins.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
                    std::string className;
                    ClassLayout layout;
                    std::vector<std::string> classTypeParameters;
                    if (!ins.metadata.readClassLayout(
                            className,
                            layout.fieldNames,
                            layout.fieldTypes,
                            &layout.fieldConstness,
                            &layout.fieldDefaults,
                            &classTypeParameters,
                            &layout.fieldTypeExplicit,
                            &layout.fieldInitializerNames)) {
                        result.error = "[emit-llvm] 类布局元信息无效";
                        return false;
                    }

                    if (!classTypeParameters.empty()) continue;

                    classLayouts[className] = std::move(layout);
                    structTypes.insert(className);
                    continue;
                }

                const bool isExternalSignature =
                    annotationKind(ins, "external_function_signature");
                if (!isExternalSignature && !annotationKind(ins, "function_signature")) {
                    continue;
                }

                FunctionSignature signature;
                signature.chunkIndex = ci;
                signature.isExternal = isExternalSignature;
                signature.isGenericTemplate = isGenericTemplate;
                signature.genericOrigin = genericOrigin;

                if (ins.metadata.kind == IRType::IRMetadataKind::FunctionSignature ||
                    ins.metadata.kind == IRType::IRMetadataKind::ExternalFunctionSignature) {
                        if (!ins.metadata.readFunctionSignature(
                            signature.name,
                            signature.returnType,
                            signature.captureCount,
                            signature.parameterTypes,
                            &signature.ownerType,
                            &signature.operatorSymbol,
                            &signature.nativeSymbol,
                            &signature.parameterNames,
                            &signature.parameterIsReference,
                            &signature.nativeReferenceABI,
                            &signature.nativeLibraryPath,
                            &signature.nativeIndirect)) {
                        result.error = "[emit-llvm] 结构化函数签名元信息无效";
                        return false;
                    }
                    signature.isStaticMethod = ins.metadata.isStaticMethod;
                    signature.isInline = ins.metadata.isInline;
                    signature.isNoSafepoint = ins.metadata.isNoSafepoint;
                    if (!signature.isExternal && !signature.nativeSymbol.empty()) {
                        nativeExportFunctions.insert(signature.name);
                        nativeExportSymbols.insert(signature.nativeSymbol);
                    }
                } else {
                    int64_t captureCount = 0;
                    int64_t parameterCount = 0;

                    if (!annotationString(ins, 1, signature.name) ||
                        !annotationString(ins, 2, signature.returnType) ||
                        !annotationInteger(ins, 3, captureCount) ||
                        !annotationInteger(ins, 4, parameterCount) ||
                        parameterCount < 0 ||
                        ins.args.size() < 5 + static_cast<size_t>(parameterCount)) {
                        result.error = "[emit-llvm] 函数签名标注格式错误";
                        return false;
                    }

                    signature.captureCount = static_cast<int>(captureCount);
                    for (int64_t i = 0; i < parameterCount; ++i) {
                        std::string typeName;
                        if (!annotationString(ins, 5 + static_cast<size_t>(i), typeName)) {
                            result.error = "[emit-llvm] 函数参数类型标注格式错误";
                            return false;
                        }
                        signature.parameterTypes.push_back(typeName);
                    }
                    const size_t nativeSymbolIndex =
                        5 + static_cast<size_t>(parameterCount);
                    if (nativeSymbolIndex < ins.args.size()) {
                        annotationString(ins, nativeSymbolIndex, signature.nativeSymbol);
                    }
                    const size_t nativeReferenceABIIndex = nativeSymbolIndex + 1;
                    if (isExternalSignature &&
                        nativeReferenceABIIndex < ins.args.size()) {
                        annotationString(ins, nativeReferenceABIIndex,
                                         signature.nativeReferenceABI);
                    }
                    const size_t nativeLibraryPathIndex = nativeReferenceABIIndex + 1;
                    if (isExternalSignature &&
                        nativeLibraryPathIndex < ins.args.size()) {
                        annotationString(ins, nativeLibraryPathIndex,
                                         signature.nativeLibraryPath);
                    }
                }

                signature.minRequiredParams = static_cast<int>(signature.parameterTypes.size());
                signature.defaultParameterMask.assign(signature.parameterTypes.size(), false);
                if (!isExternalSignature && !chunk.empty() &&
                    chunk.front().op == VMOperator::FUNC_BEGIN &&
                    chunk.front().args.size() >= 7 &&
                    std::holds_alternative<LexerType::Value>(chunk.front().args[5]) &&
                    std::holds_alternative<LexerType::Value>(chunk.front().args[6])) {
                    const auto& header = chunk.front();
                    const int minRequired = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[5]));
                    const int defaultCount = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[6]));
                    if (minRequired < 0 ||
                        minRequired > static_cast<int>(signature.parameterTypes.size()) ||
                        defaultCount < 0 || header.args.size() < 7 +
                            static_cast<size_t>(defaultCount) * 2) {
                        result.error = "[emit-llvm] function default-parameter metadata is invalid";
                        return false;
                    }
                    signature.minRequiredParams = minRequired;
                    for (int i = 0; i < defaultCount; ++i) {
                        const size_t index = 7 + static_cast<size_t>(i) * 2;
                        if (!std::holds_alternative<LexerType::Value>(header.args[index])) {
                            result.error = "[emit-llvm] function default parameter index is invalid";
                            return false;
                        }
                        const int parameterIndex = VMHelpers::getIntFromValue(
                            std::get<LexerType::Value>(header.args[index]));
                        if (parameterIndex < 0 ||
                            parameterIndex >= static_cast<int>(signature.parameterTypes.size())) {
                            result.error = "[emit-llvm] function default parameter index is out of range";
                            return false;
                        }
                        signature.defaultParameterMask[static_cast<size_t>(parameterIndex)] = true;
                    }
                    const size_t namesOffset = 7 + static_cast<size_t>(defaultCount) * 2;
                    if (header.args.size() <= namesOffset ||
                        !std::holds_alternative<LexerType::Value>(header.args[namesOffset])) {
                        result.error = "[emit-llvm] function parameter-name metadata is invalid";
                        return false;
                    }
                    const int nameCount = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[namesOffset]));
                    if (nameCount != static_cast<int>(signature.parameterTypes.size()) ||
                        header.args.size() < namesOffset + 1 + static_cast<size_t>(nameCount)) {
                        result.error = "[emit-llvm] function parameter-name count is invalid";
                        return false;
                    }
                    std::vector<std::string> headerParameterNames;
                    headerParameterNames.reserve(static_cast<size_t>(nameCount));
                    for (int i = 0; i < nameCount; ++i) {
                        const auto& name = header.args[namesOffset + 1 + static_cast<size_t>(i)];
                        if (!std::holds_alternative<std::string>(name)) {
                            result.error = "[emit-llvm] function parameter name is invalid";
                            return false;
                        }
                        headerParameterNames.push_back(std::get<std::string>(name));
                    }
                    if (!signature.parameterNames.empty() &&
                        signature.parameterNames != headerParameterNames) {
                        result.error = "[emit-llvm] function parameter-name metadata disagrees with FUNC_BEGIN";
                        return false;
                    }
                    if (signature.parameterNames.empty()) {
                        signature.parameterNames = std::move(headerParameterNames);
                    }
                    signature.hasVariadicArgs = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[1])) != 0;
                    signature.hasVariadicKwargs = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[2])) != 0;
                    bool sawDefaultParameter = false;
                    for (bool hasDefault : signature.defaultParameterMask) {
                        if (hasDefault) {
                            sawDefaultParameter = true;
                        } else if (sawDefaultParameter) {
                            result.error = "[emit-llvm] native calls require default parameters to be trailing";
                            return false;
                        }
                    }
                    if (signature.hasDefaultParameters() &&
                        signature.parameterTypes.size() > 63) {
                        result.error = "[emit-llvm] default parameters currently support at most 63 parameters";
                        return false;
                    }
                }

                functionSignatures[signature.name] = signature;

                if (!signature.isGenericTemplate &&
                    !isFunctionSignatureType(signature.returnType) &&
                    !isBuiltinTypeName(signature.returnType) &&
                        !signature.returnType.empty() &&
                        signature.returnType.back() != '?') {
                    structTypes.insert(
                        !signature.returnType.empty() && signature.returnType.front() == '&'
                            ? signature.returnType.substr(1)
                            : signature.returnType
                    );
                }
                for (const auto& typeName : signature.parameterTypes) {
                    if (signature.isGenericTemplate) break;
                    // Nullable types never need struct emission: they erase
                    // to dynamic boxes, so "Node?" must not create a
                    // "%zeus_Node_" layout entry.
                    if (!typeName.empty() && typeName.back() == '?') continue;
                    if (!isFunctionSignatureType(typeName) &&
                        !isBuiltinTypeName(typeName) && !typeName.empty()) {
                        structTypes.insert(
                            typeName.front() == '&' ? typeName.substr(1) : typeName
                        );
                    }
                }

                if (!signature.isExternal) {
                    functionSignaturesByChunk[ci] = signature;
                    ++internalFunctionSymbolCounts[llvmFunctionName(signature.name)];
                }
            }
        }

        // 类型实现的静态方法通过类型名调用（int.twice(3)）。调用点只带
        // 限定名 "TypeName::method"，这里把它解析成实际的函数签名，
        // 这样跨包的实现也能被正确链接。
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (!signature.isStaticMethod || signature.ownerType.empty()) {
                continue;
            }
            const size_t separator = signature.name.rfind("::");
            if (separator == std::string::npos) continue;
            const std::string methodName = signature.name.substr(separator + 2);
            if (methodName.empty()) continue;
            typeImplStaticSignatures.emplace(
                signature.ownerType + "::" + methodName, signature.name);
        }

        assignDynamicNativeImportThunkSymbols();

        for (const auto& entry : functionSignatures) {
            const auto& signature = entry.second;
            if (!signature.isExternal) continue;
            const size_t declarationStart = externalDeclarations.size();

            if (signature.nativeIndirect) {
                std::string thunk;
                std::string thunkError;
                if (!buildIndirectNativeThunk(signature, thunk, thunkError)) {
                    result.error = "[emit-llvm] " + thunkError;
                    return false;
                }
                externalDeclarations += thunk;
                externalDeclarationByFunction[signature.name] =
                    externalDeclarations.substr(declarationStart);
                externalDeclarations.resize(declarationStart);
                continue;
            }

            if (!signature.nativeLibraryPath.empty()) {
                ZeusBuiltins::DynamicNativeImportDescriptor descriptor;
                descriptor.declarationName = signature.name;
                descriptor.symbol = signature.nativeSymbol;
                descriptor.libraryPath = signature.nativeLibraryPath;
                descriptor.returnType = signature.returnType;
                descriptor.parameterTypes = signature.parameterTypes;
                descriptor.parameterIsReference = signature.parameterIsReference;
                descriptor.referenceABI = signature.nativeReferenceABI;
                std::string importError;
                if (!ZeusBuiltins::isSupportedDynamicNativeImport(
                        descriptor, &importError, true)) {
                    result.error = "[emit-llvm] @native '" + signature.name +
                                   "': " + importError;
                    return false;
                }

                const bool returnsDynamic = signature.returnType == "nil";
                const std::string returnType = returnsDynamic
                    ? "void" : llvmType(signature.returnType);
                const bool nativeReturnsPointer =
                    ZeusBuiltins::isNativePointerTypeName(signature.returnType) ||
                    signature.returnType == "string";
                const std::string nativeReturnType =
                    nativeReturnsPointer ? "ptr" : returnType;
                const std::string thunk = llvmCallTarget(signature);
                const std::string libraryName = poolStr(signature.nativeLibraryPath);
                const std::string symbolName = poolStr(signature.nativeSymbol);
                externalDeclarations += "define " + returnType + " " + thunk + "(";
                if (returnsDynamic) externalDeclarations += "ptr %out";
                for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                    if (i > 0 || returnsDynamic) externalDeclarations += ", ";
                    externalDeclarations += functionParameterABIType(
                        signature.parameterTypes[i]) + " %arg" + std::to_string(i);
                }
                externalDeclarations += ") {\nentry:\n";
                externalDeclarations += "  %fn = call ptr @zeus_native_library_symbol(ptr " +
                                        libraryName + ", ptr " + symbolName + ")\n";
                std::string nativeCall = "  ";
                if (returnsDynamic) {
                    nativeCall += "call void %fn(";
                } else {
                    nativeCall += "%native_result = call ";
                    if (signature.returnType == "bool") nativeCall += "zeroext ";
                    nativeCall += nativeReturnType + " %fn(";
                }
                for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                    if (i > 0) nativeCall += ", ";
                    const std::string& declaredType = signature.parameterTypes[i];
                    std::string argumentName = "%arg" + std::to_string(i);
                    std::string parameterType = functionParameterABIType(declaredType);
                    if (ZeusBuiltins::isNativePointerTypeName(declaredType)) {
                        argumentName = "%native_arg_" + std::to_string(i);
                        externalDeclarations += "  " + argumentName +
                            " = inttoptr i64 %arg" + std::to_string(i) + " to ptr\n";
                        parameterType = "ptr";
                    }
                    nativeCall += parameterType;
                    if (declaredType == "bool") nativeCall += " zeroext";
                    nativeCall += " " + argumentName;
                }
                nativeCall += ")\n";
                externalDeclarations += nativeCall;
                if (returnsDynamic) {
                    externalDeclarations += "  call void @zeus_value_init_nil(ptr %out)\n";
                    externalDeclarations += "  ret void\n";
                } else if (signature.returnType == "string") {
                    externalDeclarations +=
                        "  %zeus_result = call ptr @zeus_string_from_cstr(ptr %native_result)\n";
                    externalDeclarations += "  ret ptr %zeus_result\n";
                } else if (ZeusBuiltins::isNativePointerTypeName(signature.returnType)) {
                    externalDeclarations +=
                        "  %zeus_result = ptrtoint ptr %native_result to i64\n";
                    externalDeclarations += "  ret i64 %zeus_result\n";
                } else {
                    externalDeclarations += "  ret " + returnType + " %native_result\n";
                }
                externalDeclarations += "}\n\n";
                externalDeclarationByFunction[signature.name] =
                    externalDeclarations.substr(declarationStart);
                externalDeclarations.resize(declarationStart);
                continue;
            }

            // 显式 `-> void` 的外部声明就是 C 的 void 函数：没有出参、没有
            // 结果值。空返回类型不能同样处理——内建模块（如 zeus.dict）用
            // `decl func __dict_clone(value);` 表示“经出参返回动态值”，
            // 把它当 void 会同时打断声明和调用点的实参个数。
            const bool declaresVoidReturn = signature.isExternal &&
                (signature.returnType == "void" || signature.returnType == "nil");
            const bool returnsDynamic =
                !declaresVoidReturn && usesZeusValueABI(signature.returnType);
            const std::string returnType = returnsDynamic
                ? "void"
                : (declaresVoidReturn ? "void" : llvmType(signature.returnType));
            if (returnType.empty()) {
                result.error = "[emit-llvm] decl 函数 '" + signature.name +
                               "' 的返回类型没有 LLVM ABI 映射";
                return false;
            }

            const std::string externalName = signature.nativeSymbol.empty()
                ? llvmFunctionName(signature.name)
                : llvmExternalFunctionName(signature.nativeSymbol);
            externalDeclarations += "declare " + returnType + " " +
                                    externalName + "(";
            if (returnsDynamic) externalDeclarations += "ptr";
            for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                if (i > 0 || returnsDynamic) externalDeclarations += ", ";
                const bool byReference = i < signature.parameterIsReference.size() &&
                    signature.parameterIsReference[i];
                const std::string parameterType =
                    functionParameterABIType(
                        signature.parameterTypes[i], byReference);
                if (parameterType.empty()) {
                    result.error = "[emit-llvm] decl 函数 '" + signature.name +
                                   "' 的参数类型没有 LLVM ABI 映射";
                    return false;
                }
                externalDeclarations += parameterType;
            }
            externalDeclarations += ")\n";
            externalDeclarationByFunction[signature.name] =
                externalDeclarations.substr(declarationStart);
            externalDeclarations.resize(declarationStart);
        }

        for (const auto& entry : classLayouts) {
            for (const auto& fieldType : entry.second.fieldTypes) {
                if (fieldType.empty() || fieldType.back() == '?') continue;
                if (!isBuiltinTypeName(fieldType)) {
                    structTypes.insert(fieldType);
                }
            }
        }

        for (size_t i = 0; i < globalNames.size(); ++i) {
            const auto function = functionSignatures.find(globalNames[i]);
            if (function != functionSignatures.end() && function->second.isExternal) {
                functionGlobals[static_cast<int>(i)] = function->second.name;
            }
            if (classLayouts.find(globalNames[i]) != classLayouts.end()) {
                classGlobals[static_cast<int>(i)] = globalNames[i];
                continue;
            }

            // Package globals are stored as `package.Class`, while the
            // class-layout metadata currently carries the source-level class
            // name. Match a unique suffix so `new Class(...)` inside an
            // imported implementation module remains a native class ref.
            std::string matchedClassName;
            bool ambiguousClassName = false;
            for (const auto& layout : classLayouts) {
                const std::string suffix = "." + layout.first;
                if (globalNames[i].size() <= suffix.size() ||
                    globalNames[i].compare(
                        globalNames[i].size() - suffix.size(), suffix.size(), suffix
                    ) != 0) {
                    continue;
                }
                if (!matchedClassName.empty()) {
                    ambiguousClassName = true;
                    break;
                }
                matchedClassName = layout.first;
            }
            if (!ambiguousClassName && !matchedClassName.empty()) {
                classGlobals[static_cast<int>(i)] = std::move(matchedClassName);
            }
        }

        // Internal globals are native function references only when top-level
        // code initializes that exact slot from MAKE_FUNCTION. Matching a
        // slot's name to any internal signature is incorrect: a returned
        // closure can share its global name with a nested function and must
        // keep its runtime capture environment. External declarations are
        // mapped separately from their explicit ABI signatures above.
        std::map<int, std::string> declaredFunctionGlobals;
        std::set<int> ambiguousFunctionGlobals;
        for (const auto& chunk : program.program) {
            if (chunk.empty() || chunk.front().op == VMOperator::FUNC_BEGIN) {
                continue;
            }
            for (size_t pc = 0; pc + 1 < chunk.size(); ++pc) {
                const auto& makeFunction = chunk[pc];
                const auto& storeGlobal = chunk[pc + 1];
                if (makeFunction.op != VMOperator::MAKE_FUNCTION ||
                    makeFunction.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(makeFunction.args[0]) ||
                    storeGlobal.op != VMOperator::STORE_VALUE_GLOBAL ||
                    storeGlobal.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(storeGlobal.args[0])) {
                    continue;
                }

                const int functionChunk = VMHelpers::getIntFromValue(
                    std::get<LexerType::Value>(makeFunction.args[0]));
                const int globalIndex = VMHelpers::getIntFromValue(
                    std::get<LexerType::Value>(storeGlobal.args[0]));
                const auto signature = functionSignaturesByChunk.find(
                    static_cast<size_t>(functionChunk));
                if (signature == functionSignaturesByChunk.end() ||
                    signature->second.captureCount != 0 ||
                    globalIndex < 0 ||
                    static_cast<size_t>(globalIndex) >= globalNames.size()) {
                    continue;
                }

                const auto inserted = declaredFunctionGlobals.emplace(
                    globalIndex, signature->second.name);
                if (!inserted.second && inserted.first->second != signature->second.name) {
                    ambiguousFunctionGlobals.insert(globalIndex);
                }
            }
        }
        for (const auto& function : declaredFunctionGlobals) {
            if (ambiguousFunctionGlobals.find(function.first) ==
                ambiguousFunctionGlobals.end()) {
                functionGlobals[function.first] = function.second;
            }
        }
        for (const auto& chunk : program.program) {
            if (chunk.empty()) continue;
            if (chunk.front().metadata.kind !=
                    IRType::IRMetadataKind::ModuleImportInit) {
                continue;
            }

            const std::string& packageName = chunk.front().metadata.moduleName;
            int64_t aliasGlobal = -1;
            std::set<std::string> exports;
            for (const auto& instruction : chunk) {
                if (instruction.op == VMOperator::IMPORT &&
                    instruction.args.size() > 3) {
                    tryArgInt(instruction, 3, aliasGlobal);
                }
                if (instruction.op == VMOperator::STORE_VALUE_GLOBAL) {
                    tryArgInt(instruction, 0, aliasGlobal);
                }
                if (instruction.op != VMOperator::PUSH ||
                    instruction.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(
                        instruction.args[0])) {
                    continue;
                }
                const auto& literal =
                    std::get<LexerType::Value>(instruction.args[0]);
                if (std::holds_alternative<std::string>(literal.data)) {
                    const std::string& name =
                        std::get<std::string>(literal.data);
                    if (name != packageName) exports.insert(name);
                }
            }

            if (aliasGlobal < 0 ||
                static_cast<size_t>(aliasGlobal) >= globalNames.size()) {
                continue;
            }
            namespacePackagesByGlobal[static_cast<int>(aliasGlobal)] = packageName;
            auto& packageExports = namespaceExportsByPackage[packageName];
            packageExports.insert(exports.begin(), exports.end());
        }

        return true;
    }

    bool isBuiltinImplementationFunction(const FunctionSignature& signature) const {
        if (signature.name.rfind("ZeusBuiltins.", 0) == 0 ||
            signature.name.rfind("zeus.", 0) == 0) return true;
        if (signature.ownerType.empty()) return false;
        if (ZeusBuiltins::findTypeImplementation(signature.ownerType) != nullptr) {
            return true;
        }
        const size_t lastDot = signature.ownerType.rfind('.');
        return lastDot != std::string::npos &&
            ZeusBuiltins::findTypeImplementation(
                signature.ownerType.substr(lastDot + 1)) != nullptr;
    }

    bool isReachabilityManagedFunction(const FunctionSignature& signature) const {
        return isBuiltinImplementationFunction(signature) ||
            (!signature.ownerType.empty() && isClassType(signature.ownerType));
    }

    static std::string methodNameForSignature(const FunctionSignature& signature) {
        if (signature.ownerType.empty()) return {};
        const std::string prefix = signature.ownerType + "::";
        if (signature.name.rfind(prefix, 0) != 0) return {};
        return signature.name.substr(prefix.size());
    }

    bool hasTypedInstanceMethodSignature(
        const std::string& ownerType,
        const std::string& methodName
    ) const {
        if (ownerType.empty() || methodName.empty()) return false;
        const auto function = functionSignatures.find(ownerType + "::" + methodName);
        return function != functionSignatures.end() &&
            function->second.ownerType == ownerType &&
            !function->second.isStaticMethod &&
            !function->second.parameterTypes.empty() &&
            function->second.parameterTypes.front() == ownerType;
    }

    const FunctionSignature* findAnyInstanceMethod(
        const std::string& methodName
    ) const {
        if (methodName.empty()) return nullptr;
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (methodNameForSignature(signature) == methodName &&
                !signature.ownerType.empty() &&
                !signature.isStaticMethod &&
                !signature.parameterTypes.empty() &&
                signature.parameterTypes.front() == signature.ownerType) {
                return &signature;
            }
        }
        return nullptr;
    }

    const FunctionSignature* findNamespaceFunction(
        const std::string& packageName,
        const std::string& memberName
    ) const {
        const auto exports = namespaceExportsByPackage.find(packageName);
        if (exports == namespaceExportsByPackage.end() ||
            exports->second.find(memberName) == exports->second.end()) {
            return nullptr;
        }
        const auto function = functionSignatures.find(packageName + "." + memberName);
        return function == functionSignatures.end() ? nullptr : &function->second;
    }

    const FunctionSignature* findStaticClassFunction(
        const std::string& className,
        const std::string& memberName
    ) const {
        if (className.empty() || memberName.empty()) return nullptr;
        const auto function = functionSignatures.find(className + "::" + memberName);
        if (function == functionSignatures.end() ||
            function->second.ownerType != className ||
            !function->second.isStaticMethod) {
            return nullptr;
        }
        return &function->second;
    }
