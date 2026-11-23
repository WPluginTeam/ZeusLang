// Emitter member definitions: entry cleanup helpers and global type prescan; included inside class Emitter.
    std::vector<std::string> entryExceptionCleanupCalls() {
        std::vector<std::string> calls;
        for (const auto& value : entryNativeStringOwners) {
            calls.push_back("call void @zeus_string_release(ptr " + value + ")");
        }
        // Shared state exists only when the program touches shared bindings;
        // otherwise this call alone pins the whole shared runtime.
        if (programUsesSharedValues) {
            calls.push_back("call void @zeus_shared_values_release()");
        }
        auto appendSlot = [&](const NativeSlot& slot, bool local) {
            if (local && slot.needsLValueUnregister) {
                calls.push_back("call void @zeus_lvalue_unregister(ptr " + slot.address + ")");
            }
            if (slot.addressStorageABI == "zeusvalue" && slot.type == "ptr") {
                calls.push_back("call void @zeus_value_release(ptr " +
                                slot.address + ")");
                return;
            }
            if (slot.type != "ptr" ||
                (!isClassType(slot.sourceType) && slot.sourceType != "bytes" &&
                 slot.sourceType != "string")) {
                return;
            }
            if (slot.sourceType == "string" && !slot.ownsNativeString) {
                return;
            }
            const std::string value = freshTmp();
            calls.push_back(value + " = load ptr, ptr " + slot.address);
            if (slot.sourceType == "bytes") {
                calls.push_back("call void @zeus_bytes_release(ptr " + value + ")");
            } else if (slot.sourceType == "string") {
                calls.push_back("call void @zeus_string_release(ptr " + value + ")");
            } else {
                calls.push_back("call void @zeus_class_ref_release(ptr " + value + ")");
            }
        };
        auto appendValue = [&](const NativeValue& value) {
            if (value.dynamic && value.ownsDynamic) {
                calls.push_back("call void @zeus_value_release(ptr " +
                                value.value + ")");
            } else if (value.ownsClass && isManagedClassValue(value)) {
                calls.push_back("call void @zeus_class_ref_release(ptr " +
                                value.value + ")");
            } else if (value.ownsNativeBytes) {
                calls.push_back("call void @zeus_bytes_release(ptr " +
                                value.value + ")");
            } else if (value.ownsNativeString) {
                calls.push_back("call void @zeus_string_release(ptr " +
                                value.value + ")");
            }
        };
        for (auto it = nativeLocalSlots.rbegin(); it != nativeLocalSlots.rend(); ++it) {
            appendSlot(it->second, true);
        }
        for (auto it = nativeLocals.rbegin(); it != nativeLocals.rend(); ++it) {
            appendValue(it->second);
        }
        // Global IDs follow declaration order; package relocation preserves it.
        // Interleave all representations of each root so a later alias releases
        // before an earlier module dependency, regardless of storage ABI.
        std::set<int> globalIndices(entryDynamicValueGlobals.begin(), entryDynamicValueGlobals.end());
        for (const auto& entry : nativeGlobalSlots) globalIndices.insert(entry.first);
        for (const auto& entry : nativeGlobals) globalIndices.insert(entry.first);
        for (auto it = globalIndices.rbegin(); it != globalIndices.rend(); ++it) {
            const auto slot = nativeGlobalSlots.find(*it);
            if (slot != nativeGlobalSlots.end()) appendSlot(slot->second, false);
            const auto value = nativeGlobals.find(*it);
            if (value != nativeGlobals.end()) appendValue(value->second);
            if (entryDynamicValueGlobals.count(*it)) {
                calls.push_back("call void @zeus_global_clear(i32 " + std::to_string(*it) + ")");
            }
        }
        return calls;
    }

    bool parseEntryGlobalClearCall(const std::string& call, int& index) const {
        static const std::string prefix =
            "call void @zeus_global_clear(i32 ";
        if (call.rfind(prefix, 0) != 0 || call.empty() || call.back() != ')') {
            return false;
        }
        const std::string text = call.substr(prefix.size(),
                                             call.size() - prefix.size() - 1);
        try {
            size_t parsed = 0;
            const int value = std::stoi(text, &parsed);
            if (parsed != text.size()) return false;
            index = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string internEntryGlobalCleanupHelper(
        const std::vector<int>& indices) {
        const auto existing = entryGlobalCleanupHelpers.find(indices);
        if (existing != entryGlobalCleanupHelpers.end()) return existing->second;
        const std::string name = "__zeus_entry_global_cleanup_" +
            std::to_string(entryGlobalCleanupHelpers.size());
        entryGlobalCleanupHelpers.emplace(indices, name);
        return name;
    }

    std::vector<std::string> compactEntryGlobalCleanupCalls(
        const std::vector<std::string>& calls) {
        std::vector<std::string> compacted;
        for (size_t i = 0; i < calls.size();) {
            int index = 0;
            if (!parseEntryGlobalClearCall(calls[i], index)) {
                compacted.push_back(calls[i++]);
                continue;
            }

            std::vector<int> indices;
            size_t end = i;
            while (end < calls.size() &&
                   parseEntryGlobalClearCall(calls[end], index)) {
                indices.push_back(index);
                ++end;
            }
            if (indices.size() < 2) {
                compacted.insert(compacted.end(), calls.begin() + i,
                                 calls.begin() + end);
            } else {
                const std::string helper =
                    internEntryGlobalCleanupHelper(indices);
                compacted.push_back("call void @" + helper + "()");
            }
            i = end;
        }
        return compacted;
    }

    std::string emitEntryGlobalCleanupHelpers() const {
        std::string emitted;
        for (const auto& helper : entryGlobalCleanupHelpers) {
            emitted += "define internal void @" + helper.second +
                "() {\nentry:\n";
            for (int index : helper.first) {
                emitted += "  call void @zeus_global_clear(i32 " +
                    std::to_string(index) + ")\n";
            }
            emitted += "  ret void\n}\n";
        }
        return emitted;
    }

    void deduplicateEntryCleanupPads(std::string& body) {
        std::map<std::vector<std::string>, std::string> sharedLabels;
        for (auto& pad : entryExceptionPads) {
            if (!pad.cleanupOnly) continue;
            const auto existing = sharedLabels.find(pad.cleanupCalls);
            if (existing == sharedLabels.end()) {
                sharedLabels.emplace(pad.cleanupCalls, pad.switchLabel);
                continue;
            }

            const std::string oldUnwind = "unwind label " + pad.switchLabel + "\n";
            const std::string sharedUnwind =
                "unwind label " + existing->second + "\n";
            size_t position = 0;
            while ((position = body.find(oldUnwind, position)) !=
                   std::string::npos) {
                body.replace(position, oldUnwind.size(), sharedUnwind);
                position += sharedUnwind.size();
            }
            pad.emit = false;
        }
    }

    void prescanGlobalTypes() {
        nativeGlobalScalarTypes.clear();
        prescanGlobalClassTypes.clear();
        prescanGlobalReachabilityTypes.clear();
        prescanGlobalNativeViewTypes.clear();
        nativeGlobalDeclaredIntegerTypes.clear();
        nativeGlobalDeclaredDoubleTypes.clear();
        prescanAddressExposedGlobals.clear();
        if (!programChunks) return;

        std::set<int> typedNativePointerGlobals;
        for (const auto& chunk : *programChunks) {
            for (const auto& instruction : chunk) {
                if (instruction.op != VMOperator::STORE_VALUE_GLOBAL ||
                    instruction.metadata.kind !=
                        IRType::IRMetadataKind::GlobalBindingType ||
                    instruction.metadata.typeName.empty()) {
                    continue;
                }
                int64_t index = -1;
                if (!tryArgInt(instruction, 0, index) || index < 0 ||
                    index > std::numeric_limits<int>::max()) {
                    continue;
                }
                const auto* abi = ZeusBuiltins::findNativeTypeABI(
                    instruction.metadata.typeName);
                if (ZeusBuiltins::findNativeIntegerABI(instruction.metadata.typeName)) {
                    nativeGlobalDeclaredIntegerTypes[static_cast<int>(index)] =
                        std::string(abi->canonicalName);
                }
                if (abi && abi->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint) {
                    nativeGlobalDeclaredDoubleTypes[static_cast<int>(index)] = std::string(abi->canonicalName);
                }
                if (ZeusBuiltins::isNativePointerABI(abi)) {
                    const int globalIndex = static_cast<int>(index);
                    typedNativePointerGlobals.insert(globalIndex);
                    prescanGlobalNativeViewTypes[globalIndex] =
                        std::string(abi->canonicalName);
                }
            }
        }

        std::set<int> ambiguousNativeViewGlobals;
        std::set<int> ambiguousReachabilityGlobals;
        std::set<int> boxedLiteralGlobals;
        auto recordReachabilityType = [this, &ambiguousReachabilityGlobals](
            int index, const std::string& type
        ) {
            if (type != "Array" && type != "dict") {
                prescanGlobalReachabilityTypes.erase(index);
                ambiguousReachabilityGlobals.insert(index);
                return;
            }
            if (ambiguousReachabilityGlobals.find(index) !=
                ambiguousReachabilityGlobals.end()) return;
            const auto found = prescanGlobalReachabilityTypes.find(index);
            if (found == prescanGlobalReachabilityTypes.end()) {
                prescanGlobalReachabilityTypes.emplace(index, type);
            } else if (found->second != type) {
                prescanGlobalReachabilityTypes.erase(found);
                ambiguousReachabilityGlobals.insert(index);
            }
        };
        auto clearReachabilityType = [this, &ambiguousReachabilityGlobals](int index) {
            // Chunks are scanned in compiler order, not runtime order. An
            // unknown write in a function can therefore precede a known
            // container initializer in this scan even when it runs later.
            // Keep the uncertainty sticky so later evidence cannot resurrect
            // an unsafe global container type.
            prescanGlobalReachabilityTypes.erase(index);
            ambiguousReachabilityGlobals.insert(index);
        };
        auto recordNativeViewType = [this, &ambiguousNativeViewGlobals,
                                     &typedNativePointerGlobals](
            int index, const std::string& sourceType) {
            if (typedNativePointerGlobals.find(index) !=
                typedNativePointerGlobals.end()) {
                return;
            }
            if (ambiguousNativeViewGlobals.find(index) !=
                ambiguousNativeViewGlobals.end()) {
                return;
            }
            const auto* abi = ZeusBuiltins::findNativeTypeABI(sourceType);
            if (!ZeusBuiltins::isNativePointerABI(abi)) {
                prescanGlobalNativeViewTypes.erase(index);
                ambiguousNativeViewGlobals.insert(index);
                return;
            }
            const std::string canonicalType(abi->canonicalName);
            const auto existing = prescanGlobalNativeViewTypes.find(index);
            if (existing == prescanGlobalNativeViewTypes.end()) {
                prescanGlobalNativeViewTypes.emplace(index, canonicalType);
            } else if (existing->second != canonicalType) {
                prescanGlobalNativeViewTypes.erase(existing);
                ambiguousNativeViewGlobals.insert(index);
            }
        };
        auto nativeViewLiteralType = [](const auto& argument) -> std::string {
            if (std::holds_alternative<LexerType::Value>(argument)) {
                const auto& literal = std::get<LexerType::Value>(argument);
                if (std::holds_alternative<std::string>(literal.data)) {
                    return "string";
                }
            } else if (std::holds_alternative<ZeusType::Bytes>(argument)) {
                return "bytes";
            }
            return {};
        };
        auto isNilPush = [](const IRType::IRInstr& instruction) {
            return instruction.op == VMOperator::PUSH &&
                !instruction.args.empty() &&
                std::holds_alternative<IRType::NullLiteral>(instruction.args[0]);
        };
        auto nativeCallReturnViewType = [this](
            const IRType::IRInstr& call
        ) -> std::string {
            if (call.op != VMOperator::CALL_CLOSURE &&
                call.op != VMOperator::OBJ_CALL_METHOD) {
                return {};
            }

            std::string directCallName;
            const bool hasDirectCall = call.metadata.readDirectCall(directCallName);
            const FunctionSignature* called = hasDirectCall
                ? resolveFunctionSignature(directCallName) : nullptr;
            const std::string returnType = called != nullptr
                ? called->returnType
                : call.metadata.kind == IRType::IRMetadataKind::CallTarget
                    ? call.metadata.typeName
                    : std::string();
            if (returnType.empty() || isReferenceTypeName(returnType)) return {};

            const auto* abi = ZeusBuiltins::findNativeTypeABI(returnType);
            return ZeusBuiltins::isNativePointerABI(abi)
                ? std::string(abi->canonicalName)
                : std::string();
        };

        for (const auto& chunk : *programChunks) {
            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                const auto& store = chunk[pc];
                if (store.op == VMOperator::HOTSPOT) {
                    int64_t hotspotId = -1;
                    int64_t index = -1;
                    if (tryArgInt(store, 0, hotspotId) && hotspotId == 0 &&
                        store.args.size() >= 4 && tryArgInt(store, 2, index) &&
                        index >= 0 && index <= std::numeric_limits<int>::max()) {
                        recordNativeViewType(
                            static_cast<int>(index),
                            nativeViewLiteralType(store.args[1]));
                    }
                    continue;
                }
                if (store.op == VMOperator::DELETE_VAR) {
                    int64_t deletedIndex = -1;
                    if (store.args.size() >= 3 &&
                        std::holds_alternative<std::string>(store.args[1]) &&
                        std::get<std::string>(store.args[1]) == "global" &&
                        tryArgInt(store, 2, deletedIndex) && deletedIndex >= 0 &&
                        deletedIndex <= std::numeric_limits<int>::max()) {
                        clearReachabilityType(static_cast<int>(deletedIndex));
                    }
                    continue;
                }
                if (store.op != VMOperator::STORE_VALUE_GLOBAL &&
                    store.op != VMOperator::ASSIGN_VALUE_GLOBAL) {
                    continue;
                }
                if (store.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(store.args[0])) {
                    continue;
                }
                const auto& indexLiteral = std::get<LexerType::Value>(store.args[0]);
                if (!std::holds_alternative<ZeusType::BigInteger>(indexLiteral.data)) {
                    continue;
                }
                int64_t index = 0;
                if (!ZeusVMUtils::tryGetInt64FromValue(indexLiteral, index) || index < 0 ||
                    index > std::numeric_limits<int>::max()) {
                    continue;
                }

                const int globalIndex = static_cast<int>(index);
                if (pc > 0) {
                    const auto& initializer = chunk[pc - 1];
                    std::string reachabilityType;
                    if (initializer.op == VMOperator::LIST_BUILD) {
                        reachabilityType = "Array";
                    } else if (initializer.op == VMOperator::DICT_BUILD) {
                        reachabilityType = "dict";
                    } else if (initializer.op == VMOperator::LOAD_VALUE &&
                               initializer.args.size() > 1 &&
                               std::holds_alternative<std::string>(initializer.args[0]) &&
                               std::get<std::string>(initializer.args[0]) == "global") {
                        int64_t sourceIndex = -1;
                        if (tryArgInt(initializer, 1, sourceIndex) && sourceIndex >= 0 &&
                            sourceIndex <= std::numeric_limits<int>::max()) {
                            const auto sourceType = prescanGlobalReachabilityTypes.find(
                                static_cast<int>(sourceIndex));
                            if (sourceType != prescanGlobalReachabilityTypes.end()) {
                                reachabilityType = sourceType->second;
                            }
                        }
                    } else if (initializer.op == VMOperator::OBJ_BUILD ||
                               initializer.op == VMOperator::NEW_OBJ) {
                        std::string className;
                        std::vector<std::string> typeArguments;
                        std::string specializedClassName;
                        const std::string explicitTarget = initializer.args.size() > 1 &&
                            std::holds_alternative<std::string>(initializer.args[1])
                                ? std::get<std::string>(initializer.args[1])
                                : std::string();
                        if (!explicitTarget.empty()) {
                            reachabilityType = explicitTarget;
                        } else if (initializer.metadata.readClassInstantiation(
                                       className, typeArguments,
                                       &specializedClassName)) {
                            const std::string target = specializedClassName.empty()
                                ? className : specializedClassName;
                            const FunctionSignature* factory =
                                findNewOperatorSignature(target);
                            reachabilityType = factory == nullptr
                                ? target : factory->returnType;
                        }
                    } else if (initializer.op == VMOperator::CALL_CLOSURE ||
                               initializer.op == VMOperator::OBJ_CALL_METHOD) {
                        std::string targetName;
                        const FunctionSignature* called = nullptr;
                        if (initializer.metadata.readDirectCall(targetName)) {
                            called = resolveFunctionSignature(targetName);
                        }
                        if (called != nullptr) {
                            reachabilityType = called->returnType;
                        } else if (initializer.metadata.kind ==
                                   IRType::IRMetadataKind::CallTarget) {
                            reachabilityType = initializer.metadata.typeName;
                        }
                        if (reachabilityType.empty() &&
                            initializer.op == VMOperator::OBJ_CALL_METHOD &&
                            pc >= 2 && !initializer.args.empty() &&
                            std::holds_alternative<std::string>(initializer.args[0])) {
                            const auto& receiverLoad = chunk[pc - 2];
                            int64_t receiverIndex = -1;
                            std::string receiverType;
                            if (receiverLoad.op == VMOperator::LOAD_VALUE &&
                                receiverLoad.args.size() > 1 &&
                                std::holds_alternative<std::string>(receiverLoad.args[0]) &&
                                std::get<std::string>(receiverLoad.args[0]) == "global" &&
                                tryArgInt(receiverLoad, 1, receiverIndex) &&
                                receiverIndex >= 0 && receiverIndex <=
                                    std::numeric_limits<int>::max()) {
                                const auto receiver = prescanGlobalReachabilityTypes.find(
                                    static_cast<int>(receiverIndex));
                                if (receiver != prescanGlobalReachabilityTypes.end()) {
                                    receiverType = receiver->second;
                                }
                            }
                            int64_t methodArgc = -1;
                            const std::string& methodName =
                                std::get<std::string>(initializer.args[0]);
                            if (!receiverType.empty() &&
                                tryArgInt(initializer, 1, methodArgc) && methodArgc >= 0) {
                                for (const auto& function : functionSignatures) {
                                    const FunctionSignature& candidate = function.second;
                                    if (candidate.ownerType == receiverType &&
                                        candidate.name == receiverType + "::" + methodName &&
                                        !candidate.isStaticMethod &&
                                        candidate.parameterTypes.size() ==
                                            static_cast<size_t>(methodArgc) + 1) {
                                        reachabilityType = candidate.returnType;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (reachabilityType.empty()) {
                        const bool isNilDeclaration =
                            store.op == VMOperator::STORE_VALUE_GLOBAL &&
                            isNilPush(initializer);
                        if (!isNilDeclaration) clearReachabilityType(globalIndex);
                    } else {
                        recordReachabilityType(globalIndex, reachabilityType);
                    }
                } else {
                    clearReachabilityType(globalIndex);
                }

                std::string nativeViewType;
                if (pc > 0 && chunk[pc - 1].op == VMOperator::PUSH &&
                    !chunk[pc - 1].args.empty()) {
                    nativeViewType = nativeViewLiteralType(chunk[pc - 1].args[0]);
                } else if (pc > 0) {
                    nativeViewType = nativeCallReturnViewType(chunk[pc - 1]);
                }
                recordNativeViewType(static_cast<int>(index), nativeViewType);

                if (pc > 0 && chunk[pc - 1].op == VMOperator::PUSH &&
                    !chunk[pc - 1].args.empty() &&
                    std::holds_alternative<LexerType::Value>(chunk[pc - 1].args[0])) {
                    const auto& literal =
                        std::get<LexerType::Value>(chunk[pc - 1].args[0]);
                    const char* type = nullptr;
                    if (const auto* integer = std::get_if<ZeusType::BigInteger>(&literal.data)) {
                        int64_t inlineValue = 0;
                        if (integer->tryToInt64(inlineValue)) type = "i64";
                    } else if (std::holds_alternative<bool>(literal.data)) {
                        type = "i1";
                    }
                    const int globalIndex = static_cast<int>(index);
                    if (!type) {
                        nativeGlobalScalarTypes.erase(globalIndex);
                        boxedLiteralGlobals.insert(globalIndex);
                    }
                    if (type) {
                        const auto existing = nativeGlobalScalarTypes.find(globalIndex);
                        if (existing != nativeGlobalScalarTypes.end() && existing->second != type) {
                            nativeGlobalScalarTypes.erase(existing);
                            boxedLiteralGlobals.insert(globalIndex);
                        }
                        if (!boxedLiteralGlobals.count(globalIndex)) {
                            nativeGlobalScalarTypes[globalIndex] = type;
                        }
                        continue;
                    }
                }

                if (pc == 0) continue;
                const auto& build = chunk[pc - 1];

                if (build.op == VMOperator::OBJ_BUILD ||
                    build.op == VMOperator::NEW_OBJ) {
                    std::string genericClassName;
                    std::vector<std::string> genericTypeArguments;
                    std::string specializedClassName;
                    if (build.metadata.readClassInstantiation(
                            genericClassName, genericTypeArguments,
                            &specializedClassName) &&
                        !specializedClassName.empty() &&
                        isClassType(specializedClassName)) {
                        prescanGlobalClassTypes[static_cast<int>(index)] =
                            std::move(specializedClassName);
                        continue;
                    }
                }

                if (build.op == VMOperator::CALL_CLOSURE ||
                    build.op == VMOperator::OBJ_CALL_METHOD) {
                    std::string directCallName;
                    const bool hasDirectCall = build.metadata.readDirectCall(directCallName) ||
                        (pc >= 2 && annotationKind(chunk[pc - 2], "direct_call") &&
                         annotationSymbolName(chunk[pc - 2], directCallName));
                    const FunctionSignature* called = hasDirectCall
                        ? resolveFunctionSignature(directCallName) : nullptr;
                    const std::string returnedType = called != nullptr
                        ? called->returnType
                        : build.metadata.kind == IRType::IRMetadataKind::CallTarget
                            ? build.metadata.typeName
                            : std::string();
                    if (!returnedType.empty()) {
                        const std::string classType = isReferenceTypeName(returnedType)
                            ? referencedTypeName(returnedType) : returnedType;
                        if (isClassType(classType)) {
                            prescanGlobalClassTypes[static_cast<int>(index)] = classType;
                            continue;
                        }
                        if (!isReferenceTypeName(returnedType)) {
                            const auto* abi = ZeusBuiltins::findNativeTypeABI(returnedType);
                            if (ZeusBuiltins::isNativePointerABI(abi)) {
                                recordNativeViewType(static_cast<int>(index), returnedType);
                                continue;
                            }
                            const std::string llvmTypeName = llvmType(returnedType);
                            if ((llvmTypeName == "i64" || llvmTypeName == "i1") &&
                                !boxedLiteralGlobals.count(static_cast<int>(index))) {
                                nativeGlobalScalarTypes[static_cast<int>(index)] =
                                    llvmTypeName;
                                continue;
                            }
                        }
                    }
                }

                // A class declaration keeps its name in the CLASS_BUILD metadata.
                if (build.op == VMOperator::CLASS_BUILD ||
                    build.op == VMOperator::CLASS_BUILD_SHARED) {
                    std::string className;
                    ClassLayout layout;
                    if (build.metadata.readClassLayout(
                            className, layout.fieldNames, layout.fieldTypes,
                            &layout.fieldConstness, &layout.fieldDefaults)) {
                        prescanGlobalClassTypes[static_cast<int>(index)] =
                            std::move(className);
                    }
                    continue;
                }

                // An instance is built by OBJ_BUILD, preceded by the class
                // reference and then the constructor arguments.
                if (build.op != VMOperator::OBJ_BUILD &&
                    build.op != VMOperator::NEW_OBJ) {
                    continue;
                }
                for (size_t back = 1; back <= 8 && pc >= back; ++back) {
                    const auto& load = chunk[pc - back];
                    if (load.op != VMOperator::LOAD_VALUE || load.args.empty() ||
                        !std::holds_alternative<std::string>(load.args[0])) {
                        continue;
                    }
                    const std::string& name = std::get<std::string>(load.args[0]);
                    if (isClassType(name)) {
                        prescanGlobalClassTypes[static_cast<int>(index)] = name;
                        break;
                    }
                    if (name == "global" && load.args.size() > 1 &&
                        std::holds_alternative<LexerType::Value>(load.args[1])) {
                        int64_t classSlot = 0;
                        if (ZeusVMUtils::tryGetInt64FromValue(
                                std::get<LexerType::Value>(load.args[1]), classSlot)) {
                            const auto classGlobal =
                                classGlobals.find(static_cast<int>(classSlot));
                            if (classGlobal != classGlobals.end() &&
                                isClassType(classGlobal->second)) {
                                prescanGlobalClassTypes[static_cast<int>(index)] =
                                    classGlobal->second;
                            }
                        }
                    }
                    break;
                }
            }
        }

        // Top-level expressions may span multiple IR chunks. Track class
        // references and constructed instances across those chunks so a later
        // DELETE_VAR can retain only the destructor for the variable's class.
        std::vector<std::string> stack;
        auto popAbstract = [&]() {
            if (stack.empty()) return std::string();
            std::string value = std::move(stack.back());
            stack.pop_back();
            return value;
        };
        auto popAbstractCount = [&](int64_t count) {
            if (count < 0 || static_cast<uint64_t>(count) > stack.size()) {
                stack.clear();
                return;
            }
            while (count-- > 0) stack.pop_back();
        };
        for (size_t ci = 0; ci < programChunks->size(); ++ci) {
            const auto& chunk = (*programChunks)[ci];
            if (functionSignaturesByChunk.find(ci) != functionSignaturesByChunk.end() ||
                (!chunk.empty() && chunk.front().metadata.kind ==
                    IRType::IRMetadataKind::ModuleImportInit)) {
                stack.clear();
                continue;
            }

            for (const auto& ins : chunk) {
                switch (ins.op) {
                    case VMOperator::PUSH:
                    case VMOperator::LOAD_NATIVE:
                    case VMOperator::MAKE_FUNCTION:
                    case VMOperator::LOAD_CELL_REFERENCE:
                        stack.emplace_back();
                        break;

                    case VMOperator::LOAD_VALUE: {
                        std::string value;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0]) &&
                            std::get<std::string>(ins.args[0]) == "global") {
                            int64_t index = -1;
                            if (tryArgInt(ins, 1, index) && index >= 0 &&
                                index <= std::numeric_limits<int>::max()) {
                                const int globalIndex = static_cast<int>(index);
                                const auto nameSpace =
                                    namespacePackagesByGlobal.find(globalIndex);
                                const auto classGlobal = classGlobals.find(globalIndex);
                                const auto instance = prescanGlobalClassTypes.find(globalIndex);
                                if (nameSpace != namespacePackagesByGlobal.end()) {
                                    value = "namespace:" + nameSpace->second;
                                } else if (classGlobal != classGlobals.end()) {
                                    value = "class:" + classGlobal->second;
                                } else if (instance != prescanGlobalClassTypes.end()) {
                                    value = "instance:" + instance->second;
                                }
                            }
                        }
                        stack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::CLASS_BUILD:
                    case VMOperator::CLASS_BUILD_SHARED: {
                        std::string className;
                        std::vector<std::string> fieldNames;
                        std::vector<std::string> fieldTypes;
                        if (ins.metadata.readClassLayout(
                                className, fieldNames, fieldTypes)) {
                            stack.push_back("class:" + className);
                        } else {
                            stack.emplace_back();
                        }
                        break;
                    }

                    case VMOperator::OBJ_LOAD_ATTR:
                    case VMOperator::OBJ_LOAD_SAFE_ATTR: {
                        const std::string receiver = popAbstract();
                        std::string member;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            member = std::get<std::string>(ins.args[0]);
                        }
                        std::string value;
                        const std::string namespacePrefix = "namespace:";
                        if (receiver.rfind(namespacePrefix, 0) == 0) {
                            const std::string package = receiver.substr(namespacePrefix.size());
                            const auto exports = namespaceExportsByPackage.find(package);
                            if (exports != namespaceExportsByPackage.end() &&
                                exports->second.find(member) != exports->second.end() &&
                                classLayouts.find(member) != classLayouts.end()) {
                                value = "class:" + member;
                            }
                        }
                        stack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::LLVM_ADDR:
                        stack.push_back("int");
                        break;

                    case VMOperator::LLVM_SIZEOF:
                        stack.push_back("int");
                        break;

                    case VMOperator::LLVM_FADDR:
                        popAbstractCount(ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1]) &&
                            std::get<std::string>(ins.args[1]) == "length" ? 2 : 1);
                        stack.push_back(ins.args.empty() ||
                            !std::holds_alternative<std::string>(ins.args[0])
                                ? std::string() : std::get<std::string>(ins.args[0]));
                        break;

                    case VMOperator::LLVM_FSTORE:
                        popAbstractCount(2);
                        break;

                    case VMOperator::OBJ_BUILD:
                    case VMOperator::OBJ_BUILD_SHARED:
                    case VMOperator::NEW_OBJ: {
                        int64_t argc = 0;
                        if (!tryArgInt(ins, 0, argc) || argc < 0) {
                            stack.clear();
                            break;
                        }
                        popAbstractCount(argc);
                        std::string targetType;
                        if (ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            targetType = std::get<std::string>(ins.args[1]);
                        }
                        const std::string classRef = targetType.empty()
                            ? popAbstract() : "class:" + targetType;
                        const std::string classPrefix = "class:";
                        if (classRef.rfind(classPrefix, 0) == 0) {
                            const std::string type = classRef.substr(classPrefix.size());
                            if (isClassType(type)) {
                                stack.push_back("instance:" + type);
                                break;
                            }
                        }
                        stack.emplace_back();
                        break;
                    }

                    case VMOperator::STORE_VALUE_GLOBAL:
                    case VMOperator::ASSIGN_VALUE_GLOBAL: {
                        int64_t index = -1;
                        const std::string value = popAbstract();
                        if (!tryArgInt(ins, 0, index) || index < 0 ||
                            index > std::numeric_limits<int>::max()) break;
                        const std::string instancePrefix = "instance:";
                        if (value.rfind(instancePrefix, 0) == 0) {
                            prescanGlobalClassTypes[static_cast<int>(index)] =
                                value.substr(instancePrefix.size());
                        }
                        break;
                    }

                    case VMOperator::OBJ_CALL_METHOD:
                    case VMOperator::OBJ_CALL_SAFE:
                    case VMOperator::CALL_NATIVE:
                    case VMOperator::CALL_CLOSURE: {
                        int64_t argc = 0;
                        if (!tryArgInt(ins, ins.op == VMOperator::OBJ_CALL_METHOD ||
                                            ins.op == VMOperator::OBJ_CALL_SAFE ? 1 : 0,
                                       argc) || argc < 0) {
                            stack.clear();
                            break;
                        }
                        // CALL_CLOSURE keeps the callable on the VM stack even
                        // when metadata resolves its target statically. Method
                        // and native calls likewise consume receiver/callee.
                        popAbstractCount(argc + 1);
                        stack.emplace_back();
                        break;
                    }

                    case VMOperator::LIST_BUILD:
                    case VMOperator::DICT_BUILD: {
                        int64_t count = 0;
                        if (!tryArgInt(ins, 0, count) || count < 0) {
                            stack.clear();
                            break;
                        }
                        popAbstractCount(count * (ins.op == VMOperator::DICT_BUILD ? 2 : 1));
                        stack.emplace_back();
                        break;
                    }

                    case VMOperator::LIST_LENGTH:
                    case VMOperator::DICT_KEYS_BUILD:
                        popAbstract();
                        stack.emplace_back();
                        break;

                    case VMOperator::LIST_LOAD_ELEM:
                    case VMOperator::DICT_LOAD_VALUE:
                        popAbstractCount(2);
                        stack.emplace_back();
                        break;

                    case VMOperator::LIST_STORE_ELEM:
                    case VMOperator::DICT_STORE_VALUE:
                    case VMOperator::OBJ_STORE_ATTR:
                        popAbstractCount(3);
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
                        popAbstractCount(2);
                        stack.emplace_back();
                        break;

                    case VMOperator::POP:
                        popAbstract();
                        break;

                    case VMOperator::CLEAR:
                        stack.clear();
                        break;

                    case VMOperator::COPY:
                        stack.push_back(stack.empty() ? std::string() : stack.back());
                        break;

                    case VMOperator::SWAP:
                        if (stack.size() >= 2) {
                            std::iter_swap(stack.end() - 1, stack.end() - 2);
                        } else {
                            stack.clear();
                        }
                        break;

                    case VMOperator::JUMP_IF_TRUE:
                    case VMOperator::JUMP_IF_FALSE:
                    case VMOperator::COUNT_CHECK_NEG:
                    case VMOperator::COUNT_CHECK_STEP_ZERO:
                        popAbstract();
                        break;

                    default:
                        stack.clear();
                        break;
                }
            }
        }

        for (const auto& chunk : *programChunks) {
            for (const auto& instruction : chunk) {
                if (instruction.op == VMOperator::LLVM_ADDR &&
                    instruction.args.size() >= 2 &&
                    std::holds_alternative<std::string>(instruction.args[0]) &&
                    std::get<std::string>(instruction.args[0]) == "global") {
                    int64_t index = -1;
                    if (tryArgInt(instruction, 1, index) && index >= 0 &&
                        index <= std::numeric_limits<int>::max()) {
                        prescanAddressExposedGlobals.insert(static_cast<int>(index));
                    }
                    continue;
                }

                if (instruction.op != VMOperator::CALL_CLOSURE &&
                    instruction.op != VMOperator::OBJ_CALL_METHOD &&
                    instruction.op != VMOperator::CLASS_STATIC_METHOD_CALL &&
                    instruction.op != VMOperator::CALL_NATIVE) {
                    continue;
                }

                std::string targetName;
                const FunctionSignature* target = nullptr;
                if (instruction.metadata.readDirectCall(targetName)) {
                    target = resolveFunctionSignature(targetName);
                }

                int receiverOffset = 0;
                if (instruction.op == VMOperator::OBJ_CALL_METHOD && target &&
                    !target->isStaticMethod) {
                    receiverOffset = 1;
                }

                for (const auto& argument : instruction.metadata.callArguments) {
                    if (!argument.isAssignable || argument.source.isConst ||
                        argument.source.kind != IRType::SymbolRefKind::Global ||
                        argument.source.index < 0) {
                        continue;
                    }

                    bool mayPassByReference = target == nullptr;
                    if (target != nullptr) {
                        size_t parameterIndex = target->parameterTypes.size();
                        bool parameterKnown = false;
                        if (!argument.namedParameter.empty()) {
                            const auto parameter = std::find(
                                target->parameterNames.begin(),
                                target->parameterNames.end(),
                                argument.namedParameter);
                            if (parameter != target->parameterNames.end()) {
                                parameterIndex = static_cast<size_t>(
                                    std::distance(target->parameterNames.begin(), parameter));
                                parameterKnown = true;
                            }
                        } else if (argument.positionalIndex >= 0) {
                            parameterIndex = static_cast<size_t>(
                                argument.positionalIndex + receiverOffset);
                            parameterKnown = true;
                        }

                        mayPassByReference = !parameterKnown ||
                            parameterIndex >= target->parameterIsReference.size() ||
                            target->parameterIsReference[parameterIndex];
                    }
                    if (mayPassByReference) {
                        prescanAddressExposedGlobals.insert(argument.source.index);
                    }
                }
            }
        }
    }
