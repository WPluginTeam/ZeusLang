// Strongly-typed IR instruction representation
#ifndef IRType_hpp
#define IRType_hpp

#include <string>
#include <vector>
#include <variant>
#include <type_traits>

#include "../ZeusParser/TreeType.hpp"
#include "../ZeusParser/TreeParser.hpp"
#include "../ZeusParser/LexerType.hpp"

#include "../ZeusRunner/ZeusVMOperator.hpp"
#include "../ZeusRuntimeLib/ZeusType.hpp"

#include "IRMetadata.hpp"

namespace IRType {

    struct NullLiteral {};

    // IR 参数：既支持标识字符串（变量名、标志），也支持类型化的立即数（沿用 LexerType::Value）
    using IRArgs = std::variant<std::string, LexerType::Value, ZeusType::Bytes, NullLiteral>;

    enum class IRCompilerStatus {
        Running,
        Stopped,
        Error
    };

    struct IRInstr {

        VMOperator op;
        std::vector<IRArgs> args;
        
        int index = -1;
        int line = -1;  // Source code line number
        int sourceFileId = 0;  // Source file ID (0 = main file)
        IRMetadata metadata;

        std::string toString() const {
            std::string result = VMO_toString(op);

            for (const auto& arg : args) {
                result += " ";
                result += std::visit([](auto&& a) -> std::string {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        return a;
                    } else if constexpr (std::is_same_v<T, LexerType::Value>) {
                        return a.toString();
                    } else if constexpr (std::is_same_v<T, ZeusType::Bytes>) {
                        std::string s = "b\"";
                        for (size_t i = 0; i < a.data.size() && i < 16; ++i) {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\x%02x", a.data[i]);
                            s += buf;
                        }
                        if (a.data.size() > 16) s += "...";
                        s += "\"";
                        return s;
                    } else if constexpr (std::is_same_v<T, NullLiteral>) {
                        return "nil";
                    }
                }, arg);
            }
            if (line > 0) {
                result += " [L" + std::to_string(line) + "]";
            }
            return result;
        }
    };

}
#endif
