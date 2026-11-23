// ZeusLLVM post-emission passes and emit() entry point; included inside namespace ZeusLLVM.
inline bool deduplicateFunctionDeclarations(
    std::string& module,
    std::string& error
) {
    std::map<std::string, std::string> declarations;
    std::string rewritten;
    rewritten.reserve(module.size());

    size_t lineStart = 0;
    while (lineStart < module.size()) {
        const size_t lineEnd = module.find('\n', lineStart);
        const size_t end = lineEnd == std::string::npos
            ? module.size() : lineEnd;
        const std::string line = module.substr(lineStart, end - lineStart);
        bool keepLine = true;

        if (line.rfind("declare ", 0) == 0) {
            const size_t at = line.find('@');
            const size_t open = at == std::string::npos
                ? std::string::npos : line.find('(', at + 1);
            if (at != std::string::npos && open != std::string::npos) {
                std::string name;
                size_t cursor = at + 1;
                if (cursor < open && line[cursor] == '\"') {
                    ++cursor;
                    bool closed = false;
                    while (cursor < open) {
                        char value = line[cursor++];
                        if (value == '\"') {
                            closed = true;
                            break;
                        }
                        if (value == '\\' && cursor + 1 < open &&
                            std::isxdigit(static_cast<unsigned char>(line[cursor])) &&
                            std::isxdigit(static_cast<unsigned char>(line[cursor + 1]))) {
                            auto hexValue = [](char digit) {
                                if (digit >= '0' && digit <= '9') return digit - '0';
                                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                                return digit - 'A' + 10;
                            };
                            value = static_cast<char>(
                                (hexValue(line[cursor]) << 4) |
                                hexValue(line[cursor + 1]));
                            cursor += 2;
                        }
                        name.push_back(value);
                    }
                    if (!closed || cursor != open) {
                        error = "invalid quoted LLVM declaration name";
                        return false;
                    }
                } else {
                    name = line.substr(at + 1, open - at - 1);
                }

                const std::string signature =
                    line.substr(0, at) + line.substr(open);
                const auto found = declarations.find(name);
                if (found == declarations.end()) {
                    declarations.emplace(name, signature);
                } else if (found->second == signature) {
                    keepLine = false;
                } else {
                    error = "conflicting LLVM declarations for '" + name + "'";
                    return false;
                }
            }
        }

        if (keepLine) {
            rewritten.append(line);
            if (lineEnd != std::string::npos) rewritten.push_back('\n');
        }
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
    }

    module = std::move(rewritten);
    return true;
}

// A personality is only consulted while unwinding through a frame that owns
// landing/cleanup/catch pads. The emitter stamps it on every function
// unconditionally; dropping it from pad-less functions removes .xdata tables
// (and, when no function needs it, the __CxxFrameHandler3 import) with zero
// semantic change: unwinding *through* such frames needs no EH tables.
inline void stripRedundantPersonalities(std::string& module) {
    static constexpr char kPersonality[] = " personality ptr @__CxxFrameHandler3";
    size_t cursor = 0;
    while (true) {
        const size_t personPos = module.find(kPersonality, cursor);
        if (personPos == std::string::npos) return;
        size_t blockStart = module.rfind("\ndefine ", personPos);
        blockStart = blockStart == std::string::npos ? 0 : blockStart + 1;
        size_t blockEnd = module.find("\n}\n", personPos);
        blockEnd = blockEnd == std::string::npos ? module.size() : blockEnd + 3;
        const std::string block = module.substr(blockStart, blockEnd - blockStart);
        const bool hasPad = block.find("cleanuppad") != std::string::npos ||
            block.find("catchswitch") != std::string::npos ||
            block.find("catchpad") != std::string::npos ||
            block.find("landingpad") != std::string::npos;
        if (!hasPad) {
            module.erase(personPos, sizeof(kPersonality) - 1);
            cursor = personPos;
        } else {
            cursor = personPos + sizeof(kPersonality) - 1;
        }
    }
}

// GC-safety allowlist for the purity post-pass below: runtime helpers audited
// to never allocate GC-tracked memory and never trigger collection (pure
// range checks, slot reads/writes, inline value inits, OS/library lookup,
// raw memory ops, refcount release which only shrinks). Anything not listed
// here — including every allocating helper — keeps the program in the
// collect-if-needed regime. Default-deny: a helper added later without an
// audit entry simply preserves today's safepoints.
inline bool isGcSafeRuntimeCall(const std::string& name) {
    static const std::vector<std::string> safe = {
        "zeus_lvalue_unregister",
        "zeus_lvalue_unregister_revealed",
        "zeus_checked_signed_integer",
        "zeus_checked_unsigned_integer",
        "zeus_value_require_i64",
        "zeus_value_require_bool",
        "zeus_value_require_double",
        "zeus_value_require_float",
        "zeus_value_require_signed_integer",
        "zeus_value_require_unsigned_integer",
        "zeus_global_store_i64",
        "zeus_global_load_i64",
        "zeus_global_store_bool",
        "zeus_global_load_bool",
        "zeus_global_store_unsigned_integer",
        "zeus_global_load_unsigned_integer",
        "zeus_global_store_float",
        "zeus_global_load_float",
        "zeus_global_store_double",
        "zeus_global_load_double",
        "zeus_value_init_nil",
        "zeus_value_init_i64",
        "zeus_value_init_bool",
        "zeus_native_library_symbol",
        "zeus_try_depth_reset",
        "zeus_i64_check_division",
        "zeus_value_release",
        "memset",
        "free",
    };
    if (name.rfind("llvm.", 0) == 0) return true;
    for (const auto& entry : safe) {
        if (entry == name) return true;
    }
    return false;
}

