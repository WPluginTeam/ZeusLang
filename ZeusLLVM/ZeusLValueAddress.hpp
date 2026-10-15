// Included inside Emitter. Address values identify complete variable slots.
    template <typename EmitLine, typename FreshTemporary>
    bool emitLValueFetch(const NativeValue& address, const std::string& targetType,
                        NativeValue& result, EmitLine&& emit,
                        FreshTemporary&& fresh, std::string& error) {
        if (address.functionRef || address.dynamic || address.type != "i64") {
            error = "@faddr requires an int address";
            return false;
        }
        const std::string pointer = fresh();
        const std::string loaded = fresh();
        emit(pointer + " = inttoptr i64 " + address.value + " to ptr");
        emit(loaded + " = alloca %ZeusValue, align 8");
        emit("call void @zeus_value_init_nil(ptr " + loaded + ")");
        emit("call void @zeus_lvalue_load(ptr " + loaded + ", ptr " + pointer +
             ", ptr " + poolStr(targetType) + ")");
        result = NativeValue{loaded, "ptr", false, true, targetType, true};
        return true;
    }

    template <typename EmitLine, typename FreshTemporary, typename BoxDynamicValue>
    bool emitLValueStore(const NativeValue& address, const std::string& targetType,
                        const NativeValue& value, EmitLine&& emit,
                        FreshTemporary&& fresh, BoxDynamicValue&& box,
                        std::string& error) {
        if (address.functionRef || address.dynamic || address.type != "i64") {
            error = "@faddr assignment requires an int address";
            return false;
        }
        std::string boxed;
        bool ownsBoxed = false;
        if (!box(value, boxed, ownsBoxed, error)) return false;
        const std::string pointer = fresh();
        emit(pointer + " = inttoptr i64 " + address.value + " to ptr");
        emit("call void @zeus_lvalue_store(ptr " + pointer + ", ptr " +
             poolStr(targetType) + ", ptr " + boxed + ")");
        if (ownsBoxed) emit("call void @zeus_value_release(ptr " + boxed + ")");
        if (value.ownsClass && isManagedClassValue(value)) {
            emit("call void @zeus_class_ref_release(ptr " + value.value + ")");
        }
        return true;
    }
