// Included inside Emitter: class copying shares the emitter's layout and ABI helpers.
    std::string classCopyCallback(const std::string& typeName) const {
        return llvmFunctionName("__class_copy_" + typeName);
    }

    std::string classCopyDefaultCallback(const std::string& typeName) const {
        return llvmFunctionName("__class_copy_default_" + typeName);
    }

    template <typename EmitLine, typename FreshTemporary>
    void emitClassAssignmentCopy(const IRType::IRInstr& ins, NativeValue& value,
                                 EmitLine&& emit, FreshTemporary&& fresh) {
        if (!ins.metadata.classAssignmentCopy || value.functionRef) return;
        if (!value.dynamic && !isManagedClassValue(value)) return;
        const std::string skipHook = ins.metadata.classCopySkipHook ? "1" : "0";
        const std::string copied = fresh();
        if (value.dynamic) {
            emit(copied + " = alloca %ZeusValue, align 8");
            emit("call void @zeus_value_copy_class_owned(ptr " + copied +
                 ", ptr " + value.value + ", i32 " + skipHook + ")");
            if (value.ownsDynamic) {
                emit("call void @zeus_value_release(ptr " + value.value + ")");
            }
            value.value = copied;
            value.ownsDynamic = true;
        } else {
            emit(copied + " = call ptr @zeus_class_copy_owned(ptr " + value.value +
                 ", i32 " + skipHook + ")");
            if (value.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
            }
            value.value = copied;
            value.ownsClass = true;
        }
    }

    std::string emitClassCopyHelpers() {
        std::string generated;
        for (const auto& entry : classLayouts) {
            const std::string& className = entry.first;
            const ClassLayout& layout = entry.second;
            const std::string typeName = poolStr(className);
            const std::string structType = llvmStructType(className);
            const FunctionSignature* hook = nullptr;
            for (const auto& function : functionSignatures) {
                if (function.second.ownerType == className &&
                    function.second.operatorSymbol == "@=" &&
                    function.second.parameterTypes.size() == 1 &&
                    !function.second.isGenericTemplate) {
                    hook = &function.second;
                    break;
                }
            }
            generated += "define internal ptr " + classCopyCallback(className) +
                "(ptr %source, ptr %context) personality ptr @__CxxFrameHandler3 {\nentry:\n";
            generated += "  call void @zeus_class_ref_require_live(ptr %source)\n";
            if (hook != nullptr) {
                generated += "  %known = call ptr @zeus_class_copy_find(ptr %context, ptr %source)\n";
                generated += "  %found = icmp ne ptr %known, null\n";
                generated += "  br i1 %found, label %existing, label %hook\nexisting:\n";
                generated += "  call void @zeus_class_ref_retain(ptr %known)\n  ret ptr %known\nhook:\n";
                generated += "  %result = call ptr " + llvmCallTarget(hook->name) +
                    "(ptr %source)\n";
                generated += "  call void @zeus_class_copy_remember(ptr %context, ptr %source, ptr %result)\n";
                generated += "  ret ptr %result\n}\n\n";
            } else {
                generated += "  %result = call ptr " + classCopyDefaultCallback(className) +
                    "(ptr %source, ptr %context)\n  ret ptr %result\n}\n\n";
            }
            generated += "define internal ptr " + classCopyDefaultCallback(className) +
                "(ptr %source, ptr %context) personality ptr @__CxxFrameHandler3 {\nentry:\n";
            generated += "  call void @zeus_class_ref_require_live(ptr %source)\n";
            generated += "  %known = call ptr @zeus_class_copy_find(ptr %context, ptr %source)\n";
            generated += "  %found = icmp ne ptr %known, null\n";
            generated += "  br i1 %found, label %existing, label %allocate\nexisting:\n";
            generated += "  call void @zeus_class_ref_retain(ptr %known)\n  ret ptr %known\nallocate:\n";
            const std::string size = layout.fieldNames.empty() ? "1" : llvmTypeStorageSize(structType);
            generated += "  %object = call ptr @malloc(i64 " + size + ")\n";
            generated += "  %failed = icmp eq ptr %object, null\n";
            generated += "  br i1 %failed, label %out.of.memory, label %copy\nout.of.memory:\n";
            generated += "  call void @llvm.trap()\n  unreachable\ncopy:\n";
            generated += "  call void @llvm.memset.p0.i64(ptr %object, i8 0, i64 " + size + ", i1 false)\n";
            generated += "  call void @zeus_class_ref_register(ptr %object, ptr " +
                classLifetimeFinalizer(className) + ", ptr " + typeName + ")\n";
            generated += "  call void @zeus_class_copy_remember_allocated(ptr %context, ptr %source, ptr %object)\n";
            for (size_t i = 0; i < layout.fieldTypes.size(); ++i) {
                const std::string suffix = std::to_string(i);
                const std::string& fieldType = layout.fieldTypes[i];
                const std::string abiType = llvmFieldType(fieldType);
                generated += "  %source." + suffix + " = getelementptr inbounds " + structType +
                    ", ptr %source, i32 0, i32 " + suffix + "\n";
                generated += "  %target." + suffix + " = getelementptr inbounds " + structType +
                    ", ptr %object, i32 0, i32 " + suffix + "\n";
                if (usesZeusValueABI(fieldType)) {
                    generated += "  call void @zeus_value_copy_class_graph_internal(ptr %target." + suffix +
                        ", ptr %source." + suffix + ", ptr %context)\n";
                } else {
                    generated += "  %field." + suffix + " = load " + abiType + ", ptr %source." + suffix + "\n";
                    const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldType);
                    if (fieldABI != nullptr &&
                        fieldABI->valueABI == ZeusBuiltins::NativeValueABI::NativeBytes) {
                        generated += "  %bytes.copy." + suffix + " = call ptr @zeus_bytes_clone(ptr %field." +
                            suffix + ")\n";
                        generated += "  store ptr %bytes.copy." + suffix + ", ptr %target." +
                            suffix + "\n";
                    } else if (isClassType(fieldType)) {
                        generated += "  %copy." + suffix + " = call ptr @zeus_class_copy_object(ptr %field." +
                            suffix + ", ptr %context)\n";
                        generated += "  store ptr %copy." + suffix + ", ptr %target." + suffix + "\n";
                        generated += "  call void @zeus_class_ref_retain_edge(ptr %copy." + suffix + ")\n";
                        generated += "  call void @zeus_class_ref_release(ptr %copy." + suffix + ")\n";
                    } else {
                        generated += "  store " + abiType + " %field." + suffix + ", ptr %target." + suffix + "\n";
                    }
                }
            }
            generated += "  call void @zeus_value_dynamic_field_copy_state(ptr %object, ptr %source, ptr %context)\n";
            generated += "  call void @zeus_class_gc_register(ptr %object, ptr " + typeName + ")\n";
            generated += "  ret ptr %object\n}\n\n";
        }
        return generated;
    }
