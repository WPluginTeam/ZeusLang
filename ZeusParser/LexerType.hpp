
#ifndef LexerType_hpp
#define LexerType_hpp

#include <string>
#include <vector>
#include <variant>
#include <sstream>
#include "../ZeusRuntimeLib/ZeusType.hpp"

namespace LexerType {

    enum class TokenType {
        KEYWORD,  // int, string, bool, if, else, ...
        VALUE,    // 42, 3.14, "hello"
        OPERATOR, // +, -, *, /, =, ==, ...

        UNDEF
    };

    class TokenClass {
        public:
            TokenType type;
            int line = 1;  // Source line number

            TokenType getType() const { return TokenType::UNDEF; }
            
            virtual std::string toString() const { return "UNDEF"; }
    };


    class Keyword : public TokenClass {
        public:
            std::string name;

            Keyword(std::string name) : name(name) {}

            TokenType getType() const { 
                return TokenType::KEYWORD;
            }
            
            std::string toString() const override {
                return name;
            }
    };

    // 新增 Nil 语义：在词法阶段仍作为 Keyword("nil") 产生；运行期需要一个 Null/Nil 值表示。

    class Value : public TokenClass {
        public:
            std::variant<ZeusType::BigInteger, ZeusType::BigDecimal, std::string, bool> data;

            Value(int data) :                         data(ZeusType::BigInteger(data)) {}
            Value(int64_t data) :                     data(ZeusType::BigInteger(data)) {}
            Value(const ZeusType::BigInteger& data) : data(data) {}
            Value(double data) :                      data(ZeusType::BigDecimal(data)) {}
            Value(const ZeusType::BigDecimal& data) : data(data) {}
            Value(std::string data) :                 data(data) {}
            Value(bool data) :                        data(data) {}

            TokenType getType() const {
                return TokenType::VALUE;
            }

            std::string getStringType() const {
                if (std::holds_alternative<ZeusType::BigInteger>(data))
                    return "int";

                else if (std::holds_alternative<ZeusType::BigDecimal>(data)) 
                    return "float";

                else if (std::holds_alternative<std::string>(data)) 
                    return "string";

                else if (std::holds_alternative<bool>(data))
                    return "bool";

                else
                    return "unknown type";
            }
            
            std::string toString() const override {
                return std::visit([](auto&& arg) -> std::string {
                    
                    using T = std::decay_t<decltype(arg)>;

                    if constexpr (std::is_same_v<T, ZeusType::BigInteger>) 
                        return arg.toString();

                    else if constexpr (std::is_same_v<T, ZeusType::BigDecimal>) 
                        return arg.toString();
                        
                    else if constexpr (std::is_same_v<T, std::string>) 
                        return "\"" + arg + "\"";

                    else if constexpr (std::is_same_v<T, bool>) 
                        return arg ? "true" : "false";
                    
                }, data);
            }

            Value operator+(const Value& v1) const {
                // P0：直接判 variant 分支，避免 getStringType() 的 std::string 分配
                const bool lInt = std::holds_alternative<ZeusType::BigInteger>(this->data);
                const bool rInt = std::holds_alternative<ZeusType::BigInteger>(v1.data);
                const bool lFlt = std::holds_alternative<ZeusType::BigDecimal>(this->data);
                const bool rFlt = std::holds_alternative<ZeusType::BigDecimal>(v1.data);
                const bool lStr = std::holds_alternative<std::string>(this->data);
                const bool rStr = std::holds_alternative<std::string>(v1.data);

                if (lInt && rInt) {
                    return Value(std::get<ZeusType::BigInteger>(this->data) + std::get<ZeusType::BigInteger>(v1.data));
                } else if (lFlt && rFlt) {
                    return Value(std::get<ZeusType::BigDecimal>(this->data) + std::get<ZeusType::BigDecimal>(v1.data));
                } else if (lStr && rStr) {
                    return Value(std::get<std::string>(this->data) + std::get<std::string>(v1.data));
                } else if (lInt && rFlt) {
                    ZeusType::BigDecimal lhs(std::get<ZeusType::BigInteger>(this->data));
                    return Value(lhs + std::get<ZeusType::BigDecimal>(v1.data));
                } else if (lFlt && rInt) {
                    ZeusType::BigDecimal rhs(std::get<ZeusType::BigInteger>(v1.data));
                    return Value(std::get<ZeusType::BigDecimal>(this->data) + rhs);
                } else {
                    throw std::runtime_error("Unsupported operand types for +");
                }
            }

