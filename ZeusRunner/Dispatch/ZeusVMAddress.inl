// VM member definitions; included inside class VM.
    ZEUS_VM_NOINLINE void execute_LLVM_ADDR(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LLVM_ADDR: {
                if (instr.args.size() < 2 ||
                    !std::holds_alternative<std::string>(instr.args[0]) ||
                    !std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    VMException::VMException("invalid VM @addr locator", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                int64_t rawIndex = -1;
                if (!ZeusVMUtils::tryGetInt64FromValue(
                        std::get<LexerType::Value>(instr.args[1]), rawIndex) ||
                    rawIndex < 0 || rawIndex > std::numeric_limits<int>::max()) {
                    VMException::VMException("invalid VM @addr slot index", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                const int index = static_cast<int>(rawIndex);
                const std::string& scope = std::get<std::string>(instr.args[0]);
                Cell* cell = nullptr;
                std::shared_ptr<ZeusShared::SharedCell> sharedCell;
                if (scope == "shared") {
                    if (instr.args.size() < 3 ||
                        !std::holds_alternative<std::string>(instr.args[2])) {
                        VMException::VMException(
                            "VM @addr shared locator is missing its name", __line, instr)
                            ._throw(vmstat, currentChunk);
                        break;
                    }
                    sharedCell = ZeusShared::getGlobalHeap().find(
                        std::get<std::string>(instr.args[2]));
                } else if (scope == "local" && index < static_cast<int>(local.size())) {
                    cell = local[static_cast<size_t>(index)];
                } else if (scope == "global" && index < static_cast<int>(global.size())) {
                    cell = global[static_cast<size_t>(index)];
                } else if (scope == "upvalue" && index < static_cast<int>(upvalue.size())) {
                    cell = upvalue[static_cast<size_t>(index)];
                } else {
                    VMException::VMException(
                        "VM @addr slot is outside its " + scope + " storage", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                if (scope == "shared" && !sharedCell) {
                    VMException::VMException(
                        "VM @addr shared variable is no longer present", __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                const bool reveal = instr.args.size() > 3 &&
                    std::holds_alternative<std::string>(instr.args[3]) &&
                    std::get<std::string>(instr.args[3]) == "reveal";
                void* nativeAddress = nullptr;
                std::string error;
                const bool addressed = scope == "shared"
                    ? addressSlots.address(
                        *this, sharedCell, std::get<std::string>(instr.args[2]),
                        reveal, instr.metadata.symbol.isConst, nativeAddress, error)
                    : addressSlots.address(
                        *this, cell, reveal, instr.metadata.symbol.isConst,
                        nativeAddress, error);
                if (!addressed) {
                    VMException::VMException(error, __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }

                const uint64_t addressBits =
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(nativeAddress));
                stack.push_back(LexerType::Value(static_cast<int64_t>(addressBits)));
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LLVM_FADDR(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LLVM_FADDR: {
                std::string error;
                int64_t addressBits = 0;
                int64_t accessSize = -1;
                std::string targetType;
                if (instr.args.empty() ||
                    !std::holds_alternative<std::string>(instr.args[0])) {
                    error = "invalid VM @faddr type metadata";
                } else {
                    targetType = std::get<std::string>(instr.args[0]);
                    constexpr const char* typeVariablePrefix = "__zeus_typevar:";
                    if (targetType.rfind(typeVariablePrefix, 0) == 0) {
                        constexpr const char* sourcePrefix = "__zeus_typevar_source:";
                        int64_t sourceIndex = -1;
                        if (instr.args.size() < 2 ||
                            !std::holds_alternative<std::string>(instr.args[1]) ||
                            std::get<std::string>(instr.args[1]).rfind(sourcePrefix, 0) != 0) {
                            error = "VM generic @faddr read has no type source parameter";
                        } else {
                            const std::string& source = std::get<std::string>(instr.args[1]);
                            const std::string digits = source.substr(std::char_traits<char>::length(sourcePrefix));
                            try {
                                size_t parsed = 0;
                                sourceIndex = std::stoll(digits, &parsed);
                                if (parsed != digits.size() || sourceIndex < 0) {
                                    error = "invalid VM generic @faddr source parameter";
                                }
                            } catch (...) {
                                error = "invalid VM generic @faddr source parameter";
                            }
                            if (error.empty() &&
                                (static_cast<uint64_t>(sourceIndex) >= local.size() ||
                                 local[static_cast<size_t>(sourceIndex)] == nullptr)) {
                                error = "VM generic @faddr source parameter is unavailable";
                            }
                            if (error.empty()) {
                                targetType = runtimeValueTypeName(
                                    loadCellValue(local[static_cast<size_t>(sourceIndex)]));
                                if (targetType.empty()) {
                                    error = "VM cannot infer a native @faddr type from the generic parameter";
                                }
                            }
                        }
                    }
                }
                const bool hasByteLength = instr.args.size() > 1 &&
                    std::holds_alternative<std::string>(instr.args[1]) &&
                    (std::get<std::string>(instr.args[1]) == "length" ||
                     std::get<std::string>(instr.args[1]) == "raw_range");
                if (error.empty() && hasByteLength) {
                    StackItem lengthItem = popStackItem();
                    if (!ZeusVMUtils::tryGetInt64(lengthItem.value, accessSize)) {
                        error = "VM @faddr length must be an int";
                    } else if (accessSize < 0) {
                        error = "VM @faddr length cannot be negative";
                    }
                    dropStackItem(lengthItem);
                } else if (error.empty() && instr.args.size() > 1 &&
                           std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    const auto& widthValue = std::get<LexerType::Value>(instr.args[1]);
                    const auto* widthInteger =
                        std::get_if<ZeusType::BigInteger>(&widthValue.data);
                    if (!widthInteger || !widthInteger->tryToInt64(accessSize)) {
                        error = "invalid VM @faddr width metadata";
                    }
                }
                StackItem addressItem = popStackItem();
                if (error.empty() &&
                           !ZeusVMUtils::tryGetInt64(addressItem.value, addressBits)) {
                    error = "VM @faddr requires an int address";
                }

                ZeusType::UniversalVariant loaded;
                if (error.empty()) {
                    void* nativeAddress = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(static_cast<uint64_t>(addressBits)));
                    if (!addressSlots.fetch(
                            *this, nativeAddress, targetType,
                            loaded, error, accessSize)) {
                        // The registered slot or tracked memory contract supplies the error.
                    }
                }
                dropStackItem(addressItem);
                if (!error.empty()) {
                    VMException::VMException(error, __line, instr)
                        ._throw(vmstat, currentChunk);
                    break;
                }
                stack.push_back(std::move(loaded));
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LLVM_FSTORE(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LLVM_FSTORE: {
                StackItem valueItem = popStackItem();
                std::string error;
                int64_t addressBits = 0;
                int64_t accessSize = -1;
                const bool hasByteLength = instr.args.size() > 1 &&
                    std::holds_alternative<std::string>(instr.args[1]) &&
                    (std::get<std::string>(instr.args[1]) == "length" ||
                     std::get<std::string>(instr.args[1]) == "raw_range");
                if (hasByteLength) {
                    StackItem lengthItem = popStackItem();
                    if (!ZeusVMUtils::tryGetInt64(lengthItem.value, accessSize)) {
                        error = "VM @faddr assignment length must be an int";
                    } else if (accessSize < 0) {
                        error = "VM @faddr assignment length cannot be negative";
                    }
                    dropStackItem(lengthItem);
                } else if (instr.args.size() > 1 &&
                    std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    const auto& widthValue = std::get<LexerType::Value>(instr.args[1]);
                    const auto* widthInteger =
                        std::get_if<ZeusType::BigInteger>(&widthValue.data);
                    if (!widthInteger || !widthInteger->tryToInt64(accessSize)) {
                        error = "invalid VM @faddr assignment width metadata";
                    }
                }
                StackItem addressItem = popStackItem();
                if (error.empty() && (instr.args.empty() ||
                    !std::holds_alternative<std::string>(instr.args[0]))) {
                    error = "invalid VM @faddr assignment type metadata";
                } else if (error.empty() &&
                           !ZeusVMUtils::tryGetInt64(addressItem.value, addressBits)) {
                    error = "VM @faddr assignment requires an int address";
                } else if (error.empty()) {
                    void* nativeAddress = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(static_cast<uint64_t>(addressBits)));
                    addressSlots.store(
                        *this, nativeAddress, std::get<std::string>(instr.args[0]),
                        valueItem.value, error, accessSize);
                }
                dropStackItem(valueItem);
                dropStackItem(addressItem);
                if (!error.empty()) {
                    VMException::VMException(error, __line, instr)
                        ._throw(vmstat, currentChunk);
                }
                break;
            }
        }
    }

    ZEUS_VM_NOINLINE void execute_LLVM_SIZEOF(
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        switch (instr.op) {
            case VMOperator::LLVM_SIZEOF: {
                VMException::VMException exp(
                    "sizeof(T) requires the LLVM backend",
                    __line, instr);
                exp._throw(vmstat, currentChunk);
                break;
            }
        }
    }
