// Emitter member definitions: inline fast paths for the read only bytes builtins; included inside class Emitter.
//
// zeus_bytes_get and zeus_bytes_len live in the runtime library, so every call
// is opaque to LLVM: it cannot be inlined, hoisted out of a loop, or combined
// with the surrounding bounds checks. A JSON scan reads one byte per call.
//
// The native bytes layout is a [uint64 length] header immediately before the
// data pointer. The fast helpers read that header and the byte inline and fall
// back to the runtime function for everything unusual (null pointer, index out
// of range, oversized length), so the diagnostics are exactly the runtime's.

    static constexpr const char* kBytesGetFastSymbol = "zeus_bytes_get_fast";
    static constexpr const char* kBytesLenFastSymbol = "zeus_bytes_len_fast";

    // The call target to use for a native symbol, or empty when it keeps the
    // runtime function. MCU builds link no runtime library helpers like these.
    std::string fastBytesCallTarget(const std::string& nativeSymbol) const {
        if (isMCUMode()) return std::string();
        if (nativeSymbol == "zeus_bytes_get") {
            return std::string("@") + kBytesGetFastSymbol;
        }
        if (nativeSymbol == "zeus_bytes_len") {
            return std::string("@") + kBytesLenFastSymbol;
        }
        return std::string();
    }

    // Module level definitions. They are internal and always inlined, so an
    // unused one costs nothing.
    static std::string bytesFastPathDefinitions() {
        std::string out;
        out += "define internal i64 @zeus_bytes_get_fast(ptr %data, i64 %index) alwaysinline {\n"
               "entry:\n"
               "  %isnull = icmp eq ptr %data, null\n"
               "  br i1 %isnull, label %slow, label %check\n"
               "check:\n"
               "  %header = getelementptr inbounds i8, ptr %data, i64 -8\n"
               "  %length = load i64, ptr %header, align 1\n"
               "  %inrange = icmp ult i64 %index, %length\n"
               "  br i1 %inrange, label %fast, label %slow\n"
               "fast:\n"
               "  %address = getelementptr inbounds i8, ptr %data, i64 %index\n"
               "  %byte = load i8, ptr %address, align 1\n"
               "  %value = zext i8 %byte to i64\n"
               "  ret i64 %value\n"
               "slow:\n"
               "  %result = call i64 @zeus_bytes_get(ptr %data, i64 %index)\n"
               "  ret i64 %result\n"
               "}\n\n";
        out += "define internal i64 @zeus_bytes_len_fast(ptr %data) alwaysinline {\n"
               "entry:\n"
               "  %isnull = icmp eq ptr %data, null\n"
               "  br i1 %isnull, label %slow, label %check\n"
               "check:\n"
               "  %header = getelementptr inbounds i8, ptr %data, i64 -8\n"
               "  %length = load i64, ptr %header, align 1\n"
               "  %valid = icmp sge i64 %length, 0\n"
               "  br i1 %valid, label %fast, label %slow\n"
               "fast:\n"
               "  ret i64 %length\n"
               "slow:\n"
               "  %result = call i64 @zeus_bytes_len(ptr %data)\n"
               "  ret i64 %result\n"
               "}\n\n";
        return out;
    }
