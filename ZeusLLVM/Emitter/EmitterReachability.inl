// Emitter member definitions: usage flags, reachability pruning, and dynamic bridges; included inside class Emitter.

    // Syntactic pre-scan: usage facts that need no abstract interpretation.
    // Conservative by construction: a flag is set only on positive evidence,
    // and every gate removes emission only when its flag proves absence.
    // Anything the scan does not understand keeps the old emit-everything
    // behavior.
    // Call-graph walk for usage flags: only code that can actually execute may
    // set a flag. A syntactic scan of all chunks is unsound here because the
    // IR always carries the stdlib prelude (whose dict helpers call str()),
    // even when none of it is reachable. Roots are top-level chunks plus an
    // explicit main; edges follow direct calls, created closures (only when
    // some call site cannot be resolved statically), member/subscript/with
    // dispatches (over-approximated by name), and construction (into
    // init/new/destructor bodies). Anything the walk cannot resolve keeps
    // the old emit-everything behavior for the affected gate.
    void collectProgramUsageFlags(
        const std::vector<std::vector<IRType::IRInstr>>& program,
        const std::set<size_t>* rootChunks = nullptr
    ) {
        programUsesSharedValues = false;
        programCallsPrintOrStr = false;
        constructedClassNames.clear();
        programHasDynamicClassConstruction = false;
        if (program.empty()) return;
        std::vector<size_t> worklist;
        std::vector<bool> visited(program.size(), false);
        auto pushChunk = [&](size_t ci) {
            if (ci < program.size() && !visited[ci]) worklist.push_back(ci);
        };
        for (size_t ci = 0; ci < program.size(); ++ci) {
            if (functionSignaturesByChunk.find(ci) == functionSignaturesByChunk.end() &&
                (rootChunks == nullptr || rootChunks->find(ci) != rootChunks->end())) {
                pushChunk(ci);
            }
        }
        for (const auto& exportedName : nativeExportFunctions) {
            if (const auto* exportedFunc = resolveFunctionSignature(exportedName)) {
                if (!exportedFunc->isExternal && !exportedFunc->isGenericTemplate) {
                    pushChunk(exportedFunc->chunkIndex);
                }
            }
        }
        if (const auto* mainFunction = resolveFunctionSignature("main")) {
            if (!mainFunction->isExternal && !mainFunction->isGenericTemplate) {
                pushChunk(mainFunction->chunkIndex);
            }
        }
        std::vector<size_t> createdClosureChunks;
        bool hasUnresolvableCall = false;
        const auto pushSameNamedMethods = [&](const std::string& method) {
            for (const auto& entry : functionSignatures) {
                const auto& candidate = entry.second;
                if (candidate.isExternal) continue;
                if (methodNameForSignature(candidate) != method) continue;
                pushChunk(candidate.chunkIndex);
            }
        };
        const auto pushOperatorImpls = [&](const std::string& symbol) {
            for (const auto& entry : functionSignatures) {
                const auto& candidate = entry.second;
                if (candidate.isExternal) continue;
                if (candidate.operatorSymbol != symbol) continue;
                pushChunk(candidate.chunkIndex);
            }
        };
        const auto pushLifecycleBodies = [&](const std::string& type) {
            for (const auto& entry : functionSignatures) {
                const auto& candidate = entry.second;
                if (candidate.isExternal) continue;
                if (candidate.ownerType != type) continue;
                if (candidate.name == type + "::init" ||
                    candidate.operatorSymbol == "new" ||
                    candidate.operatorSymbol == "~") {
                    pushChunk(candidate.chunkIndex);
                }
            }
        };
        while (!worklist.empty() || (hasUnresolvableCall && !createdClosureChunks.empty())) {
            while (!worklist.empty()) {
                const size_t ci = worklist.back();
                worklist.pop_back();
                if (ci >= program.size() || visited[ci]) continue;
                visited[ci] = true;
                for (size_t pc = 0; pc < program[ci].size(); ++pc) {
                    const auto& ins = program[ci][pc];
                    switch (ins.op) {
                        case VMOperator::CLASS_BUILD_SHARED:
                        case VMOperator::OBJ_BUILD_SHARED:
                        case VMOperator::STORE_VALUE_SHARED:
                        case VMOperator::LOAD_VALUE_SHARED:
                        case VMOperator::ASSIGN_VALUE_SHARED:
                        case VMOperator::SHARED_LOCK:
                        case VMOperator::SHARED_UNLOCK:
                            programUsesSharedValues = true;
                            break;
                        default:
                            break;
                    }
                    if (ins.op == VMOperator::LLVM_ADDR &&
                        ins.args.size() >= 3 &&
                        std::holds_alternative<std::string>(ins.args[0]) &&
                        std::get<std::string>(ins.args[0]) == "function" &&
                        std::holds_alternative<std::string>(ins.args[2])) {
                        if (const auto* target = resolveAddressableFunctionSignature(
                                std::get<std::string>(ins.args[2]));
                            target != nullptr && !target->isExternal &&
                            !target->isGenericTemplate) {
                            pushChunk(target->chunkIndex);
                        }
                    }
                    std::string directTarget;
                    const bool hasDirectCall = readDirectCallAt(
                        program[ci], pc, directTarget);
                    if (hasDirectCall &&
                        (directTarget == "print" || directTarget == "delayPrint" ||
                         directTarget == "str" || directTarget == "__builtin_str")) {
                        programCallsPrintOrStr = true;
                    } else if (hasDirectCall) {
                        if (const auto* target = resolveFunctionSignature(directTarget)) {
                            // Generic templates are walked for their body text:
                            // a print inside the template may execute after
                            // instantiation. Safe direction only.
                            if (!target->isExternal) {
                                pushChunk(target->chunkIndex);
                            }
                        }
                    } else if (ins.op == VMOperator::CALL_FUNC ||
                               ins.op == VMOperator::CALL_CLOSURE) {
                        hasUnresolvableCall = true;
                    }
                    if (ins.op == VMOperator::LOAD_VALUE && !ins.args.empty() &&
                        std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        if (name == "print" || name == "delayPrint" || name == "str") {
                            programCallsPrintOrStr = true;
                        }
                    }
                    if (ins.op == VMOperator::MAKE_FUNCTION && !ins.args.empty() &&
                        std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        int64_t targetChunk = -1;
                        if (tryArgInt(ins, 0, targetChunk) && targetChunk >= 0) {
                            createdClosureChunks.push_back(static_cast<size_t>(targetChunk));
                        }
                    }
                    if ((ins.op == VMOperator::OBJ_CALL_METHOD ||
                         ins.op == VMOperator::OBJ_CALL_SAFE) && !ins.args.empty() &&
                        std::holds_alternative<std::string>(ins.args.front())) {
                        const std::string& method = std::get<std::string>(ins.args.front());
                        // io.print funnels into the same runtime stringifier;
                        // a same-named user method only costs a missed fold.
                        if (method == "print") {
                            programCallsPrintOrStr = true;
                        }
                        pushSameNamedMethods(method);
                    }
                    if (ins.op == VMOperator::WITH_BEGIN) {
                        pushOperatorImpls("operator[with]");
                        pushOperatorImpls("operator[~with]");
                    }
                    if ((ins.op == VMOperator::DICT_LOAD_VALUE ||
                         ins.op == VMOperator::DICT_STORE_VALUE) &&
                        !hasDirectCall) {
                        pushOperatorImpls("[]");
                        pushOperatorImpls("[]=");
                    }
                    // NOTE: CLASS_BUILD is a class *declaration*, not an
                    // instantiation (the main scan treats it the same way);
                    // only OBJ_BUILD/NEW_OBJ construct instances.
                    if (ins.op == VMOperator::OBJ_BUILD ||
                        ins.op == VMOperator::OBJ_BUILD_SHARED ||
                        ins.op == VMOperator::NEW_OBJ) {
                        std::string targetType;
                        if (ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            targetType = std::get<std::string>(ins.args[1]);
                        }
                        // Ordinary `new T()` carries its type in class-
                        // instantiation metadata, not in args[1] (which is
                        // only set for native constructions).
                        std::string genericClassName;
                        std::vector<std::string> genericTypeArguments;
                        std::string specializedClassName;
                        if (targetType.empty() &&
                            ins.metadata.readClassInstantiation(
                                genericClassName, genericTypeArguments,
                                &specializedClassName) &&
                            !specializedClassName.empty()) {
                            targetType = specializedClassName;
                        }
                        if (!targetType.empty()) {
                            pushLifecycleBodies(targetType);
                        } else {
                            // The walk cannot resolve class handles that the
                            // compiler loads from global slots (the common
                            // same-unit `new T()` form), so it only ensures
                            // constructors/destructors/initializers are
                            // walked: a print inside them still counts. The
                            // construction set used for toString seeding is
                            // owned by the main scan's operand stack.
                            for (const auto& entry : functionSignatures) {
                                const auto& candidate = entry.second;
                                if (candidate.isExternal) continue;
                                if (candidate.operatorSymbol == "new" ||
                                    candidate.operatorSymbol == "~" ||
                                    candidate.name == candidate.ownerType + "::init") {
                                    pushChunk(candidate.chunkIndex);
                                }
                            }
                        }
                    }
                }
            }
            if (hasUnresolvableCall && !createdClosureChunks.empty()) {
                // A closure value may flow to a call site this walk cannot
                // resolve; any created closure body may therefore execute.
                // Note: auto-imported prelude closures (str, dict helpers)
                // are walked too when reachable this way; they only ever set
                // flags in the safe direction (keep more code).
                for (const size_t chunk : createdClosureChunks) pushChunk(chunk);
                createdClosureChunks.clear();
            }
        }
    }

    void collectReachableFunctionChunks(
        const std::vector<std::vector<IRType::IRInstr>>& program,
        const std::set<size_t>* rootChunks = nullptr,
        const std::set<int>* allowedFunctionSourceFileIds = nullptr
    ) {
        collectProgramUsageFlags(program, rootChunks);
        dynamicMemberMethodNames.clear();
        // The runtime stringifier looks up "toString" through the dynamic
        // registry, so the name has to be present for the bridge to generate
        // adapters. Impls still gate separately on reachability (direct calls,
        // dynamic dispatches below), so an empty registry costs nothing.
        // NOTE: there is deliberately no builtin toString SEED here (it used
        // to pull the whole dict module into every printing program); builtin
        // containers stringify through the C++ fallback with identical
        // output, verified by print/str round-trips of dict values.
        if (programCallsPrintOrStr) {
            dynamicMemberMethodNames.insert("toString");
        }
        dynamicFieldStores.clear();
        std::map<std::string, std::set<std::string>> inferredFields;
        std::vector<size_t> pending;
        for (const auto& exportedName : nativeExportFunctions) {
            if (const auto* exportedFunc = resolveFunctionSignature(exportedName)) {
                if (!exportedFunc->isExternal && !exportedFunc->isGenericTemplate &&
                    reachableFunctionChunks.insert(exportedFunc->chunkIndex).second) {
                    pending.push_back(exportedFunc->chunkIndex);
                }
            }
        }
        if (const auto* mainFunction = resolveFunctionSignature("main")) {
            if (!mainFunction->isExternal && !mainFunction->isGenericTemplate &&
                reachableFunctionChunks.insert(mainFunction->chunkIndex).second) {
                pending.push_back(mainFunction->chunkIndex);
            }
        }
        for (size_t ci = 0; ci < program.size(); ++ci) {
            if (functionSignaturesByChunk.find(ci) == functionSignaturesByChunk.end() &&
                (rootChunks == nullptr || rootChunks->find(ci) != rootChunks->end())) {
                pending.push_back(ci);
            }
        }

        auto addTarget = [&](const std::string& name) {
            const auto* function = resolveFunctionSignature(name);
            if (function == nullptr) return;
            if (function->isExternal) {
                reachableExternalFunctions.insert(function->name);
                return;
            }
            if (function->isGenericTemplate) {
                return;
            }
            const size_t targetChunk = function->chunkIndex;
            if (reachableFunctionChunks.insert(targetChunk).second) {
                pending.push_back(targetChunk);
            }
        };

        std::map<size_t, std::map<size_t, std::set<std::string>>>
            inferredParameterInstanceTypes;
        std::map<size_t, std::set<size_t>> unknownParameterTypes;
        std::map<size_t, size_t> analysisVersions;
        std::map<size_t, size_t> scannedVersions;
        auto recordParameterTypes = [&](const FunctionSignature& target,
                                        size_t parameterIndex,
                                        const std::set<std::string>& types,
                                        bool unknown) {
            if (target.isExternal || target.isGenericTemplate ||
                target.chunkIndex >= program.size()) {
                return;
            }
            bool changed = false;
            if (unknown) {
                changed = unknownParameterTypes[target.chunkIndex]
                    .insert(parameterIndex).second;
            } else {
                auto& inferred =
                    inferredParameterInstanceTypes[target.chunkIndex][parameterIndex];
                for (const auto& type : types) {
                    changed = inferred.insert(type).second || changed;
                }
            }
            if (changed) {
                ++analysisVersions[target.chunkIndex];
                if (reachableFunctionChunks.find(target.chunkIndex) !=
                    reachableFunctionChunks.end()) {
                    pending.push_back(target.chunkIndex);
                }
            }
        };
        auto markUnknownFunctionParameters = [&](const FunctionSignature& target) {
            for (size_t i = 0; i < target.parameterTypes.size(); ++i) {
                recordParameterTypes(target, i, {}, true);
            }
        };
        bool hasUnknownIndirectFunctionCall = false;
        std::set<std::string> seededClassCopyHooks;
        auto markReachableFunctionsUnknown = [&]() {
            for (const size_t chunk : reachableFunctionChunks) {
                const auto target = functionSignaturesByChunk.find(chunk);
                if (target != functionSignaturesByChunk.end()) {
                    markUnknownFunctionParameters(target->second);
                }
            }
        };

        // print()/str() stringify handle-boxed class instances through the
        // runtime dynamic-member registry, so a class's toString has to be
        // emitted whenever an instance of it can reach the stringifier. Static
        // and dynamic `toString` dispatches seed their own targets (direct
        // calls / methodCalls below); the block below only covers the implicit
        // path, which the call graph cannot see because the runtime resolves
        // the method by name.
        //
        // Seeding is keyed on positive evidence, and only when the program
        // actually prints:
        //  - user class: seed toString iff the class is constructed in walked,
        //    execution-reachable code. The main scan tracks operand-stack
        //    class handles (including handles loaded from global slots), which
        //    is the form same-unit `new T()` lowers to. This closes the AOT
        //    gap where print(obj) fell back to <Type:#N> while the VM found
        //    toString dynamically.
        //  - builtin Dict: the only builtin whose handle-boxed instances are
        //    printed through the registry (`new Dict()`); literals are plain
        //    variants and never trigger a lookup, so they stay unseeded. Other
        //    builtins either print natively (string) or through the identical
        //    C++ fallback (lists).
        //  - an impl the AOT backend cannot compile is skipped: it would turn
        //    a graceful <Type:#N> fallback into a build failure. `str` is the
        //    known dynamic-only spelling (Array::toString stops there), and
        //    direct calls are followed transitively so a helper cannot smuggle
        //    it in.
        auto isNativelyCompilableToString = [&](const FunctionSignature& candidate) {
            if (candidate.isExternal || candidate.isGenericTemplate) return false;
            if (candidate.chunkIndex >= program.size()) return false;
            std::vector<size_t> worklist;
            std::set<size_t> visited;
            worklist.push_back(candidate.chunkIndex);
            while (!worklist.empty()) {
                const size_t chunk = worklist.back();
                worklist.pop_back();
                if (chunk >= program.size() || !visited.insert(chunk).second) continue;
                for (const auto& ins : program[chunk]) {
                    if (ins.op != VMOperator::LOAD_VALUE || ins.args.size() != 1 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        continue;
                    }
                    const std::string& name = std::get<std::string>(ins.args[0]);
                    if (name == "str") return false;
                    const FunctionSignature* callee =
                        resolveFunctionSignature(resolveCallableName(name));
                    if (callee != nullptr && !callee->isExternal &&
                        !callee->isGenericTemplate) {
                        worklist.push_back(callee->chunkIndex);
                    }
                }
            }
            return true;
        };
        auto seedToStringFor = [&](const std::string& owner) {
            // Builtin types stringify natively (string) or through the C++
            // fallback (lists); the registry opts in only the ones that must
            // be reached dynamically (Dict, the sole dynamicStringification
            // type).
            const auto* builtinType = ZeusBuiltins::findTypeImplementation(owner);
            if (builtinType != nullptr && !builtinType->dynamicStringification) return;
            for (const auto& entry : functionSignatures) {
                const auto& candidate = entry.second;
                if (candidate.ownerType != owner) continue;
                if (methodNameForSignature(candidate) != "toString") continue;
                if (candidate.parameterTypes.empty() ||
                    candidate.parameterTypes.front() != candidate.ownerType) {
                    continue;
                }
                if (!isNativelyCompilableToString(candidate)) continue;
                addTarget(candidate.name);
            }
        };

        while (!pending.empty()) {
            const size_t ci = pending.back();
            pending.pop_back();
            if (ci >= program.size()) continue;
            const size_t analysisVersion = analysisVersions[ci];
            const auto scannedVersion = scannedVersions.find(ci);
            if (scannedVersion != scannedVersions.end() &&
                scannedVersion->second == analysisVersion) {
                continue;
            }
            scannedVersions[ci] = analysisVersion;
            if (hasUnknownIndirectFunctionCall) {
                const auto currentFunction = functionSignaturesByChunk.find(ci);
                if (currentFunction != functionSignaturesByChunk.end()) {
                    markUnknownFunctionParameters(currentFunction->second);
                }
            }

            size_t nativeCallCount = 0;
            size_t nativeHandleCount = 0;
            bool hasUnresolvedNativeHandle = false;
            for (const auto& ins : program[ci]) {
                if (ins.op == VMOperator::CALL_NATIVE) ++nativeCallCount;
                if (ins.op != VMOperator::LOAD_NATIVE) continue;
                ++nativeHandleCount;
                int64_t encodedHandle = -1;
                std::string nativeName;
                if (!tryArgInt(ins, 0, encodedHandle) ||
                    !resolveNativeHandle(encodedHandle, nativeName)) {
                    hasUnresolvedNativeHandle = true;
                    continue;
                }
                const FunctionSignature* nativeDeclaration =
                    findNativeBuiltinDeclaration(nativeName);
                if (nativeDeclaration != nullptr) {
                    reachableExternalFunctions.insert(nativeDeclaration->name);
                }
            }
            if (nativeCallCount > nativeHandleCount || hasUnresolvedNativeHandle) {
                keepAllExternalDeclarations = true;
            }

            std::set<std::string> constructedTypes;
            std::set<std::string> deletedTypes;
            bool usesDynamicClassConstruction = false;
            bool usesDynamicSubscriptLoad = false;
            bool usesDynamicSubscriptStore = false;
            std::set<std::string> subscriptLoadOwnerTypes;
            std::set<std::string> subscriptStoreOwnerTypes;
            std::vector<std::string> methodCalls;

            enum class ReachabilityValueKind {
                Untracked,
                Namespace,
                Class,
                Instance,
                InstanceSet,
                Function
            };
            struct ReachabilityValue {
                ReachabilityValueKind kind;
                std::string type;
                std::set<std::string> types;
                ReachabilityValue()
                    : kind(ReachabilityValueKind::Untracked) {}
                ReachabilityValue(ReachabilityValueKind valueKind, std::string valueType)
                    : kind(valueKind), type(std::move(valueType)) {}
                ReachabilityValue(
                    ReachabilityValueKind valueKind,
                    std::set<std::string> valueTypes
                ) : kind(valueKind), types(std::move(valueTypes)) {}
            };
            auto canonicalReachabilityType = [&](const std::string& type) {
                const std::string referenceTarget = referencedTypeName(type);
                std::string unqualifiedType = referenceTarget.empty()
                    ? type : referenceTarget;
                // The nullable suffix is a static nil contract; the set of
                // concrete instance types is exactly the base type's set.
                // Dropping it here lets a `dict?` parameter dispatch only to
                // dict members instead of every class in the program.
                if (!unqualifiedType.empty() && unqualifiedType.back() == '?') {
                    unqualifiedType.pop_back();
                }
                if (const auto* builtin =
                        ZeusBuiltins::findTypeImplementation(unqualifiedType)) {
                    return std::string(builtin->canonicalName);
                }
                if (const auto* abi = ZeusBuiltins::findNativeTypeABI(unqualifiedType)) {
                    return std::string(abi->canonicalName);
                }
                return unqualifiedType;
            };
            auto noteLiveClassType = [&](const std::string& type) {
                const std::string classType = canonicalReachabilityType(type);
                if (classLayouts.find(classType) == classLayouts.end()) return;
                constructedTypes.insert(classType);
                constructedClassNames.insert(classType);
            };
            std::vector<ReachabilityValue> valueStack;
            std::map<int, ReachabilityValue> localValues;
            std::map<int, ReachabilityValue> declaredParameterValues;
            std::map<int, ReachabilityValue> globalValues;
            const auto function = functionSignaturesByChunk.find(ci);
            if (function != functionSignaturesByChunk.end()) {
                const auto& parameters = function->second.parameterTypes;
                for (size_t i = 0; i < parameters.size(); ++i) {
                    const std::string parameterType =
                        canonicalReachabilityType(parameters[i]);
                    if (classLayouts.find(parameterType) != classLayouts.end() ||
                        ZeusBuiltins::findNativeTypeABI(parameterType) != nullptr ||
                        ZeusBuiltins::findTypeImplementation(parameterType) != nullptr) {
                        localValues[static_cast<int>(i)] = {
                            ReachabilityValueKind::Instance, parameterType
                        };
                        declaredParameterValues[static_cast<int>(i)] =
                            localValues[static_cast<int>(i)];
                } else {
                    const auto unknownParameters = unknownParameterTypes.find(ci);
                    const bool hasUnknownCallsite =
                        unknownParameters != unknownParameterTypes.end() &&
                        unknownParameters->second.find(i) !=
                            unknownParameters->second.end();
                    const auto inferredByChunk =
                        inferredParameterInstanceTypes.find(ci);
                    if (!hasUnknownCallsite &&
                        inferredByChunk != inferredParameterInstanceTypes.end()) {
                        const auto inferred = inferredByChunk->second.find(i);
                        if (inferred != inferredByChunk->second.end() &&
                            !inferred->second.empty()) {
                            localValues[static_cast<int>(i)] = {
                                ReachabilityValueKind::InstanceSet,
                                inferred->second
                            };
                            declaredParameterValues[static_cast<int>(i)] =
                                localValues[static_cast<int>(i)];
                        }
                    }
                    }
                }
            }
            for (const auto& global : prescanGlobalClassTypes) {
                globalValues[global.first] = {
                    ReachabilityValueKind::Instance, global.second
                };
            }
            for (const auto& global : prescanGlobalReachabilityTypes) {
                globalValues[global.first] = {
                    ReachabilityValueKind::Instance, global.second
                };
            }
            auto popValue = [&]() {
                if (valueStack.empty()) return ReachabilityValue{};
                ReachabilityValue value = std::move(valueStack.back());
                valueStack.pop_back();
                return value;
            };
            auto popValues = [&](int64_t count) {
                if (count < 0 || static_cast<uint64_t>(count) > valueStack.size()) {
                    valueStack.clear();
                    return;
                }
                while (count-- > 0) valueStack.pop_back();
            };
            auto pushUnknown = [&]() { valueStack.emplace_back(); };
            auto instanceTypesFor = [&](const ReachabilityValue& value) {
                std::set<std::string> types;
                if (value.kind == ReachabilityValueKind::Instance) {
                    types.insert(canonicalReachabilityType(value.type));
                } else if (value.kind == ReachabilityValueKind::InstanceSet) {
                    for (const auto& type : value.types) {
                        types.insert(canonicalReachabilityType(type));
                    }
                }
                return types;
            };
            auto recordCallArguments = [&](const FunctionSignature& target,
                                           size_t firstValue,
                                           size_t argumentCount,
                                           bool argumentsAvailable) {
                if (target.captureCount != 0) {
                    markUnknownFunctionParameters(target);
                    return;
                }
                for (size_t i = 0; i < target.parameterTypes.size(); ++i) {
                    if (!argumentsAvailable || i >= argumentCount) {
                        recordParameterTypes(target, i, {}, true);
                        continue;
                    }
                    const auto types = instanceTypesFor(valueStack[firstValue + i]);
                    for (const auto& type : types) {
                        noteLiveClassType(type);
                    }
                    recordParameterTypes(target, i, types, types.empty());
                }
            };
            auto readSlotIndex = [&](const IRType::IRInstr& ins,
                                     size_t argIndex,
                                     int64_t& index) {
                return tryArgInt(ins, argIndex, index) && index >= 0 &&
                    index <= std::numeric_limits<int>::max();
            };
            const bool isFunctionBody =
                functionSignaturesByChunk.find(ci) !=
                functionSignaturesByChunk.end();
            for (size_t instructionIndex = 0;
                 instructionIndex < program[ci].size();
                 ++instructionIndex) {
                const auto& ins = program[ci][instructionIndex];
                ReachabilityValue preciseMethodReturn;
                std::string targetName;
                bool hasResolvedDirectCall = readDirectCallAt(
                    program[ci], instructionIndex, targetName);
                if (hasResolvedDirectCall) {
                    addTarget(targetName);
                }

                if (isFunctionBody && ins.op == VMOperator::MAKE_FUNCTION) {
                    int64_t targetChunkIndex = -1;
                    if (readSlotIndex(ins, 0, targetChunkIndex)) {
                        const auto target = functionSignaturesByChunk.find(
                            static_cast<size_t>(targetChunkIndex));
                        if (target != functionSignaturesByChunk.end() &&
                            !target->second.isGenericTemplate) {
                            functionValueReferences.insert(target->second.name);
                            addTarget(target->second.name);
                            if (hasUnknownIndirectFunctionCall) {
                                markUnknownFunctionParameters(target->second);
                            }
                        }
                    }
                } else if (ins.op == VMOperator::LOAD_VALUE) {
                    const FunctionSignature* referencedFunction = nullptr;
                    if (ins.args.size() == 1 &&
                        std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& functionName =
                            std::get<std::string>(ins.args[0]);
                        if (dynamicVariadicABISymbol(functionName).empty()) {
                            referencedFunction = resolveFunctionSignature(
                                resolveCallableName(functionName));
                            if (referencedFunction == nullptr) {
                                referencedFunction =
                                    resolveFunctionSignatureByUniqueSuffix(functionName);
                            }
                        }
                    } else if (ins.args.size() >= 2 &&
                               std::holds_alternative<std::string>(ins.args[0]) &&
                               std::get<std::string>(ins.args[0]) == "global") {
                        int64_t globalIndex = -1;
                        if (readSlotIndex(ins, 1, globalIndex)) {
                            const auto global = functionGlobals.find(
                                static_cast<int>(globalIndex));
                            if (global != functionGlobals.end()) {
                                referencedFunction = resolveFunctionSignature(
                                    global->second);
                                if (referencedFunction == nullptr) {
                                    referencedFunction =
                                        resolveFunctionSignatureByUniqueSuffix(
                                            global->second);
                                }
                            }
                        }
                    }
                    if (referencedFunction != nullptr &&
                        referencedFunction->captureCount == 0) {
                        functionValueReferences.insert(referencedFunction->name);
                        if (hasUnknownIndirectFunctionCall) {
                            markUnknownFunctionParameters(*referencedFunction);
                        }
                        if (referencedFunction->isExternal) {
                            reachableExternalFunctions.insert(referencedFunction->name);
                        } else if (!referencedFunction->isGenericTemplate) {
                            addTarget(referencedFunction->name);
                        }
                    }
                }

                // A direct-call annotation is the authoritative result of
                // static operator resolution. The reachability stack is only
                // an approximation and can lose receiver types across control
                // flow; treating that as a dynamic subscript would pull every
                // class's operator into the module (including unrelated
                // built-in string setters).
                if (ins.op == VMOperator::DICT_LOAD_VALUE &&
                    !hasResolvedDirectCall) {
                    if (valueStack.size() >= 2) {
                        const auto ownerTypes = instanceTypesFor(
                            valueStack[valueStack.size() - 2]);
                        if (!ownerTypes.empty()) {
                            subscriptLoadOwnerTypes.insert(
                                ownerTypes.begin(), ownerTypes.end());
                        } else {
                            usesDynamicSubscriptLoad = true;
                            dynamicMemberMethodNames.insert("operator[[]]");
                        }
                    } else {
                        usesDynamicSubscriptLoad = true;
                        dynamicMemberMethodNames.insert("operator[[]]");
                    }
                }
                if (ins.op == VMOperator::DICT_STORE_VALUE &&
                    !hasResolvedDirectCall) {
                    if (valueStack.size() >= 3) {
                        const auto ownerTypes = instanceTypesFor(
                            valueStack[valueStack.size() - 3]);
                        if (!ownerTypes.empty()) {
                            subscriptStoreOwnerTypes.insert(
                                ownerTypes.begin(), ownerTypes.end());
                        } else {
                            usesDynamicSubscriptStore = true;
                            dynamicMemberMethodNames.insert("operator[[]=]");
                        }
                    } else {
                        usesDynamicSubscriptStore = true;
                        dynamicMemberMethodNames.insert("operator[[]=]");
                    }
                }
                if ((ins.op == VMOperator::OBJ_CALL_METHOD ||
                     ins.op == VMOperator::OBJ_CALL_SAFE) && !ins.args.empty() &&
                    std::holds_alternative<std::string>(ins.args.front())) {
                    const std::string method = std::get<std::string>(ins.args.front());
                    bool resolvedTypedReceiver = false;
                    auto recordResolvedMethod = [&](const FunctionSignature& candidate) {
                        if (candidate.isGenericTemplate) return;
                        addTarget(candidate.name);
                        resolvedTypedReceiver = true;
                    };
                    auto setPreciseMethodReturn = [&](const std::string& type) {
                        const std::string returnType =
                            canonicalReachabilityType(type);
                        noteLiveClassType(returnType);
                        if (classLayouts.find(returnType) != classLayouts.end() ||
                            ZeusBuiltins::findNativeTypeABI(returnType) != nullptr ||
                            ZeusBuiltins::findTypeImplementation(returnType) != nullptr) {
                            preciseMethodReturn = {
                                ReachabilityValueKind::Instance, returnType
                            };
                        }
                    };
                    const bool safeNavigation = ins.op == VMOperator::OBJ_CALL_SAFE;
                    // Safe navigation always lowers to the dynamic member
                    // dispatcher, even when the receiver's static class is
                    // known. The registry entry must therefore exist for every
                    // `?.` call, not only for receivers whose type is unknown.
                    if (safeNavigation) {
                        dynamicMemberMethodNames.insert(method);
                    }
                    const FunctionSignature* directMethod = !safeNavigation &&
                        hasResolvedDirectCall
                        ? resolveFunctionSignature(targetName) : nullptr;
                    if (directMethod != nullptr) {
                        recordResolvedMethod(*directMethod);
                        setPreciseMethodReturn(directMethod->returnType);
                    }

                    int64_t argc = -1;
                    const bool hasArgumentCount =
                        readSlotIndex(ins, 1, argc) && argc >= 0;
                    const bool callArgumentsAvailable = hasArgumentCount &&
                        static_cast<uint64_t>(argc) < valueStack.size();
                    const size_t firstCallArgument = callArgumentsAvailable
                        ? valueStack.size() - static_cast<size_t>(argc) - 1
                        : 0;
                    if (directMethod != nullptr) {
                        recordCallArguments(
                            *directMethod, firstCallArgument,
                            hasArgumentCount
                                ? static_cast<size_t>(argc) + 1 : 0,
                            callArgumentsAvailable);
                    }

                    bool receiverTypesKnown = false;
                    if (!resolvedTypedReceiver && hasArgumentCount &&
                        callArgumentsAvailable) {
                        const ReachabilityValue& receiver = valueStack[
                            valueStack.size() - static_cast<size_t>(argc) - 1];
                        if (receiver.kind == ReachabilityValueKind::Instance) {
                            const std::string ownerType =
                                canonicalReachabilityType(receiver.type);
                            bool hasReturnType = false;
                            bool returnTypesAgree = true;
                            std::string returnType;
                            for (const auto& candidateEntry : functionSignatures) {
                                const FunctionSignature& candidate =
                                    candidateEntry.second;
                                if (canonicalReachabilityType(candidate.ownerType) !=
                                        ownerType ||
                                    methodNameForSignature(candidate) != method ||
                                    candidate.isStaticMethod ||
                                    candidate.isGenericTemplate) {
                                    continue;
                                }
                                recordResolvedMethod(candidate);
                                if (!hasResolvedDirectCall) {
                                    recordCallArguments(
                                        candidate, firstCallArgument,
                                        static_cast<size_t>(argc) + 1, true);
                                }
                                const std::string candidateReturn =
                                    canonicalReachabilityType(candidate.returnType);
                                if (!hasReturnType) {
                                    returnType = std::move(candidateReturn);
                                    hasReturnType = true;
                                } else if (returnType != candidateReturn) {
                                    returnTypesAgree = false;
                                }
                            }
                            if (resolvedTypedReceiver && hasReturnType &&
                                returnTypesAgree) {
                                const std::string canonicalReturnType =
                                    canonicalReachabilityType(returnType);
                                if (classLayouts.find(canonicalReturnType) !=
                                        classLayouts.end() ||
                                    ZeusBuiltins::findNativeTypeABI(
                                        canonicalReturnType) != nullptr ||
                                    ZeusBuiltins::findTypeImplementation(
                                        canonicalReturnType) != nullptr) {
                                    preciseMethodReturn = {
                                        ReachabilityValueKind::Instance,
                                        canonicalReturnType
                                    };
                                }
                            }
                        } else if (receiver.kind ==
                                   ReachabilityValueKind::InstanceSet) {
                            receiverTypesKnown = true;
                            for (const auto& ownerType : instanceTypesFor(receiver)) {
                                for (const auto& candidateEntry : functionSignatures) {
                                    const FunctionSignature& candidate =
                                        candidateEntry.second;
                                    if (canonicalReachabilityType(candidate.ownerType) !=
                                            ownerType ||
                                        methodNameForSignature(candidate) != method ||
                                        candidate.isStaticMethod ||
                                        candidate.isGenericTemplate) {
                                        continue;
                                    }
                                    recordResolvedMethod(candidate);
                                    if (!hasResolvedDirectCall) {
                                        recordCallArguments(
                                            candidate, firstCallArgument,
                                            static_cast<size_t>(argc) + 1, true);
                                    }
                                }
                            }
                        } else if (receiver.kind == ReachabilityValueKind::Class) {
                            const FunctionSignature* staticMethod =
                                findStaticClassFunction(receiver.type, method);
                            if (staticMethod != nullptr) {
                                recordResolvedMethod(*staticMethod);
                                setPreciseMethodReturn(staticMethod->returnType);
                            }
                        } else if (receiver.kind == ReachabilityValueKind::Namespace) {
                            const FunctionSignature* namespaceFunction =
                                findNamespaceFunction(receiver.type, method);
                            if (namespaceFunction != nullptr) {
                                recordResolvedMethod(*namespaceFunction);
                                setPreciseMethodReturn(namespaceFunction->returnType);
                            }
                        }
                    }
                    if (!hasResolvedDirectCall) {
                        dynamicMemberMethodNames.insert(method);
                    }
                    const bool withOperatorCall =
                        method == "operator[with]" ||
                        method == "operator[~with]";
                    if (withOperatorCall ||
                        (!resolvedTypedReceiver && !receiverTypesKnown)) {
                        // `with` is lowered to protected member calls before
                        // this reachability pass. Keep both operator methods
                        // reachable for the dynamic call path used by AOT.
                        methodCalls.push_back(method);
                    }
                }
                if (ins.op == VMOperator::WITH_BEGIN) {
                    methodCalls.push_back("operator[with]");
                    methodCalls.push_back("operator[~with]");
                }

                switch (ins.op) {
                    case VMOperator::PUSH:
                    case VMOperator::LOAD_CELL_REFERENCE:
                        pushUnknown();
                        break;

                    case VMOperator::LOAD_NATIVE: {
                        int64_t encodedHandle = -1;
                        std::string nativeName;
                        if (tryArgInt(ins, 0, encodedHandle) &&
                            resolveNativeHandle(encodedHandle, nativeName)) {
                            const FunctionSignature* nativeDeclaration =
                                findNativeBuiltinDeclaration(nativeName);
                            if (nativeDeclaration != nullptr) {
                                valueStack.push_back({
                                    ReachabilityValueKind::Function,
                                    nativeDeclaration->name
                                });
                                break;
                            }
                        }
                        pushUnknown();
                        break;
                    }

                    case VMOperator::MAKE_FUNCTION: {
                        int64_t targetChunkIndex = -1;
                        if (readSlotIndex(ins, 0, targetChunkIndex)) {
                            const auto target = functionSignaturesByChunk.find(
                                static_cast<size_t>(targetChunkIndex));
                            if (target != functionSignaturesByChunk.end() &&
                                !target->second.isGenericTemplate &&
                                target->second.captureCount == 0) {
                                valueStack.push_back({
                                    ReachabilityValueKind::Function,
                                    target->second.name
                                });
                                break;
                            }
                        }
                        pushUnknown();
                        break;
                    }

                    case VMOperator::LOAD_VALUE: {
                        ReachabilityValue value;
                        if (ins.metadata.symbol.kind ==
                                IRType::SymbolRefKind::Class &&
                            !ins.metadata.symbol.name.empty()) {
                            value = {
                                ReachabilityValueKind::Class,
                                ins.metadata.symbol.name
                            };
                        } else if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            const std::string& kind = std::get<std::string>(ins.args[0]);
                            int64_t index = -1;
                            if (kind == "global" && readSlotIndex(ins, 1, index)) {
                                const auto nameSpace = namespacePackagesByGlobal.find(
                                    static_cast<int>(index));
                                if (nameSpace != namespacePackagesByGlobal.end()) {
                                    value = { ReachabilityValueKind::Namespace,
                                              nameSpace->second };
                                } else {
                                    const auto classGlobal = classGlobals.find(
                                        static_cast<int>(index));
                                    if (classGlobal != classGlobals.end()) {
                                        value = { ReachabilityValueKind::Class,
                                                  classGlobal->second };
                                    } else {
                                        const auto global = globalValues.find(
                                            static_cast<int>(index));
                                        if (global != globalValues.end()) {
                                            value = global->second;
                                        }
                                    }
                                }
                            } else if (kind == "local" && readSlotIndex(ins, 1, index)) {
                                const auto local = localValues.find(static_cast<int>(index));
                                if (local != localValues.end()) value = local->second;
                            } else if (kind != "local" && kind != "global" &&
                                       kind != "upvalue" &&
                                       classLayouts.find(kind) != classLayouts.end()) {
                                value = { ReachabilityValueKind::Class, kind };
                            }
                        }
                        if (value.kind == ReachabilityValueKind::Untracked) {
                            const FunctionSignature* functionValue = nullptr;
                            if (ins.args.size() == 1 &&
                                std::holds_alternative<std::string>(ins.args[0])) {
                                const std::string& functionName =
                                    std::get<std::string>(ins.args[0]);
                                if (!dynamicVariadicABISymbol(functionName).empty()) {
                                    value = {
                                        ReachabilityValueKind::Function,
                                        functionName
                                    };
                                } else {
                                    functionValue = resolveFunctionSignature(
                                        resolveCallableName(functionName));
                                    if (functionValue == nullptr) {
                                        functionValue =
                                            resolveFunctionSignatureByUniqueSuffix(
                                                functionName);
                                    }
                                }
                            } else if (ins.args.size() >= 2 &&
                                       std::holds_alternative<std::string>(ins.args[0]) &&
                                       std::get<std::string>(ins.args[0]) == "global") {
                                int64_t globalIndex = -1;
                                if (readSlotIndex(ins, 1, globalIndex)) {
                                    const auto global = functionGlobals.find(
                                        static_cast<int>(globalIndex));
                                    if (global != functionGlobals.end()) {
                                        functionValue = resolveFunctionSignature(
                                            global->second);
                                        if (functionValue == nullptr) {
                                            functionValue =
                                                resolveFunctionSignatureByUniqueSuffix(
                                                    global->second);
                                        }
                                    }
                                }
                            }
                            if (functionValue != nullptr &&
                                functionValue->captureCount == 0) {
                                value = {
                                    ReachabilityValueKind::Function,
                                    functionValue->name
                                };
                            }
                        }
                        valueStack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::OBJ_LOAD_ATTR:
                    case VMOperator::OBJ_LOAD_SAFE_ATTR: {
                        const ReachabilityValue receiver = popValue();
                        std::string member;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            member = std::get<std::string>(ins.args[0]);
                        }
                        ReachabilityValue value;
                        if (receiver.kind == ReachabilityValueKind::Namespace) {
                            const auto exports = namespaceExportsByPackage.find(receiver.type);
                            if (exports != namespaceExportsByPackage.end() &&
                                exports->second.find(member) != exports->second.end() &&
                                classLayouts.find(member) != classLayouts.end()) {
                                value = { ReachabilityValueKind::Class, member };
                            } else if (const FunctionSignature* function =
                                           findNamespaceFunction(receiver.type, member);
                                       function != nullptr &&
                                       function->captureCount == 0) {
                                functionValueReferences.insert(function->name);
                                addTarget(function->name);
                                if (hasUnknownIndirectFunctionCall) {
                                    markUnknownFunctionParameters(*function);
                                }
                                value = {
                                    ReachabilityValueKind::Function,
                                    function->name
                                };
                            }
                        } else if (receiver.kind == ReachabilityValueKind::Class) {
                            const FunctionSignature* function =
                                findStaticClassFunction(receiver.type, member);
                            if (function != nullptr &&
                                function->captureCount == 0) {
                                functionValueReferences.insert(function->name);
                                addTarget(function->name);
                                if (hasUnknownIndirectFunctionCall) {
                                    markUnknownFunctionParameters(*function);
                                }
                                value = {
                                    ReachabilityValueKind::Function,
                                    function->name
                                };
                            }
                        } else if (receiver.kind == ReachabilityValueKind::Instance) {
                            const ClassLayout* layout = findClassLayout(receiver.type);
                            bool hasField = false;
                            if (layout != nullptr) {
                                const int field = findFieldIndex(*layout, member);
                                if (field >= 0 && static_cast<size_t>(field) <
                                    layout->fieldTypes.size()) {
                                    hasField = true;
                                    const std::string& type = layout->fieldTypes[
                                        static_cast<size_t>(field)];
                                    const std::string reachabilityType =
                                        canonicalReachabilityType(type);
                                    noteLiveClassType(reachabilityType);
                                    if (classLayouts.find(reachabilityType) !=
                                            classLayouts.end() ||
                                        ZeusBuiltins::findNativeTypeABI(
                                            reachabilityType) != nullptr ||
                                        ZeusBuiltins::findTypeImplementation(
                                            reachabilityType) != nullptr) {
                                        value = {
                                            ReachabilityValueKind::Instance,
                                            reachabilityType
                                        };
                                    }
                                }
                            }
                            if (!hasField) {
                                const auto method =
                                    functionSignatures.find(receiver.type + "::" + member);
                                if (method != functionSignatures.end() &&
                                    method->second.ownerType == receiver.type &&
                                    !method->second.isStaticMethod &&
                                    !method->second.parameterTypes.empty() &&
                                    (method->second.parameterTypes.front() == receiver.type ||
                                     method->second.parameterTypes.front() == "&" + receiver.type)) {
                                    dynamicMemberMethodNames.insert(member);
                                    addTarget(method->second.name);
                                }
                            }
                        } else if (receiver.kind == ReachabilityValueKind::Untracked ||
                                   receiver.kind ==
                                       ReachabilityValueKind::InstanceSet) {
                            for (const auto& candidateEntry : functionSignatures) {
                                const FunctionSignature& method = candidateEntry.second;
                                if (methodNameForSignature(method) != member ||
                                    method.ownerType.empty() ||
                                    method.isStaticMethod ||
                                    method.parameterTypes.empty() ||
                                    (method.parameterTypes.front() != method.ownerType &&
                                     method.parameterTypes.front() != "&" + method.ownerType)) {
                                    continue;
                                }
                                dynamicMemberMethodNames.insert(member);
                                addTarget(method.name);
                            }
                        }
                        valueStack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::LLVM_ADDR:
                        if (ins.args.size() >= 3 &&
                            std::holds_alternative<std::string>(ins.args[0]) &&
                            std::get<std::string>(ins.args[0]) == "function" &&
                            std::holds_alternative<std::string>(ins.args[2])) {
                            if (const auto* target = resolveAddressableFunctionSignature(
                                    std::get<std::string>(ins.args[2]));
                                target != nullptr && !target->nativeSymbol.empty() &&
                                target->captureCount == 0 && !target->isGenericTemplate) {
                                functionValueReferences.insert(target->name);
                                addTarget(target->name);
                            }
                        }
                        pushUnknown();
                        break;

                    case VMOperator::LLVM_SIZEOF:
                        pushUnknown();
                        break;

                    case VMOperator::LLVM_FADDR:
                        popValues(ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1]) &&
                            std::get<std::string>(ins.args[1]) == "length" ? 2 : 1);
                        pushUnknown();
                        break;

                    case VMOperator::LLVM_FSTORE:
                        popValues(2);
                        break;

                    case VMOperator::OBJ_BUILD:
                    case VMOperator::OBJ_BUILD_SHARED:
                    case VMOperator::NEW_OBJ: {
                        int64_t argc = 0;
                        if (!readSlotIndex(ins, 0, argc)) {
                            valueStack.clear();
                            break;
                        }
                        popValues(argc);
                        ReachabilityValue classValue;
                        std::string targetType;
                        if (ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            targetType = std::get<std::string>(ins.args[1]);
                        }
                        std::string genericClassName;
                        std::vector<std::string> genericTypeArguments;
                        std::string specializedClassName;
                        const bool hasSpecializedClass = targetType.empty() &&
                            ins.metadata.readClassInstantiation(
                                genericClassName, genericTypeArguments,
                                &specializedClassName) &&
                            !specializedClassName.empty();
                        if (targetType.empty()) classValue = popValue();
                        const std::string type = !targetType.empty()
                            ? targetType
                            : hasSpecializedClass
                                ? specializedClassName
                                : classValue.kind == ReachabilityValueKind::Class
                                    ? classValue.type : std::string();
                        if (targetType.empty() && !hasSpecializedClass &&
                            classValue.kind != ReachabilityValueKind::Class) {
                            usesDynamicClassConstruction = true;
                            programHasDynamicClassConstruction = true;
                        }
                        if (!type.empty() &&
                            (classLayouts.find(type) != classLayouts.end() ||
                             findNewOperatorSignature(type) != nullptr)) {
                            noteLiveClassType(type);
                            valueStack.push_back({ ReachabilityValueKind::Instance, type });
                        } else {
                            pushUnknown();
                        }
                        break;
                    }

                    case VMOperator::STORE_VALUE_GLOBAL:
                    case VMOperator::ASSIGN_VALUE_GLOBAL:
                    case VMOperator::STORE_VALUE_LOCAL:
                    case VMOperator::ASSIGN_VALUE_LOCAL: {
                        int64_t index = -1;
                        if (!readSlotIndex(ins, 0, index)) {
                            popValue();
                            break;
                        }
                        const ReachabilityValue value = popValue();
                        auto& slots = (ins.op == VMOperator::STORE_VALUE_GLOBAL ||
                                       ins.op == VMOperator::ASSIGN_VALUE_GLOBAL)
                            ? globalValues : localValues;
                        if (value.kind == ReachabilityValueKind::Instance ||
                            value.kind == ReachabilityValueKind::InstanceSet ||
                            value.kind == ReachabilityValueKind::Function) {
                            slots[static_cast<int>(index)] = value;
                        } else if ((ins.op == VMOperator::STORE_VALUE_LOCAL ||
                                    ins.op == VMOperator::ASSIGN_VALUE_LOCAL) &&
                                   value.kind == ReachabilityValueKind::Untracked) {
                            // Default-parameter prologues store nil before the
                            // body runs. The annotation still constrains every
                            // value the slot can receive, so keep its instance
                            // set instead of forgetting the parameter type.
                            const auto declared = declaredParameterValues.find(
                                static_cast<int>(index));
                            if (declared != declaredParameterValues.end()) {
                                slots[static_cast<int>(index)] = declared->second;
                            } else {
                                slots.erase(static_cast<int>(index));
                            }
                        } else {
                            slots.erase(static_cast<int>(index));
                        }
                        break;
                    }

                    case VMOperator::DELETE_VAR: {
                        if (ins.args.size() >= 3 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            int64_t index = -1;
                            if (readSlotIndex(ins, 2, index)) {
                                const std::string& kind = std::get<std::string>(ins.args[1]);
                                auto& slots = kind == "global" ? globalValues : localValues;
                                const auto variable = slots.find(static_cast<int>(index));
                                if (variable != slots.end()) {
                                    const auto types = instanceTypesFor(variable->second);
                                    deletedTypes.insert(types.begin(), types.end());
                                }
                                slots.erase(static_cast<int>(index));
                            }
                        }
                        break;
                    }

                    case VMOperator::OBJ_CALL_METHOD:
                    case VMOperator::OBJ_CALL_SAFE: {
                        int64_t argc = 0;
                        if (tryArgInt(ins, 1, argc) && argc >= 0) {
                            popValues(argc + 1);
                            valueStack.push_back(std::move(preciseMethodReturn));
                        } else {
                            valueStack.clear();
                        }
                        break;
                    }

                    case VMOperator::CALL_NATIVE:
                    case VMOperator::CALL_CLOSURE: {
                        int64_t argc = 0;
                        if (readSlotIndex(ins, 0, argc)) {
                            ReachabilityValue callReturn;
                            std::string directCallTarget = targetName;
                            bool hasKnownCallTarget =
                                ins.op == VMOperator::CALL_CLOSURE &&
                                hasResolvedDirectCall;
                            if (ins.op == VMOperator::CALL_CLOSURE &&
                                !hasKnownCallTarget &&
                                static_cast<uint64_t>(argc) < valueStack.size()) {
                                const ReachabilityValue& calleeValue = valueStack[
                                    valueStack.size() -
                                    static_cast<size_t>(argc) - 1];
                                if (calleeValue.kind ==
                                    ReachabilityValueKind::Function) {
                                    directCallTarget = calleeValue.type;
                                    hasKnownCallTarget = true;
                                }
                            }
                            if (ins.op == VMOperator::CALL_CLOSURE &&
                                !hasKnownCallTarget) {
                                hasUnknownIndirectFunctionCall = true;
                                markReachableFunctionsUnknown();
                            }
                            if (hasKnownCallTarget) {
                                const FunctionSignature* called =
                                    resolveFunctionSignature(directCallTarget);
                                if (called != nullptr) {
                                    const bool argumentsAvailable =
                                        static_cast<uint64_t>(argc) + 1 <=
                                            valueStack.size();
                                    const size_t firstArgument = argumentsAvailable
                                        ? valueStack.size() -
                                            static_cast<size_t>(argc)
                                        : 0;
                                    recordCallArguments(
                                        *called, firstArgument,
                                        static_cast<size_t>(argc),
                                        argumentsAvailable);
                                    const std::string returnType =
                                        canonicalReachabilityType(called->returnType);
                                    if (classLayouts.find(returnType) !=
                                            classLayouts.end() ||
                                        ZeusBuiltins::findNativeTypeABI(returnType) !=
                                            nullptr ||
                                        ZeusBuiltins::findTypeImplementation(returnType) !=
                                            nullptr) {
                                        noteLiveClassType(returnType);
                                        callReturn = {
                                            ReachabilityValueKind::Instance,
                                            returnType
                                        };
                                    }
                                }
                            }
                            // A resolved direct-call target still has its callee
                            // value in the VM operand stack.
                            popValues(argc + 1);
                            valueStack.push_back(std::move(callReturn));
                        } else {
                            valueStack.clear();
                        }
                        break;
                    }

                    case VMOperator::LIST_BUILD:
                    case VMOperator::DICT_BUILD: {
                        int64_t count = 0;
                        const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                        if (readSlotIndex(ins, 0, count) &&
                            (!dictionary || count <=
                                std::numeric_limits<int64_t>::max() / 2)) {
                            popValues(count * (dictionary ? 2 : 1));
                            valueStack.push_back({
                                ReachabilityValueKind::Instance,
                                dictionary ? "dict" : "Array"
                            });
                        } else {
                            valueStack.clear();
                        }
                        break;
                    }

                    case VMOperator::LIST_LENGTH:
                    case VMOperator::DICT_KEYS_BUILD:
                        popValue();
                        pushUnknown();
                        break;

                    case VMOperator::LIST_LOAD_ELEM:
                    case VMOperator::DICT_LOAD_VALUE:
                        popValues(2);
                        pushUnknown();
                        break;

                    case VMOperator::LIST_STORE_ELEM:
                    case VMOperator::DICT_STORE_VALUE:
                        popValues(3);
                        break;

                    case VMOperator::OBJ_STORE_ATTR: {
                        popValue();
                        const ReachabilityValue receiver = popValue();
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            const std::string& name = std::get<std::string>(ins.args[0]);
                            if (receiver.kind == ReachabilityValueKind::Instance) {
                                const ClassLayout* layout = findClassLayout(receiver.type);
                                if (layout != nullptr && findFieldIndex(*layout, name) < 0) {
                                    inferredFields[receiver.type].insert(name);
                                }
                            } else if (receiver.kind == ReachabilityValueKind::Untracked ||
                                       receiver.kind ==
                                           ReachabilityValueKind::InstanceSet) {
                                // An untyped/unknown receiver can have any class at
                                // runtime. Route every access to this field through
                                // the dynamic field ABI so later writes can change
                                // its type or add it without invalidating the native
                                // class layout.
                                dynamicFieldStores.insert(name);
                            }
                        }
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
                        popValues(2);
                        pushUnknown();
                        break;

                    case VMOperator::POP:
                    case VMOperator::WITH_BEGIN:
                        popValue();
                        break;

                    case VMOperator::WITH_END:
                    case VMOperator::IR_ANNOTATION:
                        break;

                    case VMOperator::CLEAR:
                        valueStack.clear();
                        break;

                    case VMOperator::COPY:
                        valueStack.push_back(valueStack.empty()
                            ? ReachabilityValue{} : valueStack.back());
                        break;

                    case VMOperator::SWAP:
                        if (valueStack.size() >= 2) {
                            std::iter_swap(valueStack.end() - 1, valueStack.end() - 2);
                        } else {
                            valueStack.clear();
                        }
                        break;

                    case VMOperator::JUMP_IF_TRUE:
                    case VMOperator::JUMP_IF_FALSE:
                    case VMOperator::COUNT_CHECK_NEG:
                    case VMOperator::COUNT_CHECK_STEP_ZERO:
                        popValue();
                        break;

                    default:
                        valueStack.clear();
                        break;
                }
            }

            if (usesDynamicClassConstruction) {
                for (const auto& layout : classLayouts) {
                    constructedTypes.insert(layout.first);
                }
            }

            // Copy helpers and the call graph must select the same @= hook.
            // A hook is a root only when its class copy callback is emitted.
            for (const auto& entry : classLayouts) {
                const std::string& className = entry.first;
                if (!classNeedsRuntimeMachinery(className)) {
                    continue;
                }
                const FunctionSignature* hook = findClassCopyHook(className);
                if (hook == nullptr) continue;
                if (allowedFunctionSourceFileIds != nullptr) {
                    const size_t chunkIndex = hook->chunkIndex;
                    if (chunkIndex >= program.size() || program[chunkIndex].empty() ||
                        allowedFunctionSourceFileIds->find(
                            program[chunkIndex].front().sourceFileId) ==
                            allowedFunctionSourceFileIds->end()) {
                        continue;
                    }
                }
                if (seededClassCopyHooks.insert(hook->name).second) {
                    addTarget(hook->name);
                }
            }

            // Implicit stringification path: print()/str() resolve toString by
            // name at runtime, so the call graph above cannot reach these
            // impls; seed them from the classes this chunk (or the whole
            // program, when construction is dynamic) can instantiate.
            if (programCallsPrintOrStr) {
                for (const auto& type : constructedTypes) {
                    seedToStringFor(type);
                }
            }

            for (const auto& method : methodCalls) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if (!isReachabilityManagedFunction(candidate) ||
                        methodNameForSignature(candidate) != method) {
                        continue;
                    }
                    if (method == "toString") {
                        const auto* type = ZeusBuiltins::findTypeImplementation(
                            candidate.ownerType);
                        if (type == nullptr || !type->dynamicStringification) {
                            continue;
                        }
                    }
                    addTarget(candidate.name);
                    markUnknownFunctionParameters(candidate);
                }
            }

            auto addLifecycleFunctions = [&](const std::set<std::string>& types,
                                             bool constructors,
                                             bool destructors) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if (!isReachabilityManagedFunction(candidate) ||
                        types.find(candidate.ownerType) == types.end()) {
                        continue;
                    }
                    if ((constructors &&
                         (candidate.name == candidate.ownerType + "::init" ||
                          candidate.operatorSymbol == "new")) ||
                        (destructors && candidate.operatorSymbol == "~")) {
                        addTarget(candidate.name);
                    }
                }
            };
            addLifecycleFunctions(constructedTypes, true, false);
            for (const auto& type : constructedTypes) {
                const auto layout = classLayouts.find(type);
                if (layout == classLayouts.end() ||
                    findNewOperatorSignature(type) != nullptr) {
                    continue;
                }
                for (const auto& initializerName :
                     layout->second.fieldInitializerNames) {
                    if (!initializerName.empty()) addTarget(initializerName);
                }
            }
            addLifecycleFunctions(constructedTypes, false, true);
            addLifecycleFunctions(deletedTypes, false, true);

            for (const auto& function : functionSignatures) {
                const auto& candidate = function.second;
                const bool isReachableSubscriptLoad =
                    candidate.operatorSymbol == "[]" &&
                    (usesDynamicSubscriptLoad ||
                     subscriptLoadOwnerTypes.find(candidate.ownerType) !=
                         subscriptLoadOwnerTypes.end());
                const bool isReachableSubscriptStore =
                    candidate.operatorSymbol == "[]=" &&
                    (usesDynamicSubscriptStore ||
                     subscriptStoreOwnerTypes.find(candidate.ownerType) !=
                         subscriptStoreOwnerTypes.end());
                if (isReachableSubscriptLoad || isReachableSubscriptStore) {
                    addTarget(candidate.name);
                }
            }
        }

        for (const auto& entry : inferredFields) {
            ClassLayout& layout = classLayouts.at(entry.first);
            if (layout.fieldTypeExplicit.empty()) {
                layout.fieldTypeExplicit.resize(layout.fieldNames.size(), false);
            }
            for (const std::string& name : entry.second) {
                if (findFieldIndex(layout, name) >= 0) continue;
                const size_t fieldIndex = layout.fieldNames.size();
                layout.fieldNames.push_back(name);
                layout.fieldTypes.emplace_back(); // Unannotated Zeus value.
                layout.fieldTypeExplicit.push_back(false);
                layout.fieldConstness.push_back(false);
                IRType::ClassFieldDefault missingValue;
                missingValue.kind = IRType::ClassFieldDefaultKind::Null;
                layout.fieldDefaults.push_back(missingValue);
                layout.fieldNames.push_back("$initialized:" + name);
                layout.fieldTypes.push_back("bool");
                layout.fieldTypeExplicit.push_back(false);
                layout.fieldConstness.push_back(false);
                layout.fieldDefaults.emplace_back(); // Zero / false at construction.
                if (!layout.fieldInitializerNames.empty()) {
                    // Keep the parallel metadata in sync: inferred fields
                    // have no initializer function. The OBJ_BUILD layout
                    // check requires identical lengths once the vector is
                    // non-empty.
                    layout.fieldInitializerNames.emplace_back();
                    layout.fieldInitializerNames.emplace_back();
                }
                layout.inferredFieldFlags[name] = fieldIndex + 1;
            }
        }

    }

    bool buildDynamicMemberBridge() {
        std::vector<DynamicMemberBridge::MethodBinding> bindings;
        std::set<std::pair<std::string, std::string>> registeredMethods;

        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            const std::string methodName = methodNameForSignature(signature);
            const bool hasNativeOwnerABI =
                ZeusBuiltins::findNativeTypeABI(signature.ownerType) != nullptr;
            const std::string expectedOwnerRef = "&" + signature.ownerType;
            if (methodName.empty() ||
                dynamicMemberMethodNames.find(methodName) == dynamicMemberMethodNames.end() ||
                signature.isStaticMethod ||
                signature.parameterTypes.empty() ||
                (signature.parameterTypes.front() != signature.ownerType &&
                 signature.parameterTypes.front() != expectedOwnerRef) ||
                (!isClassType(signature.ownerType) && !hasNativeOwnerABI)) {
                continue;
            }

            if (!signature.isExternal &&
                reachableFunctionChunks.find(signature.chunkIndex) ==
                    reachableFunctionChunks.end()) {
                continue;
            }
            if (signature.isExternal) {
                reachableExternalFunctions.insert(signature.name);
            }

            const auto key = std::make_pair(signature.ownerType, methodName);
            if (!registeredMethods.insert(key).second) {
                result.error = "[emit-llvm] 动态成员调用存在重载歧义 " +
                    signature.ownerType + "::" + methodName;
                return false;
            }

            bindings.push_back(DynamicMemberBridge::MethodBinding{
                signature.ownerType,
                methodName,
                signature.name,
                signature.returnType,
                signature.parameterTypes,
                static_cast<size_t>(std::max(0, signature.minRequiredParams - 1)),
                signature.hasDefaultParameters()
            });
        }

        DynamicMemberBridge::TypeRules rules;
        rules.llvmType = [this](const std::string& typeName) {
            return llvmType(typeName);
        };
        rules.parameterABIType = [this](const std::string& typeName) {
            return functionParameterABIType(typeName);
        };
        rules.usesZeusValueABI = [this](const std::string& typeName) {
            return usesZeusValueABI(typeName);
        };
        rules.isClassType = [this](const std::string& typeName) {
            return isClassType(typeName);
        };
        rules.callTarget = [this](const std::string& name) {
            return llvmCallTarget(name);
        };

        dynamicMemberBridge = DynamicMemberBridge::generate(bindings, rules);
        if (!dynamicMemberBridge.error.empty()) {
            result.error = "[emit-llvm] " + dynamicMemberBridge.error;
            return false;
        }
        return true;
    }

    bool buildDynamicFieldBridge() {
        std::map<std::pair<std::string, std::string>, DynamicFieldBridge::FieldBinding>
            bindingMap;
        std::set<std::string> fieldNames = dynamicFieldLoads;
        fieldNames.insert(dynamicFieldStores.begin(), dynamicFieldStores.end());
        for (const std::string& fieldName : fieldNames) {
            for (const auto& entry : classLayouts) {
                if (reachableClassLayouts.find(entry.first) ==
                    reachableClassLayouts.end()) {
                    continue;
                }
                const ClassLayout& layout = entry.second;
                const int fieldIndex = findFieldIndex(layout, fieldName);
                if (fieldIndex < 0 ||
                    static_cast<size_t>(fieldIndex) >= layout.fieldTypes.size()) {
                    continue;
                }

                DynamicFieldBridge::FieldBinding binding;
                binding.typeName = entry.first;
                binding.fieldName = fieldName;
                binding.fieldTypeName =
                    layout.fieldTypes[static_cast<size_t>(fieldIndex)];
                binding.fieldIndex = static_cast<size_t>(fieldIndex);
                binding.typeIsExplicit =
                    static_cast<size_t>(fieldIndex) <
                        layout.fieldTypeExplicit.size() &&
                    layout.fieldTypeExplicit[static_cast<size_t>(fieldIndex)];
                binding.hasSetter = dynamicFieldStores.find(fieldName) !=
                    dynamicFieldStores.end();
                binding.hasNativeSetter = binding.typeIsExplicit &&
                    supportsDynamicTypedFieldSetter(binding.fieldTypeName);
                binding.writable = static_cast<size_t>(fieldIndex) >=
                        layout.fieldConstness.size() ||
                    !layout.fieldConstness[static_cast<size_t>(fieldIndex)];
                const auto initializedFlag = layout.inferredFieldFlags.find(fieldName);
                if (initializedFlag != layout.inferredFieldFlags.end()) {
                    binding.hasInitializationFlag = true;
                    binding.initializationFlagIndex = initializedFlag->second;
                }
                binding.registerDynamic = true;
                bindingMap[std::make_pair(entry.first, fieldName)] = std::move(binding);
            }
        }

        auto canTraceField = [this](const std::string& fieldTypeName) {
            if (usesZeusValueABI(fieldTypeName) || isClassType(fieldTypeName)) return true;
            if (!fieldTypeName.empty() && fieldTypeName.front() == '&') {
                const std::string targetType = fieldTypeName.substr(1);
                if (targetType == "int" ||
                    targetType == "int64" || targetType == "bool" ||
                    isClassType(targetType)) return true;
                return ZeusBuiltins::isNativePointerABI(
                    ZeusBuiltins::findNativeTypeABI(targetType));
            }
            const auto* abi = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
            return isClassType(fieldTypeName) ||
                (abi && (abi->valueABI == ZeusBuiltins::NativeValueABI::Int64 ||
                         abi->valueABI == ZeusBuiltins::NativeValueABI::Bool1 ||
                         ZeusBuiltins::isNativePointerABI(abi)));
        };

        // The collector must see every heap-bearing field in each LLVM class,
        // even when that field is never accessed through dynamic member syntax.
        // Classes without live instances have no fields to trace; skipping
        // them drops the field adapters for dead classes.
        for (const auto& entry : classLayouts) {
            if (!classNeedsRuntimeMachinery(entry.first)) {
                continue;
            }
            const ClassLayout& layout = entry.second;
            for (size_t fieldIndex = 0; fieldIndex < layout.fieldNames.size(); ++fieldIndex) {
                const std::string& fieldName = layout.fieldNames[fieldIndex];
                if (fieldName.rfind("$initialized:", 0) == 0 ||
                    fieldIndex >= layout.fieldTypes.size()) continue;
                const std::string& fieldTypeName = layout.fieldTypes[fieldIndex];
                if (!canTraceField(fieldTypeName)) continue;

                auto key = std::make_pair(entry.first, fieldName);
                auto found = bindingMap.find(key);
                if (found == bindingMap.end()) {
                    DynamicFieldBridge::FieldBinding binding;
                    binding.typeName = entry.first;
                    binding.fieldName = fieldName;
                    binding.fieldTypeName = fieldTypeName;
                    binding.fieldIndex = fieldIndex;
                    binding.registerDynamic = false;
                    bindingMap.emplace(std::move(key), std::move(binding));
                    found = bindingMap.find(std::make_pair(entry.first, fieldName));
                }
                found->second.registerForTracing = true;
                const auto initializedFlag = layout.inferredFieldFlags.find(fieldName);
                if (initializedFlag != layout.inferredFieldFlags.end()) {
                    found->second.hasInitializationFlag = true;
                    found->second.initializationFlagIndex = initializedFlag->second;
                }
            }
        }

        std::vector<DynamicFieldBridge::FieldBinding> bindings;
        bindings.reserve(bindingMap.size());
        for (auto& entry : bindingMap) bindings.push_back(std::move(entry.second));

        DynamicFieldBridge::TypeRules rules;
        rules.llvmFieldType = [this](const std::string& typeName) {
            return llvmFieldType(typeName);
        };
        rules.llvmStructType = [this](const std::string& typeName) {
            return llvmStructType(typeName);
        };
        rules.usesZeusValueABI = [this](const std::string& typeName) {
            return usesZeusValueABI(typeName);
        };
        rules.isClassType = [this](const std::string& typeName) {
            return isClassType(typeName);
        };

        dynamicFieldBridge = DynamicFieldBridge::generate(bindings, rules);
        if (!dynamicFieldBridge.error.empty()) {
            result.error = "[emit-llvm] " + dynamicFieldBridge.error;
            return false;
        }
        return true;
    }

