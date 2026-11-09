// Included inside Emitter: class-layout reachability follows live functions,
// allocations, signatures, and field type dependencies.
    std::string classLayoutNameForType(std::string typeName) const {
        while (!typeName.empty() && typeName.front() == '&') {
            typeName.erase(typeName.begin());
        }
        if (!typeName.empty() && typeName.back() == '?') {
            typeName.pop_back();
        }
        if (classLayouts.find(typeName) != classLayouts.end()) {
            return typeName;
        }

        std::string match;
        for (const auto& entry : classLayouts) {
            const std::string suffix = "." + entry.first;
            if (typeName.size() <= suffix.size() ||
                typeName.compare(
                    typeName.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            if (!match.empty()) return {};
            match = entry.first;
        }
        return match;
    }

    void collectReachableClassLayouts(
        const std::vector<std::vector<IRType::IRInstr>>& program,
        const std::set<size_t>* rootChunks
    ) {
        reachableClassLayouts.clear();
        std::vector<std::string> pending;
        auto retainClass = [&](const std::string& typeName) {
            const std::string className = classLayoutNameForType(typeName);
            if (!className.empty() &&
                reachableClassLayouts.insert(className).second) {
                pending.push_back(className);
            }
        };

        for (const auto& typeName : constructedClassNames) retainClass(typeName);
        for (const auto& typeName : embeddedSlotTypes) retainClass(typeName);
        if (needsRuntimeErrorFactory) {
            retainClass("RuntimeError");
            retainClass("TypeError");
        }
        if (programHasDynamicClassConstruction) {
            // An unresolved class handle may name any class in the stable AOT
            // handle table, so every entry is a genuine dynamic-construction edge.
            for (const auto& entry : classLayouts) retainClass(entry.first);
        }
        for (const auto& entry : classLayouts) {
            if (classNeedsRuntimeMachinery(entry.first)) retainClass(entry.first);
        }

        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            const bool reachable = signature.isExternal
                ? reachableExternalFunctions.find(signature.name) !=
                    reachableExternalFunctions.end()
                : reachableFunctionChunks.find(signature.chunkIndex) !=
                    reachableFunctionChunks.end();
            if (!reachable) continue;

            retainClass(signature.ownerType);
            retainClass(signature.returnType);
            for (const auto& parameterType : signature.parameterTypes) {
                retainClass(parameterType);
            }
        }

        for (size_t ci = 0; ci < program.size(); ++ci) {
            const bool reachableFunction =
                reachableFunctionChunks.find(ci) != reachableFunctionChunks.end();
            const bool reachableRoot =
                functionSignaturesByChunk.find(ci) == functionSignaturesByChunk.end() &&
                (rootChunks == nullptr || rootChunks->find(ci) != rootChunks->end());
            if (!reachableFunction && !reachableRoot) continue;

            for (const auto& ins : program[ci]) {
                if (ins.op == VMOperator::LOAD_VALUE && !ins.args.empty() &&
                    std::holds_alternative<std::string>(ins.args[0])) {
                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    if (kind == "global") {
                        int64_t index = -1;
                        if (tryArgInt(ins, 1, index) && index >= 0 &&
                            index <= std::numeric_limits<int>::max()) {
                            const auto classGlobal = classGlobals.find(
                                static_cast<int>(index));
                            if (classGlobal != classGlobals.end()) {
                                retainClass(classGlobal->second);
                            }
                            const auto typedGlobal = prescanGlobalClassTypes.find(
                                static_cast<int>(index));
                            if (typedGlobal != prescanGlobalClassTypes.end()) {
                                retainClass(typedGlobal->second);
                            }
                            const auto inferredGlobal = prescanGlobalReachabilityTypes.find(
                                static_cast<int>(index));
                            if (inferredGlobal != prescanGlobalReachabilityTypes.end()) {
                                retainClass(inferredGlobal->second);
                            }
                        }
                    } else if (kind != "local" && kind != "upvalue") {
                        retainClass(kind);
                    }
                    if (ins.metadata.symbol.kind == IRType::SymbolRefKind::Class) {
                        retainClass(ins.metadata.symbol.name);
                    }
                }

                std::string className;
                std::vector<std::string> typeArguments;
                std::string specializedClassName;
                if (ins.metadata.readClassInstantiation(
                        className, typeArguments, &specializedClassName)) {
                    retainClass(specializedClassName.empty()
                        ? className : specializedClassName);
                }
            }
        }

        while (!pending.empty()) {
            const std::string className = std::move(pending.back());
            pending.pop_back();
            const auto layout = classLayouts.find(className);
            if (layout == classLayouts.end()) continue;
            for (const auto& fieldType : layout->second.fieldTypes) {
                retainClass(fieldType);
            }
        }
    }

    void pruneUnreachableClassStructTypes() {
        for (auto type = structTypes.begin(); type != structTypes.end();) {
            if (classLayouts.find(*type) != classLayouts.end() &&
                reachableClassLayouts.find(*type) == reachableClassLayouts.end() &&
                !classNeedsRuntimeMachinery(*type)) {
                type = structTypes.erase(type);
            } else {
                ++type;
            }
        }
    }