// Removes "collect if needed" polls from functions that provably cannot
// allocate: every call/invoke in the body must resolve to the GC-safe
// allowlist above (or to another provably pure program function, by
// fixpoint). Indirect calls and unknown targets are impure by construction.
// Skipping collection is safe exactly when nothing collectible is created;
// the safepoint calls themselves are ignored while deciding.
inline void stripUnneededSafepoints(std::string& module) {
    struct FunctionBody {
        std::string name;
        size_t start = 0;
        size_t end = 0;
        std::vector<std::string> callees;
        bool pure = true;
    };
    std::vector<FunctionBody> functions;
    size_t cursor = 0;
    while (true) {
        size_t definePos = module.find("define ", cursor);
        if (definePos != std::string::npos &&
            (definePos != 0 && module[definePos - 1] != '\n')) {
            cursor = definePos + 7;
            continue;
        }
        if (definePos == std::string::npos) break;
        size_t bodyEnd = module.find("\n}\n", definePos);
        bodyEnd = bodyEnd == std::string::npos ? module.size() : bodyEnd + 3;
        FunctionBody body;
        body.start = definePos == 0 ? 0 : definePos;
        body.end = bodyEnd;
        const size_t at = module.find('@', definePos);
        const size_t open = at == std::string::npos
            ? std::string::npos : module.find('(', at + 1);
        if (at != std::string::npos && open != std::string::npos && open < bodyEnd) {
            if (module[at + 1] == '"') {
                const size_t close = module.find('"', at + 2);
                if (close != std::string::npos && close < open) {
                    body.name = module.substr(at + 2, close - at - 2);
                }
            } else {
                body.name = module.substr(at + 1, open - at - 1);
            }
        }
        // Collect call targets line by line; comment lines cannot contain
        // real calls, indirect calls have no @target and mark impurity.
        size_t lineStart = definePos;
        while (lineStart < bodyEnd) {
            const size_t lineEnd = module.find('\n', lineStart);
            const size_t end = lineEnd == std::string::npos
                ? module.size() : lineEnd;
            std::string line = module.substr(lineStart, end - lineStart);
            const size_t first = line.find_first_not_of(" \t\r");
            if (first != std::string::npos && line[first] != ';') {
                const size_t callPos = line.find("call ");
                const size_t invokePos = line.find("invoke ");
                size_t opPos = std::string::npos;
                if (callPos != std::string::npos &&
                    (invokePos == std::string::npos || callPos < invokePos)) {
                    opPos = callPos;
                } else if (invokePos != std::string::npos) {
                    opPos = invokePos;
                }
                if (opPos != std::string::npos) {
                    const size_t target = line.find('@', opPos);
                    if (target == std::string::npos) {
                        body.callees.emplace_back();
                    } else if (target + 1 < line.size() && line[target + 1] == '"') {
                        const size_t close = line.find('"', target + 2);
                        body.callees.push_back(close == std::string::npos
                            ? std::string()
                            : line.substr(target + 2, close - target - 2));
                    } else {
                        size_t nameEnd = target + 1;
                        while (nameEnd < line.size() &&
                            (std::isalnum(static_cast<unsigned char>(line[nameEnd])) ||
                             line[nameEnd] == '_' || line[nameEnd] == '.' ||
                             line[nameEnd] == '$')) {
                            ++nameEnd;
                        }
                        body.callees.push_back(line.substr(target + 1, nameEnd - target - 1));
                    }
                }
            }
            if (lineEnd == std::string::npos || lineEnd >= bodyEnd) break;
            lineStart = lineEnd + 1;
        }
        functions.push_back(std::move(body));
        cursor = bodyEnd;
    }
    std::map<std::string, size_t> indexByName;
    for (size_t i = 0; i < functions.size(); ++i) {
        if (!functions[i].name.empty()) indexByName[functions[i].name] = i;
    }
    // Greatest fixpoint: everything starts pure; a function drops out when
    // any callee is unknown, indirect, non-allowlisted, or itself impure.
    // Safepoint polls are the subject of this pass and never count.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& function : functions) {
            if (!function.pure) continue;
            for (const auto& callee : function.callees) {
                if (callee == "zeus_gc_safepoint" || isGcSafeRuntimeCall(callee)) {
                    continue;
                }
                const auto found = indexByName.find(callee);
                if (found == indexByName.end() || !functions[found->second].pure) {
                    function.pure = false;
                    changed = true;
                    break;
                }
            }
        }
    }
    for (const auto& function : functions) {
        if (!function.pure) continue;
        const std::string marker = "call void @zeus_gc_safepoint()";
        size_t at = function.start;
        while (true) {
            const size_t found = module.find(marker, at);
            if (found == std::string::npos || found >= function.end) break;
            size_t lineStart = module.rfind('\n', found);
            lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
            size_t lineEnd = module.find('\n', found);
            lineEnd = lineEnd == std::string::npos ? module.size() : lineEnd + 1;
            std::string head = module.substr(lineStart, found - lineStart);
            const bool blankHead = head.find_first_not_of(" \t\r") == std::string::npos;
            std::string tail = module.substr(found + marker.size(),
                lineEnd - found - marker.size());
            const bool blankTail = tail.find_first_not_of(" \t\r\n") == std::string::npos;
            if (blankHead && blankTail) {
                module.erase(lineStart, lineEnd - lineStart);
                const size_t erased = lineEnd - lineStart;
                at = lineStart;
                for (auto& other : functions) {
                    if (other.start > lineStart) {
                        other.start -= erased;
                        other.end -= erased;
                    } else if (other.end > lineStart) {
                        other.end -= erased;
                    }
                }
            } else {
                at = found + marker.size();
            }
        }
    }
}

} // namespace detail