            Value operator-(const Value& v1) const {
                const bool lInt = std::holds_alternative<ZeusType::BigInteger>(this->data);
                const bool rInt = std::holds_alternative<ZeusType::BigInteger>(v1.data);
                const bool lFlt = std::holds_alternative<ZeusType::BigDecimal>(this->data);
                const bool rFlt = std::holds_alternative<ZeusType::BigDecimal>(v1.data);

                if (lInt && rInt) {
                    return Value(std::get<ZeusType::BigInteger>(this->data) - std::get<ZeusType::BigInteger>(v1.data));
                } else if (lFlt && rFlt) {
                    return Value(std::get<ZeusType::BigDecimal>(this->data) - std::get<ZeusType::BigDecimal>(v1.data));
                } else if (lInt && rFlt) {
                    ZeusType::BigDecimal lhs(std::get<ZeusType::BigInteger>(this->data));
                    return Value(lhs - std::get<ZeusType::BigDecimal>(v1.data));
                } else if (lFlt && rInt) {
                    ZeusType::BigDecimal rhs(std::get<ZeusType::BigInteger>(v1.data));
                    return Value(std::get<ZeusType::BigDecimal>(this->data) - rhs);
                } else {
                    throw std::runtime_error("Unsupported operand types for -");
                }
            }

            Value operator*(const Value& v1) const {
                const bool lInt = std::holds_alternative<ZeusType::BigInteger>(this->data);
                const bool rInt = std::holds_alternative<ZeusType::BigInteger>(v1.data);
                const bool lFlt = std::holds_alternative<ZeusType::BigDecimal>(this->data);
                const bool rFlt = std::holds_alternative<ZeusType::BigDecimal>(v1.data);

                if (lInt && rInt) {
                    return Value(std::get<ZeusType::BigInteger>(this->data) * std::get<ZeusType::BigInteger>(v1.data));
                } else if (lFlt && rFlt) {
                    return Value(std::get<ZeusType::BigDecimal>(this->data) * std::get<ZeusType::BigDecimal>(v1.data));
                } else if (lInt && rFlt) {
                    ZeusType::BigDecimal lhs(std::get<ZeusType::BigInteger>(this->data));
                    return Value(lhs * std::get<ZeusType::BigDecimal>(v1.data));
                } else if (lFlt && rInt) {
                    ZeusType::BigDecimal rhs(std::get<ZeusType::BigInteger>(v1.data));
                    return Value(std::get<ZeusType::BigDecimal>(this->data) * rhs);
                } else {
                    throw std::runtime_error("Unsupported operand types for *");
                }
            }

            Value operator/(const Value& v1) const {
                const bool lInt = std::holds_alternative<ZeusType::BigInteger>(this->data);
                const bool rInt = std::holds_alternative<ZeusType::BigInteger>(v1.data);
                const bool lFlt = std::holds_alternative<ZeusType::BigDecimal>(this->data);
                const bool rFlt = std::holds_alternative<ZeusType::BigDecimal>(v1.data);

                if (lInt && rInt) {
                    const auto& divisor = std::get<ZeusType::BigInteger>(v1.data);
                    if (divisor == ZeusType::BigInteger(0)) throw std::runtime_error("Division by zero");
                    return Value(std::get<ZeusType::BigInteger>(this->data) / divisor);
                } else if (lFlt && rFlt) {
                    const auto& divisor = std::get<ZeusType::BigDecimal>(v1.data);
                    if (divisor == ZeusType::BigDecimal(0)) throw std::runtime_error("Division by zero");
                    return Value(std::get<ZeusType::BigDecimal>(this->data).divide(divisor, 16));
                } else if (lInt && rFlt) {
                    const auto& divisor = std::get<ZeusType::BigDecimal>(v1.data);
                    if (divisor == ZeusType::BigDecimal(0)) throw std::runtime_error("Division by zero");
                    ZeusType::BigDecimal lhs(std::get<ZeusType::BigInteger>(this->data));
                    return Value(lhs.divide(divisor, 16));
                } else if (lFlt && rInt) {
                    const auto& bigInt = std::get<ZeusType::BigInteger>(v1.data);
                    if (bigInt == ZeusType::BigInteger(0)) throw std::runtime_error("Division by zero");
                    ZeusType::BigDecimal rhs(bigInt);
                    return Value(std::get<ZeusType::BigDecimal>(this->data).divide(rhs, 16));
                } else {
                    throw std::runtime_error("Unsupported operand types for /");
                }
            }
    };

    class Operator : public TokenClass {
        public:
            std::string symbol;

            Operator(std::string symbol) : symbol(symbol) {}
            Operator(char symbol) :        symbol(1, symbol) {}

            TokenType getType() const {
                return TokenType::OPERATOR;
            }
            
            std::string toString() const override {
                return symbol;
            }
    };

    // BytesLiteral: 字节串字面量 b"..."
    class BytesLiteral : public TokenClass {
        public:
            std::vector<uint8_t> data;

            BytesLiteral(const std::vector<uint8_t>& data) : data(data) {}

            TokenType getType() const {
                return TokenType::VALUE;
            }
            
            std::string toString() const override {
                std::string result = "b\"";
                for (uint8_t byte : data) {
                    if (byte >= 32 && byte < 127 && byte != '\\' && byte != '"') {
                        result += static_cast<char>(byte);
                    } else {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\x%02x", byte);
                        result += buf;
                    }
                }
                result += "\"";
                return result;
            }
    };

    using Token = std::variant<LexerType::Keyword,
                               LexerType::Value,
                               LexerType::Operator,
                               LexerType::BytesLiteral
                               >;
}

#endif
