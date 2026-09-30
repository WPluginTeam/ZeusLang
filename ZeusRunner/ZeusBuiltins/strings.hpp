// ZeusBuiltins/strings.hpp - 字符串操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"
#include "array.hpp"

#include <cstdint>
#include <limits>

namespace ZeusBuiltins {

    // ===== String 操作函数 =====

    // __str_get(string, index): 获取指定索引处的字符
    inline ZeusType::UniversalVariant builtin_str_get(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__str_get expects 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_get: first argument must be string");

        int index = 0;
        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[1])) {
            index = ZeusVMUtils::bigIntegerToInt(*ip);
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                index = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
            }
        }

        if (index < 0) {
            index = static_cast<int>(str->length()) + index;
        }

        if (index < 0 || index >= static_cast<int>(str->length())) {
            throw std::runtime_error("__str_get: index out of range");
        }

        std::string result(1, (*str)[index]);
        return ZeusType::UniversalVariant(result);
    }

    // __str_slice(string, start, end): 字符串切片 [start, end)
    inline ZeusType::UniversalVariant builtin_str_slice(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__str_slice expects 3 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_slice: first argument must be string");

        int start = 0;
        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[1])) {
            start = ZeusVMUtils::bigIntegerToInt(*ip);
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                start = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
            }
        }

        int end = static_cast<int>(str->length());
        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[2])) {
            end = ZeusVMUtils::bigIntegerToInt(*ip);
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[2])) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                end = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
            }
        }

        int len = static_cast<int>(str->length());
        if (start < 0) start = len + start;
        if (end < 0) end = len + end;

        if (start < 0) start = 0;
        if (end > len) end = len;
        if (start > end) start = end;

        std::string result = str->substr(start, end - start);
        return ZeusType::UniversalVariant(result);
    }

    // __str_index(string, substring): 查找子字符串首次出现位置
    inline ZeusType::UniversalVariant builtin_str_index(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) throw std::runtime_error("__str_index expects at least 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_index: first argument must be string");

        const std::string* substr = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            substr = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                substr = &std::get<std::string>(vp->data);
            }
        }
        if (!substr) throw std::runtime_error("__str_index: second argument must be string");

        int start = 0;
        if (args.size() > 2) {
            if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[2])) {
                start = ZeusVMUtils::bigIntegerToInt(*ip);
            } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[2])) {
                if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                    start = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
                }
            }
        }

        if (start < 0) start = 0;
        size_t pos = str->find(*substr, start);
        
        if (pos == std::string::npos) {
            return ZeusType::UniversalVariant(ZeusType::BigInteger(-1));
        }
        return ZeusType::UniversalVariant(ZeusType::BigInteger(static_cast<int>(pos)));
    }

    // __str_rindex(string, substring): 查找子字符串最后出现位置
    inline ZeusType::UniversalVariant builtin_str_rindex(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) throw std::runtime_error("__str_rindex expects at least 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_rindex: first argument must be string");

        const std::string* substr = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            substr = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                substr = &std::get<std::string>(vp->data);
            }
        }
        if (!substr) throw std::runtime_error("__str_rindex: second argument must be string");

        size_t pos = str->rfind(*substr);
        
        if (pos == std::string::npos) {
            return ZeusType::UniversalVariant(ZeusType::BigInteger(-1));
        }
        return ZeusType::UniversalVariant(ZeusType::BigInteger(static_cast<int>(pos)));
    }

    // __str_replace(string, old, new): 替换所有子字符串
    inline ZeusType::UniversalVariant builtin_str_replace(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__str_replace expects 3 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_replace: first argument must be string");

        const std::string* old_str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            old_str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                old_str = &std::get<std::string>(vp->data);
            }
        }
        if (!old_str) throw std::runtime_error("__str_replace: second argument must be string");

        const std::string* new_str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[2])) {
            new_str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[2])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                new_str = &std::get<std::string>(vp->data);
            }
        }
        if (!new_str) throw std::runtime_error("__str_replace: third argument must be string");

        if (old_str->empty()) {
            throw std::runtime_error("__str_replace: old substring cannot be empty");
        }

        std::string result = *str;
        size_t pos = 0;
        while ((pos = result.find(*old_str, pos)) != std::string::npos) {
            result.replace(pos, old_str->length(), *new_str);
            pos += new_str->length();
        }

        return ZeusType::UniversalVariant(result);
    }

    inline ZeusType::UniversalVariant builtin_str_length(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__str_length expects 1 argument");

        const std::string* str = nullptr;
        if (const std::string* value = std::get_if<std::string>(&args[0])) {
            str = value;
        } else if (const LexerType::Value* value = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(value->data)) {
                str = &std::get<std::string>(value->data);
            }
        }
        if (!str) throw std::runtime_error("__str_length: argument must be string");
        if (str->size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("__str_length: string is too large");
        }
        return ZeusType::UniversalVariant(
            ZeusType::BigInteger(static_cast<int64_t>(str->size())));
    }

    // __str_split(string, separator): 分割字符串，返回数组
    inline ZeusType::UniversalVariant builtin_str_split(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) throw std::runtime_error("__str_split expects at least 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_split: first argument must be string");

        const std::string* sep = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            sep = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                sep = &std::get<std::string>(vp->data);
            }
        }
        if (!sep) throw std::runtime_error("__str_split: second argument must be string");

        ZeusType::Array result;
        result.impl = std::make_shared<ZeusType::ArrayData>();

        if (sep->empty()) {
            for (char c : *str) {
                std::string part(1, c);
                result.impl->data.push_back(ZeusType::UniversalVariant(part));
            }
        } else {
            size_t start = 0;
            size_t end = str->find(*sep);
            
            while (end != std::string::npos) {
                std::string part = str->substr(start, end - start);
                result.impl->data.push_back(ZeusType::UniversalVariant(part));
                start = end + sep->length();
                end = str->find(*sep, start);
            }
            std::string part = str->substr(start);
            result.impl->data.push_back(ZeusType::UniversalVariant(part));
        }

        return ZeusType::UniversalVariant(ZeusType::Variant(result));
    }

    // __str_startswith(string, prefix): 检查字符串是否以prefix开头
    inline ZeusType::UniversalVariant builtin_str_startswith(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__str_startswith expects 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_startswith: first argument must be string");

        const std::string* prefix = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            prefix = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                prefix = &std::get<std::string>(vp->data);
            }
        }
        if (!prefix) throw std::runtime_error("__str_startswith: second argument must be string");

        bool result = str->length() >= prefix->length() && 
                      str->compare(0, prefix->length(), *prefix) == 0;
        return ZeusType::UniversalVariant(result);
    }

    // __str_endswith(string, suffix): 检查字符串是否以suffix结尾
    inline ZeusType::UniversalVariant builtin_str_endswith(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__str_endswith expects 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_endswith: first argument must be string");

        const std::string* suffix = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            suffix = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                suffix = &std::get<std::string>(vp->data);
            }
        }
        if (!suffix) throw std::runtime_error("__str_endswith: second argument must be string");

        bool result = str->length() >= suffix->length() && 
                      str->compare(str->length() - suffix->length(), suffix->length(), *suffix) == 0;
        return ZeusType::UniversalVariant(result);
    }

    // __str_lower(string): 转换为小写
    inline ZeusType::UniversalVariant builtin_str_lower(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__str_lower expects 1 argument");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_lower: argument must be string");

        std::string result = *str;
        for (char& c : result) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        return ZeusType::UniversalVariant(result);
    }

    // __str_upper(string): 转换为大写
    inline ZeusType::UniversalVariant builtin_str_upper(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__str_upper expects 1 argument");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_upper: argument must be string");

        std::string result = *str;
        for (char& c : result) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        return ZeusType::UniversalVariant(result);
    }

    // __str_count(string, substring): 计算子字符串出现次数
    inline ZeusType::UniversalVariant builtin_str_count(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__str_count expects 2 arguments");

        const std::string* str = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[0])) {
            str = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[0])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                str = &std::get<std::string>(vp->data);
            }
        }
        if (!str) throw std::runtime_error("__str_count: first argument must be string");

        const std::string* substr = nullptr;
        if (const std::string* sp = std::get_if<std::string>(&args[1])) {
            substr = sp;
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<std::string>(vp->data)) {
                substr = &std::get<std::string>(vp->data);
            }
        }
        if (!substr) throw std::runtime_error("__str_count: second argument must be string");

        if (substr->empty()) {
            return ZeusType::UniversalVariant(ZeusType::BigInteger(0));
        }

        int count = 0;
        size_t pos = 0;
        while ((pos = str->find(*substr, pos)) != std::string::npos) {
            count++;
            pos += substr->length();
        }

        return ZeusType::UniversalVariant(ZeusType::BigInteger(count));
    }

    // ===== 注册函数 =====
    inline void registerStringBuiltins() {
        registerBuiltin("__str_length", builtin_str_length, 1, 1);
        registerBuiltin("__str_get", builtin_str_get, 2, 2);
        registerBuiltin("__str_slice", builtin_str_slice, 3, 3);
        registerBuiltin("__str_index", builtin_str_index, 2, 3);
        registerBuiltin("__str_rindex", builtin_str_rindex, 2, 2);
        registerBuiltin("__str_replace", builtin_str_replace, 3, 3);
        registerBuiltin("__str_split", builtin_str_split, 2, 2);
        registerBuiltin("__str_startswith", builtin_str_startswith, 2, 2);
        registerBuiltin("__str_endswith", builtin_str_endswith, 2, 2);
        registerBuiltin("__str_lower", builtin_str_lower, 1, 1);
        registerBuiltin("__str_upper", builtin_str_upper, 1, 1);
        registerBuiltin("__str_count", builtin_str_count, 2, 2);
    }

}  // namespace ZeusBuiltins