inline EmitResult emit(const ZeusCompile::Result& r) {
    EmitResult res;
    detail::Emitter e;
    e.mcuTarget = r.mcuTarget;
    e.programChunks = &r.program;
    // 失败判定统一为 error 字符串（fail() 负责填充）。
    if (r.program.empty()) {
        res.ok = false;
        res.error = "[emit-llvm] 空程序";
        return res;
    }

    if (!e.collectMetadata(r)) {
        res.ok = false;
        res.error = e.result.error;
        res.errorLine = e.result.errorLine;
        return res;
    }

    // Both passes need to know what each global holds before any code is
    // emitted, so the scan runs on its own here.
    e.prescanGlobalTypes();
    e.collectReachableFunctionChunks(r.program);
    e.prescanLivenessElision();
    size_t availableFunctions = 0;
    size_t retainedFunctions = 0;
    for (const auto& function : e.functionSignaturesByChunk) {
        if (function.second.isGenericTemplate) continue;
        ++availableFunctions;
        if (e.reachableFunctionChunks.find(function.first) !=
            e.reachableFunctionChunks.end()) {
            ++retainedFunctions;
        }
    }
    std::fprintf(stdout, "[emit-llvm] reachable functions %zu/%zu\n",
        retainedFunctions, availableFunctions);
    for (size_t ci = 0; ci < r.program.size(); ++ci) {
        if (!r.program[ci].empty() &&
            r.program[ci].front().metadata.kind == IRType::IRMetadataKind::ModuleImportInit) {
            if (!e.emitChunk({}, ci, ci + 1 == r.program.size())) {
                res.ok = false;
                res.error = e.result.error;
                res.errorLine = e.result.errorLine;
                res.warnings = e.result.warnings;
                return res;
            }
            continue;
        }
        if (!e.emitChunk(r.program[ci], ci, ci + 1 == r.program.size())) {
            res.ok = false;
            res.error = e.result.error;
            res.errorLine = e.result.errorLine;
            res.warnings = e.result.warnings;
            return res;
        }
    }

    if (!e.pendingVariadicCall.empty() || !e.pendingVariadicCallStack.empty()) {
        res.ok = false;
        res.error = "[emit-llvm] 悬空的动态变长函数加载（LOAD 后无 CALL）";
        res.warnings = e.result.warnings;
        return res;
    }

    if (!e.pendingDirectCall.empty() || !e.pendingFunctionLoad.empty()) {
        res.ok = false;
        res.error = "[emit-llvm] 悬空的函数直接调用标注";
        res.warnings = e.result.warnings;
        return res;
    }

    // Code emission discovers native embedded-slot layouts. Close the class
    // graph after that pass, then remove only layouts with no incoming edge.
    e.collectReachableClassLayouts(r.program, nullptr);
    e.pruneUnreachableClassStructTypes();
    size_t retainedClassRuntimeHelpers = 0;
    for (const auto& typeName : e.reachableClassLayouts) {
        if (e.classNeedsRuntimeMachinery(typeName)) {
            ++retainedClassRuntimeHelpers;
        }
    }
    std::fprintf(stdout,
        "[emit-llvm] reachable class layouts %zu/%zu; runtime helpers %zu; dynamic new %s\n",
        e.reachableClassLayouts.size(), e.classLayouts.size(),
        retainedClassRuntimeHelpers,
        e.programHasDynamicClassConstruction ? "yes" : "no");

    if (!e.buildDynamicMemberBridge()) {
        res.ok = false;
        res.error = e.result.error;
        res.errorLine = e.result.errorLine;
        res.warnings = e.result.warnings;
        return res;
    }
    if (!e.buildDynamicFieldBridge()) {
        res.ok = false;
        res.error = e.result.error;
        res.errorLine = e.result.errorLine;
        res.warnings = e.result.warnings;
        return res;
    }

    size_t retainedExternalDeclarations = 0;
    for (const auto& declaration : e.externalDeclarationByFunction) {
        if (!e.keepAllExternalDeclarations &&
            e.reachableExternalFunctions.find(declaration.first) ==
                e.reachableExternalFunctions.end()) {
            continue;
        }
        e.externalDeclarations += declaration.second;
        ++retainedExternalDeclarations;
    }
    if (!e.externalDeclarationByFunction.empty()) {
        std::fprintf(stdout,
            "[emit-llvm] external declarations retained %zu/%zu\n",
            retainedExternalDeclarations, e.externalDeclarationByFunction.size());
    }

    size_t runtimeErrorMessageField = 0;
    size_t runtimeErrorTypeField = 0;
    size_t typeErrorMessageField = 0;
    size_t typeErrorTypeField = 0;
    if (e.needsRuntimeErrorFactory) {
        const auto layout = e.classLayouts.find("RuntimeError");
        if (layout == e.classLayouts.end() || layout->second.fieldNames.size() != 2 ||
            layout->second.fieldTypes.size() != 2) {
            res.ok = false;
            res.error = "[emit-llvm] RuntimeError class must have _message and _type string fields";
            return res;
        }
        const auto& names = layout->second.fieldNames;
        const auto message = std::find(names.begin(), names.end(), "_message");
        const auto type = std::find(names.begin(), names.end(), "_type");
        if (message == names.end() || type == names.end() ||
            message == type ||
            e.llvmFieldType(layout->second.fieldTypes[message - names.begin()]) != "ptr" ||
            e.llvmFieldType(layout->second.fieldTypes[type - names.begin()]) != "ptr") {
            res.ok = false;
            res.error = "[emit-llvm] RuntimeError fields have incompatible LLVM types";
            return res;
        }
        runtimeErrorMessageField = static_cast<size_t>(message - names.begin());
        runtimeErrorTypeField = static_cast<size_t>(type - names.begin());

        const auto typeErrorLayout = e.classLayouts.find("TypeError");
        if (typeErrorLayout == e.classLayouts.end() ||
            typeErrorLayout->second.fieldNames.size() != 2 ||
            typeErrorLayout->second.fieldTypes.size() != 2) {
            res.ok = false;
            res.error = "[emit-llvm] TypeError class must have _message and _type string fields";
            return res;
        }
        const auto& typeErrorNames = typeErrorLayout->second.fieldNames;
        const auto typeErrorMessage = std::find(
            typeErrorNames.begin(), typeErrorNames.end(), "_message");
        const auto typeErrorType = std::find(
            typeErrorNames.begin(), typeErrorNames.end(), "_type");
        if (typeErrorMessage == typeErrorNames.end() ||
            typeErrorType == typeErrorNames.end() ||
            typeErrorMessage == typeErrorType ||
            e.llvmFieldType(typeErrorLayout->second.fieldTypes[
                typeErrorMessage - typeErrorNames.begin()]) != "ptr" ||
            e.llvmFieldType(typeErrorLayout->second.fieldTypes[
                typeErrorType - typeErrorNames.begin()]) != "ptr") {
            res.ok = false;
            res.error = "[emit-llvm] TypeError fields have incompatible LLVM types";
            return res;
        }
        typeErrorMessageField = static_cast<size_t>(
            typeErrorMessage - typeErrorNames.begin());
        typeErrorTypeField = static_cast<size_t>(
            typeErrorType - typeErrorNames.begin());
    }

    // Slot helpers add class names to the string pool. Build them before the
    // pool is serialized into the module so every referenced symbol exists.
    const std::string classEmbeddedSlotHelpers =
        e.emitClassEmbeddedSlotHelpers();
    const std::string classCopyHelpers = e.emitClassCopyHelpers();

    // 组装：声明+ 字符串池 + 入口。
    std::string out;
    out += "; Generated by Zeus --emit-llvm (v0 naive stack machine)\n";
    out += "; 入口 zeus_entry 由所有 top-level chunk 按序拼接，main 链接 libzeus_rt.lib。\n\n";
    out += "%ZeusValue = type { i32, i32, i64 }\n";
    out += "%ZeusRawBytesRange = type { ptr, i64 }\n";
    out += "declare void @zeus_value_init_nil(ptr)\n";
    out += "declare void @zeus_value_init_double(ptr, double)\n";
    out += "declare void @zeus_value_init_float(ptr, float)\n";
    out += "declare void @zeus_value_init_unsigned_integer(ptr, i64, i32)\n";
    out += "declare double @zeus_value_require_double(ptr)\n";
    out += "declare float @zeus_value_require_float(ptr)\n";
    out += "declare float @zeus_double_require_float(double)\n";
    out += "declare double @zeus_global_load_double(i32)\n";
    out += "declare void @zeus_global_store_double(i32, double)\n";
    out += "declare float @zeus_global_load_float(i32)\n";
    out += "declare void @zeus_global_store_float(i32, float)\n";
    out += "declare i64 @zeus_global_load_unsigned_integer(i32, i32)\n";
    out += "declare void @zeus_global_store_unsigned_integer(i32, i64, i32)\n";
    out += "declare void @zeus_value_init_i64(ptr, i64)\n";
    out += "declare void @zeus_value_init_bool(ptr, i32)\n";
    out += "declare void @zeus_value_from_i64_ptr(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bool_ptr(ptr, ptr)\n";
    out += "declare void @zeus_value_clone(ptr, ptr)\n";
    out += "declare void @zeus_value_from_class_ptr(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_value_require_class_ptr(ptr, ptr)\n";
    out += "declare ptr @zeus_value_try_class_ptr(ptr, ptr)\n";
    out += "declare void @zeus_class_pointer_invalidate(ptr)\n";
    out += "declare void @zeus_class_ref_register(ptr, ptr, ptr)\n";
    out += "declare void @zeus_class_ref_register_embedded(ptr, ptr, ptr, i64)\n";
    out += "declare void @zeus_class_gc_register(ptr, ptr)\n";
    out += "declare void @zeus_class_ref_retain(ptr)\n";
    out += "declare void @zeus_class_ref_require_managed_live(ptr)\n";
    out += "declare void @zeus_class_ref_retain_owned(ptr)\n";
    out += "declare void @zeus_class_ref_release(ptr)\n";
    out += "declare void @zeus_class_ref_retain_edge(ptr)\n";
    out += "declare void @zeus_class_ref_release_edge(ptr)\n";
    out += "declare i32 @zeus_class_ref_destroy_now(ptr)\n";
    out += "declare void @zeus_class_ref_require_live(ptr)\n";
    out += "declare i32 @zeus_class_ref_embedded_slot_state(ptr, ptr, i64)\n";
    out += "declare ptr @zeus_class_ref_embedded_slot_object(ptr, ptr, i64)\n";
    out += "declare ptr @zeus_class_ref_embedded_slot_create(ptr, ptr, ptr, i64)\n";
    out += "declare void @zeus_class_ref_embedded_slot_assign(ptr, ptr, ptr, ptr, i64)\n";
    out += "declare void @zeus_class_ref_embedded_slot_clear(ptr, ptr, i64)\n";
    out += "declare void @zeus_class_ref_embedded_set_initialized(ptr, i32)\n";
    out += "declare void @zeus_class_ref_require_embedded_live(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_string_data(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_native_string(ptr, ptr)\n";
    out += "declare void @zeus_value_from_native_bytes(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bytes(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_bigint_decimal(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bigdecimal_decimal(ptr, ptr)\n";
    out += "declare i64 @zeus_value_require_i64(ptr)\n";
    out += "declare i64 @zeus_value_require_signed_integer(ptr, i32)\n";
    out += "declare i64 @zeus_double_require_signed_integer(double, i32)\n";
    out += "declare i64 @zeus_checked_signed_integer(i64, i32)\n";
    out += "declare i64 @zeus_value_require_unsigned_integer(ptr, i32)\n";
    out += "declare i64 @zeus_double_require_unsigned_integer(double, i32)\n";
    out += "declare i64 @zeus_checked_unsigned_integer(i64, i32)\n";
    out += "declare i64 @zeus_signed_require_unsigned_integer(i64, i32)\n";
    out += "declare i64 @zeus_unsigned_require_signed_integer(i64, i32)\n";
    out += "declare i32 @zeus_value_require_bool(ptr)\n";
    out += "declare i64 @zeus_value_require_argument_i64(ptr, ptr, i32, ptr)\n";
    out += "declare i32 @zeus_value_require_argument_bool(ptr, ptr, i32, ptr)\n";
    out += "declare i32 @zeus_value_require_char(ptr)\n";
    out += "declare void @zeus_value_from_char(ptr, i32)\n";
    out += "declare ptr @zeus_value_native_address(ptr, i32)\n";
    out += "declare ptr @zeus_value_to_native_string(ptr)\n";
    out += "declare ptr @zeus_value_copy_bytes(ptr)\n";
    out += "declare ptr @zeus_string_literal_from_data(ptr, i64)\n";
    out += "declare ptr @zeus_string_from_data(ptr, i64)\n";
    out += "declare ptr @zeus_string_retain(ptr)\n";
    out += "declare void @zeus_string_release(ptr)\n";
    out += "declare void @zeus_string_store_slot(ptr, ptr)\n";
    out += "declare ptr @zeus_string_from_cstr(ptr)\n";
    out += "declare ptr @zeus_native_library_symbol(ptr, ptr)\n";
    out += "declare ptr @zeus_string_copy_from_data(ptr, i64)\n";
    out += "declare i64 @zeus_string_data_address(ptr)\n";
    out += "declare ptr @zeus_native_string_concat(ptr, ptr)\n";
    out += "declare i1 @zeus_native_string_equal(ptr, ptr)\n";
    out += "declare ptr @zeus_bytes_from_data(ptr, i64)\n";
    out += "declare ptr @zeus_bytes_copy_from_address(i64, i64)\n";
    out += "declare ptr @zeus_bytes_clone(ptr)\n";
    out += "declare void @zeus_bytes_release(ptr)\n";
    out += "declare void @zeus_bytes_store_slot(ptr, ptr, i32)\n";
    out += "declare void @zeus_bytes_copy_to_address(ptr, i64, i64)\n";
    out += "declare void @zeus_bytes_store_raw_range(ptr, i64, i64)\n";
    out += "declare void @zeus_raw_bytes_range_validate(i64, i64)\n";
    out += "declare void @zeus_raw_bytes_range_store(i64, i64, i64, i64)\n";
    out += "declare void @zeus_raw_bytes_range_copy(i64, i64, i64)\n";
    out += "declare i64 @zeus_raw_bytes_range_get(i64, i64, i64)\n";
    out += "declare void @zeus_raw_bytes_range_set(i64, i64, i64, i64)\n";
    out += "declare void @zeus_value_from_raw_bytes_range(ptr, i64, i64)\n";
    out += "declare void @zeus_value_require_raw_bytes_range(ptr, ptr)\n";
    out += "declare i64 @zeus_bytes_data_address(ptr)\n";
    out += "declare i64 @zeus_bytes_len(ptr)\n";
    out += "declare i64 @zeus_bytes_get(ptr, i64)\n";
    out += e.isMCUMode() ? std::string() : e.bytesFastPathDefinitions();
    out += "declare ptr @zeus_bytes_concat(ptr, ptr)\n";
    out += "declare ptr @zeus_bytes_slice(ptr, i64, i64)\n";
    out += "declare ptr @zeus_bytes_decode(ptr, ptr)\n";
    out += "declare void @zeus_bytes_to_list(ptr, ptr)\n";
    out += "declare ptr @zeus_value_require_i64_ptr(ptr)\n";
    out += "declare ptr @zeus_value_require_bool_ptr(ptr)\n";
    out += "declare void @zeus_value_release(ptr)\n";
    out += "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n";
    out += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)\n";
    out += "declare void @zeus_range_check_i64(i64, i32, i32)\n";
    out += "declare void @zeus_range_check_value(ptr, i32, i32)\n";
    out += "declare void @zeus_value_push(ptr)\n";
    out += "declare void @zeus_value_pop(ptr)\n";
    out += "declare void @zeus_value_print(ptr)\n";
    out += "declare void @zeus_value_print_call(ptr, i32, ptr)\n";
    out += "declare void @zeus_value_native_call(ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_dynamic_member_register(ptr, ptr, ptr)\n";
    out += "declare void @zeus_dynamic_field_register(ptr, ptr, ptr)\n";
    out += "declare void @zeus_dynamic_field_trace_register(ptr, ptr, ptr, ptr)\n";
    out += "declare void @zeus_dynamic_field_setter_register(ptr, ptr, i32, ptr, ptr)\n";
    out += "declare void @zeus_value_dynamic_field_load(ptr, ptr, ptr, i32)\n";
    out += "declare void @zeus_value_dynamic_attribute_load(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_value_dynamic_safe_attribute_load(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_value_dynamic_field_store(ptr, ptr, ptr, i32)\n";
    out += "declare void @zeus_value_dynamic_field_replace_state(ptr, ptr)\n";
    out += "declare void @zeus_value_dynamic_field_object_destroy(ptr)\n";
    out += "declare void @zeus_value_dynamic_field_prepare(ptr, ptr)\n";
    out += "declare i32 @zeus_value_try_dynamic_member_call(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_value_dynamic_member_call(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_value_dynamic_safe_member_call(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_dynamic_member_arity_error(ptr, i32, i32)\n";
    out += "declare void @zeus_dynamic_member_arity_range_error(ptr, i32, i32, i32)\n";
    out += "declare void @zeus_value_global_store(i32, ptr)\n";
    out += "declare void @zeus_value_global_load(ptr, i32)\n";
    out += "declare ptr @zeus_value_global_slot(i32)\n";
    out += "declare void @zeus_global_clear(i32)\n";
    out += "declare void @zeus_value_global_delete(i32)\n";
    out += "declare void @zeus_value_local_delete(i32)\n";
    out += "declare void @zeus_value_delete_object(ptr)\n";
    out += "declare void @zeus_shared_value_store(ptr, ptr)\n";
    out += "declare void @zeus_shared_values_release()\n";
    out += "declare void @zeus_shared_value_assign(ptr, ptr)\n";
    out += "declare void @zeus_shared_value_load(ptr, ptr)\n";
    out += "declare void @zeus_shared_value_store_i64(ptr, i64)\n";
    out += "declare void @zeus_shared_value_assign_i64(ptr, i64)\n";
    out += "declare i64 @zeus_shared_value_load_i64(ptr)\n";
    out += "declare void @zeus_shared_value_store_bool(ptr, i32)\n";
    out += "declare void @zeus_shared_value_assign_bool(ptr, i32)\n";
    out += "declare i32 @zeus_shared_value_load_bool(ptr)\n";
    out += "declare ptr @zeus_shared_value_native_address(ptr, i32)\n";
    out += "declare i32 @zeus_shared_value_delete(ptr)\n";
    out += "declare i64 @zeus_global_load_i64(i32)\n";
    out += "declare i32 @zeus_global_load_bool(i32)\n";
    out += "declare void @zeus_global_store_i64(i32, i64)\n";
    out += "declare void @zeus_global_store_bool(i32, i32)\n";
    out += "declare void @zeus_global_store_string(i32, ptr)\n";
    out += "declare ptr @zeus_global_load_string(i32)\n";
    out += "declare ptr @zeus_global_string_slot(i32)\n";
    out += "declare void @zeus_global_store_bytes(i32, ptr)\n";
    out += "declare ptr @zeus_global_load_bytes(i32)\n";
    out += "declare ptr @zeus_global_bytes_slot(i32)\n";
    out += "declare void @zeus_value_local_store(i32, ptr)\n";
    out += "declare void @zeus_value_local_load(ptr, i32)\n";
    out += "declare ptr @zeus_capture_cell_create(ptr)\n";
    out += "declare ptr @zeus_capture_cell_clone(ptr)\n";
    out += "declare ptr @zeus_capture_cell_retain(ptr)\n";
    out += "declare void @zeus_capture_cell_release(ptr)\n";
    out += "declare void @zeus_capture_cell_load(ptr, ptr)\n";
    out += "declare void @zeus_capture_cell_store(ptr, ptr)\n";
    out += "declare ptr @zeus_capture_cell_value_slot(ptr)\n";
    out += "declare ptr @zeus_closure_env_value_slot(ptr, i32)\n";
    out += "declare void @zeus_value_slot_load(ptr, ptr)\n";
    out += "declare i64 @zeus_value_slot_load_i64(ptr)\n";
    out += "declare i32 @zeus_value_slot_load_bool(ptr)\n";
    out += "declare ptr @zeus_value_slot_i64_pointer(ptr)\n";
    out += "declare ptr @zeus_value_slot_bool_pointer(ptr)\n";
    out += "declare void @zeus_value_slot_store(ptr, ptr)\n";
    out += "declare void @zeus_value_slot_register_gc_edge(ptr)\n";
    out += "declare void @zeus_value_slot_unregister_gc_edge(ptr)\n";
    out += "declare void @zeus_value_slot_store_i64(ptr, i64)\n";
    out += "declare void @zeus_value_slot_store_bool(ptr, i32)\n";
    out += "declare void @zeus_value_slot_validate_argument_type(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_capture_cell_delete(ptr)\n";
    out += "declare ptr @zeus_closure_capture_ref(ptr, i32)\n";
    out += "declare ptr @zeus_closure_capture_copy(ptr, i32)\n";
    out += "declare void @zeus_closure_env_delete(ptr, i32)\n";
    out += "declare void @zeus_closure_create(ptr, ptr, i32, i32, ptr)\n";
    out += "declare void @zeus_closure_create_range(ptr, ptr, i32, i32, i32, ptr)\n";
    out += "declare void @zeus_closure_call(ptr, ptr, i32, ptr, ptr)\n";
    out += "declare void @zeus_closure_call_named(ptr, ptr, i32, ptr, ptr, ptr)\n";
    out += "declare void @zeus_closure_bind_arguments(i32, ptr, i64, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr)\n";
    out += "declare void @zeus_closure_env_load(ptr, ptr, i32)\n";
    out += "declare void @zeus_closure_env_store(ptr, i32, ptr)\n";
    out += "declare void @zeus_gc_collect()\n";
    out += "declare void @zeus_gc_safepoint()\n";
    out += "declare i64 @zeus_gc_live_node_count()\n";
    out += "declare void @zeus_value_int_add(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_less_than(ptr, ptr, ptr)\n";
    out += "declare i32 @zeus_value_truthy(ptr)\n";
    out += "declare ptr @malloc(i64)\n";
    out += "declare void @zeus_oom_abort(ptr, ptr) noreturn\n";
    out += "declare void @free(ptr)\n";
    out += "declare void @zeus_class_field_require_initialized(i32, ptr, ptr, i32)\n";
    out += "declare void @zeus_exception_raise(ptr)\n";
    out += "declare void @zeus_exception_raise_owned(ptr)\n";
    out += "declare void @zeus_exception_capture(ptr, ptr, ptr)\n";
    out += "declare void @zeus_try_enter()\n";
    out += "declare void @zeus_try_exit()\n";
    out += "declare void @zeus_try_depth_reset()\n";
    out += "declare void @zeus_value_validate_argument_type(ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_i64_check_division(i64, i64, i32)\n";
    if (!e.isMCUMode()) {
        out += "declare i32 @__CxxFrameHandler3(...)\n";
    }
    out += "declare void @llvm.trap()\n\n";
    DynamicABI::appendLLVMDeclarations(out);
    DynamicBinaryABI::appendLLVMDeclarations(out);
    out += "\n";

    for (const auto& typeName : e.structTypes) {
        auto layout = e.classLayouts.find(typeName);
        if (layout == e.classLayouts.end()) {
            out += e.llvmStructType(typeName) + " = type { i8 }\n";
            continue;
        }

        out += e.llvmStructType(typeName) + " = type {";
        for (size_t i = 0; i < layout->second.fieldTypes.size(); ++i) {
            if (i > 0) out += ",";
            out += " " + e.llvmFieldType(layout->second.fieldTypes[i]);
        }
        out += " }\n";
    }

    if (!e.structTypes.empty()) out += "\n";

    for (const auto& kv : e.strPool) {
        const std::string& s = kv.first;
        const std::string& sym = kv.second;
        out += sym + " = private unnamed_addr constant [" + std::to_string(s.size() + 1) +
               " x i8] c\"" + detail::escapeLLString(s) + "\\00\", align 1\n";
    }

    if (!e.strPool.empty()) out += "\n";
    out += e.dynamicMemberBridge.constants;
    if (!e.dynamicMemberBridge.constants.empty()) out += "\n";
    out += e.dynamicFieldBridge.constants;
    if (!e.dynamicFieldBridge.constants.empty()) out += "\n";

    out += e.externalDeclarations;
    if (e.needsRuntimeErrorFactory) {
        out += "@.zeus_runtime_error_type = private unnamed_addr constant [13 x i8] c\"RuntimeError\\00\", align 1\n\n";
        out += "define void @__zeus_make_runtime_error(ptr %out, ptr %message, i64 %length) {\n";
        out += "entry:\n";
        out += "  %size.ptr = getelementptr %zeus_RuntimeError, ptr null, i32 1\n";
        out += "  %size = ptrtoint ptr %size.ptr to i64\n";
        out += "  %object = call ptr @malloc(i64 %size)\n";
        out += "  %failed = icmp eq ptr %object, null\n";
        out += "  br i1 %failed, label %out.of.memory, label %ready\n";
        out += "out.of.memory:\n";
        out += "  call void @llvm.trap()\n";
        out += "  unreachable\n";
        out += "ready:\n";
        out += "  %message.field = getelementptr inbounds %zeus_RuntimeError, ptr %object, i32 0, i32 " +
            std::to_string(runtimeErrorMessageField) + "\n";
        out += "  %message.view = call ptr @zeus_string_from_data(ptr %message, i64 %length)\n";
        out += "  store ptr %message.view, ptr %message.field\n";
        out += "  %type.field = getelementptr inbounds %zeus_RuntimeError, ptr %object, i32 0, i32 " +
            std::to_string(runtimeErrorTypeField) + "\n";
        out += "  %type.view = call ptr @zeus_string_literal_from_data(ptr @.zeus_runtime_error_type, i64 12)\n";
        out += "  store ptr %type.view, ptr %type.field\n";
        out += "  call void @zeus_class_ref_register(ptr %object, ptr " +
            e.classLifetimeFinalizer("RuntimeError") + ", ptr @.zeus_runtime_error_type)\n";
        out += "  call void @zeus_value_from_class_ptr(ptr %out, ptr %object, ptr @.zeus_runtime_error_type)\n";
        out += "  call void @zeus_class_ref_release(ptr %object)\n";
        out += "  ret void\n";
        out += "}\n\n";

        out += "@.zeus_type_error_type = private unnamed_addr constant [10 x i8] c\"TypeError\\00\", align 1\n\n";
        out += "define void @__zeus_make_type_error(ptr %out, ptr %message, i64 %length) {\n";
        out += "entry:\n";
        out += "  %size.ptr = getelementptr %zeus_TypeError, ptr null, i32 1\n";
        out += "  %size = ptrtoint ptr %size.ptr to i64\n";
        out += "  %object = call ptr @malloc(i64 %size)\n";
        out += "  %failed = icmp eq ptr %object, null\n";
        out += "  br i1 %failed, label %out.of.memory, label %ready\n";
        out += "out.of.memory:\n";
        out += "  call void @llvm.trap()\n";
        out += "  unreachable\n";
        out += "ready:\n";
        out += "  %message.field = getelementptr inbounds %zeus_TypeError, ptr %object, i32 0, i32 " +
            std::to_string(typeErrorMessageField) + "\n";
        out += "  %message.view = call ptr @zeus_string_from_data(ptr %message, i64 %length)\n";
        out += "  store ptr %message.view, ptr %message.field\n";
        out += "  %type.field = getelementptr inbounds %zeus_TypeError, ptr %object, i32 0, i32 " +
            std::to_string(typeErrorTypeField) + "\n";
        out += "  %type.view = call ptr @zeus_string_literal_from_data(ptr @.zeus_type_error_type, i64 9)\n";
        out += "  store ptr %type.view, ptr %type.field\n";
        out += "  call void @zeus_class_ref_register(ptr %object, ptr " +
            e.classLifetimeFinalizer("TypeError") + ", ptr @.zeus_type_error_type)\n";
        out += "  call void @zeus_value_from_class_ptr(ptr %out, ptr %object, ptr @.zeus_type_error_type)\n";
        out += "  call void @zeus_class_ref_release(ptr %object)\n";
        out += "  ret void\n";
        out += "}\n\n";
    }
    out += e.emitClassLifetimeFinalizers();
    out += classEmbeddedSlotHelpers;
    out += classCopyHelpers;
    out += e.nativeFunctions;
    out += e.boundMethodAdapters;
    out += e.dynamicMemberBridge.adapters;
    out += e.dynamicFieldBridge.adapters;

    out += e.isMCUMode()
        ? "define i32 @zeus_entry() {\nentry:\n"
        : "define i32 @zeus_entry() personality ptr @__CxxFrameHandler3 {\nentry:\n";
    // The top-level statements all end up in this one function, so their
    // allocas are hoisted the same way a native function hoists its own: at
    // -O0 an alloca inside a loop grows the frame on every iteration.
    std::string hoistedAllocas;
    std::string hoistedBody = detail::Emitter::takeAllocas(e.body, 0, hoistedAllocas);
    std::vector<std::string> entryExitCleanupCalls =
        e.compactEntryGlobalCleanupCalls(e.entryExceptionCleanupCalls());
    for (auto& pad : e.entryExceptionPads) {
        pad.cleanupCalls = e.compactEntryGlobalCleanupCalls(pad.cleanupCalls);
    }
    e.deduplicateEntryCleanupPads(hoistedBody);
    for (const auto& entry : e.classLayouts) {
        // See classNeedsRuntimeMachinery: registration without live
        // instances only pins class machinery into the binary.
        if (!e.classNeedsRuntimeMachinery(entry.first)) {
            continue;
        }
        out += "  call void @zeus_class_copy_register(ptr " + e.poolStr(entry.first) +
            ", ptr " + e.classCopyCallback(entry.first) + ")\n";
        out += "  call void @zeus_class_copy_register_default(ptr " + e.poolStr(entry.first) +
            ", ptr " + e.classCopyDefaultCallback(entry.first) + ")\n";
    }
    out += e.takeEntryAllocas();
    out += hoistedAllocas;
    if (!e.dynamicMemberBridge.empty()) {
        out += "  call void @__zeus_register_dynamic_members()\n";
    }
    if (!e.dynamicFieldBridge.empty()) {
        out += "  call void @__zeus_register_dynamic_fields()\n";
    }
    out += "  br label %L0_0\n";
    out += hoistedBody;

    // 收尾：上一块若已终结（末尾是无条件跳转，如死循环），补不可达块。
    // 判定：取 body 最后一行，看是否为 "br label ..."。
    bool endsWithBr = false;

    {
        // 取 body 最后一行（body 以换行结尾，out 尾部还有 "}\n" 未加——）。
        // 注意此处检查的是 e.body，不是 out）。
        std::string b = e.body;

        while (!b.empty() && (b.back() == '\n' || b.back() == ' ')) {
            b.pop_back();
        }
        // 取最后一行。
        size_t nl = b.rfind('\n');
        std::string last = (nl == std::string::npos) ? b : b.substr(nl + 1);

        // 去前导空白。
        size_t p = last.find_first_not_of(" \t");

        if (p != std::string::npos) last = last.substr(p);

        endsWithBr = (last.rfind("br label ", 0) == 0);
    }

    if (endsWithBr) {
        out += "Lend_unreachable:\n  unreachable\n";
    } else {
        for (const auto& cleanupCall : entryExitCleanupCalls) {
            out += "  " + cleanupCall + "\n";
        }
        if (e.entryHasTry) {
            // The top-level entry is the outermost frame: any try region still
            // open here (e.g. an abnormal jump) must not leak into later code.
            out += "  call void @zeus_try_depth_reset()\n";
        }
        out += "  call void @zeus_gc_safepoint()\n";
        out += "  ret i32 0\n";
    }

    for (size_t i = 0; i < e.entryExceptionPads.size(); ++i) {
        if (e.isMCUMode()) break;
        const auto& pad = e.entryExceptionPads[i];
        if (!pad.emit) continue;
        if (pad.cleanupOnly) {
            const std::string cleanupToken = "%entry_eh_cleanup_token_" +
                std::to_string(i);
            out += pad.switchLabel.substr(1) + ":\n";
            out += "  " + cleanupToken + " = cleanuppad within none []\n";
            for (const auto& cleanupCall : pad.cleanupCalls) {
                out += "  " + cleanupCall;
                if (cleanupCall.rfind("call ", 0) == 0) {
                    out += " [ \"funclet\"(token " + cleanupToken + ") ]";
                }
                out += "\n";
            }
            out += "  cleanupret from " + cleanupToken +
                " unwind to caller\n";
            continue;
        }
        const std::string index = std::to_string(i);
        const std::string switchToken = "%entry_eh_switch_token_" + index;
        const std::string catchToken = "%entry_eh_catch_token_" + index;
        out += pad.switchLabel.substr(1) + ":\n";
        out += "  " + switchToken + " = catchswitch within none [label " +
            pad.padLabel + "] unwind to caller\n";
        out += pad.padLabel.substr(1) + ":\n";
        out += "  " + catchToken + " = catchpad within " + switchToken +
            " [ptr null, i32 64, ptr null]\n";
        for (const auto& cleanupCall : pad.cleanupCalls) {
            out += "  " + cleanupCall + " [ \"funclet\"(token " +
                catchToken + ") ]\n";
        }
        out += "  call void @zeus_exception_capture(ptr " + pad.valueSlot +
            ", ptr @__zeus_make_runtime_error, ptr @__zeus_make_type_error) [ \"funclet\"(token " + catchToken + ") ]\n";
        out += "  call void @zeus_try_exit() [ \"funclet\"(token " + catchToken + ") ]\n";
        out += "  catchret from " + catchToken + " to label " + pad.resumeLabel + "\n";
        out += pad.resumeLabel.substr(1) + ":\n";
        out += "  br label " + detail::Emitter::labelFor(pad.catchChunk, pad.catchPc) + "\n";
    }

    out += "}\n";
    out += e.emitEntryGlobalCleanupHelpers();

    // On-demand emit, final sweep over the exact text being linked: drop
    // personalities no unwinding consults, and drop "collect if needed"
    // polls from functions that provably allocate nothing. Both are pure
    // text transforms with no semantic content, so they cannot change what
    // the program computes — only which runtime pieces the linker must keep.
    detail::stripRedundantPersonalities(out);
    detail::stripUnneededSafepoints(out);

    if (!detail::deduplicateFunctionDeclarations(out, res.error)) {
        res.ok = false;
        return res;
    }

    const bool isMCUMode = ZeusMCURuntime::shouldEnable(e.mcuTarget, e.nativeExportSymbols);
    if (isMCUMode) {
        ZeusMCURuntime::stripExceptionHandling(out);
        ZeusMCURuntime::stripDefinedDeclarations(out);

        // 扫描 IR 模块中实际引用的最大全局槽索引
        size_t maxGlobalSlot = 0;
        size_t scanPos = 0;
        while (scanPos < out.size()) {
            const size_t storePos = out.find("@zeus_global_store_i64(i32 ", scanPos);
            const size_t loadPos = out.find("@zeus_global_load_i64(i32 ", scanPos);
            if (storePos == std::string::npos && loadPos == std::string::npos) break;

            size_t matchPos = 0;
            size_t numStart = 0;
            if (storePos != std::string::npos && (loadPos == std::string::npos || storePos < loadPos)) {
                matchPos = storePos;
                numStart = storePos + 27; // length of "@zeus_global_store_i64(i32 "
            } else {
                matchPos = loadPos;
                numStart = loadPos + 26; // length of "@zeus_global_load_i64(i32 "
            }

            size_t numEnd = numStart;
            while (numEnd < out.size() && (out[numEnd] >= '0' && out[numEnd] <= '9')) {
                ++numEnd;
            }
            if (numEnd > numStart) {
                try {
                    const size_t slot = std::stoull(out.substr(numStart, numEnd - numStart));
                    if (slot > maxGlobalSlot) maxGlobalSlot = slot;
                } catch (...) {}
            }
            scanPos = (numEnd > matchPos) ? numEnd : matchPos + 1;
        }

        // 槽位总数至少为 maxGlobalSlot + 1
        const size_t neededSlots = maxGlobalSlot + 1;
        out += ZeusMCURuntime::emitRuntime(e.nativeExportSymbols, true, neededSlots);
    }

    res.ok = true;
    res.text = out;
    res.warnings = e.result.warnings;
    return res;
}

