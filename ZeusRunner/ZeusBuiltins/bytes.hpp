// ZeusBuiltins/bytes.hpp - 字节数组操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"
#include "array.hpp"  // for getArrayPtr

namespace ZeusBuiltins {

    // ===== Bytes 操作函数 =====
    
    // bytes(arg): 创建字节数组
    // - bytes(): 创建空字节数组
    // - bytes(size): 创建指定大小的零填充字节数组
    // - bytes(list): 从整数列表创建字节数组
    // - bytes(str): 从字符串创建字节数组(UTF-8编码)
    inline ZeusType::UniversalVariant builtin_bytes_func(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            ZeusType::Bytes bytes;
            return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
        }

        const auto& arg = args[0];

        // 从整数创建指定大小的字节数组
        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&arg)) {
            int size = ZeusVMUtils::bigIntegerToInt(*ip);
            if (size < 0) throw std::runtime_error("bytes size must be non-negative");
            
            ZeusType::Bytes bytes;
            bytes.data.resize(size, 0);
            return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
        }

        if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&arg)) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                int size = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
                if (size < 0) throw std::runtime_error("bytes size must be non-negative");
                
                ZeusType::Bytes bytes;
                bytes.data.resize(size, 0);
                return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
            }
        }

        // 从字符串创建字节数组
        if (const std::string* sp = std::get_if<std::string>(&arg)) {
            ZeusType::Bytes bytes;
            for (char c : *sp) {
                bytes.data.push_back(static_cast<uint8_t>(c));
            }
            return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
        }

        if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&arg)) {
            if (std::holds_alternative<std::string>(vp->data)) {
                const std::string& str = std::get<std::string>(vp->data);
                ZeusType::Bytes bytes;
                for (char c : str) {
                    bytes.data.push_back(static_cast<uint8_t>(c));
                }
                return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
            }
        }

        // 从数组创建字节数组 (从Variant中的Array)
        if (const ZeusType::Array* ap = getArrayPtr(arg)) {
            if (!ap->impl) throw std::runtime_error("bytes from array: invalid array");
            
            ZeusType::Bytes bytes;
            for (const auto& item : ap->impl->data) {
                int value = 0;
                if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&item)) {
                    value = ZeusVMUtils::bigIntegerToInt(*ip);
                } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&item)) {
                    if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                        value = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
                    }
                }
                if (value < 0 || value > 255) throw std::runtime_error("byte value must be 0-255");
                bytes.data.push_back(static_cast<uint8_t>(value));
            }
            return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
        }

        // 从数组对象创建字节数组 (通过ObjectHandle - Zeus Array类)
        if (const ZeusType::ObjectHandle* ohp = std::get_if<ZeusType::ObjectHandle>(&arg)) {
            if (g_vmObjects && ohp->id >= 0 && ohp->id < (int)g_vmObjects->size()) {
                const auto& obj = (*g_vmObjects)[ohp->id];
                
                // 检查是否有data属性(Array类的内部数组)
                auto it = obj.values.find("data");
                if (it != obj.values.end()) {
                    const auto& dataVar = it->second;
                    
                    // data是一个原生dict，使用ObjectHandle访问
                    if (const ZeusType::ObjectHandle* dataOhp = std::get_if<ZeusType::ObjectHandle>(&dataVar)) {
                        if (dataOhp->id >= 0 && dataOhp->id < (int)g_vmObjects->size()) {
                            const auto& dataObj = (*g_vmObjects)[dataOhp->id];
                            
                            ZeusType::Bytes bytes;
                            // 遍历所有属性，按数字索引顺序读取
                            for (int i = 0; ; ++i) {
                                auto itemIt = dataObj.values.find(std::to_string(i));
                                if (itemIt == dataObj.values.end()) break;
                                
                                const auto& itemVar = itemIt->second;
                                int value = 0;
                                
                                if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&itemVar)) {
                                    value = ZeusVMUtils::bigIntegerToInt(*ip);
                                } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&itemVar)) {
                                    if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                                        value = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
                                    }
                                }
                                
                                if (value < 0 || value > 255) throw std::runtime_error("byte value must be 0-255");
                                bytes.data.push_back(static_cast<uint8_t>(value));
                            }
                            return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
                        }
                    }
                }
            }
        }

        throw std::runtime_error("bytes() expects int, string, or array");
    }

    // __bytes_get(bytes, index): 获取字节数组指定位置的字节值
    inline ZeusType::UniversalVariant builtin_bytes_get(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__bytes_get expects 2 arguments");

        const auto& bytesArg = args[0];
        const ZeusType::Bytes* bytes = nullptr;

        // 尝试直接获取Bytes
        if (const ZeusType::Bytes* bp = std::get_if<ZeusType::Bytes>(&bytesArg)) {
            bytes = bp;
        } else if (const ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&bytesArg)) {
            bytes = std::get_if<ZeusType::Bytes>(vp);
        }

        if (!bytes) throw std::runtime_error("__bytes_get: first argument must be bytes");

        int index = 0;
        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[1])) {
            index = ZeusVMUtils::bigIntegerToInt(*ip);
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                index = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
            }
        }

        if (index < 0 || index >= (int)bytes->data.size()) {
            throw std::runtime_error("__bytes_get: index out of range");
        }

        return ZeusType::UniversalVariant(ZeusType::BigInteger(bytes->data[index]));
    }

    // __bytes_set(bytes, index, value): 设置字节数组指定位置的字节值
    inline ZeusType::UniversalVariant builtin_bytes_set(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__bytes_set expects 3 arguments");

        // 注意：由于Bytes在Variant中，无法直接修改，需要返回新的bytes对象
        throw std::runtime_error("__bytes_set: bytes is immutable, use bytearray instead");
    }

    // __bytes_len(bytes): 获取字节数组长度
    inline ZeusType::UniversalVariant builtin_bytes_len(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__bytes_len expects 1 argument");

        const auto& bytesArg = args[0];
        const ZeusType::Bytes* bytes = nullptr;

        if (const ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&bytesArg)) {
            bytes = std::get_if<ZeusType::Bytes>(vp);
        }

        if (!bytes) throw std::runtime_error("__bytes_len: argument must be bytes");

        return ZeusType::UniversalVariant(ZeusType::BigInteger((int)bytes->data.size()));
    }

    // __bytes_slice(bytes, start, end): 字节数组切片
    inline ZeusType::UniversalVariant builtin_bytes_slice(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__bytes_slice expects 3 arguments");

        const auto& bytesArg = args[0];
        const ZeusType::Bytes* bytes = nullptr;

        if (const ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&bytesArg)) {
            bytes = std::get_if<ZeusType::Bytes>(vp);
        }

        if (!bytes) throw std::runtime_error("__bytes_slice: first argument must be bytes");

        int start = 0;
        int end = bytes->data.size();

        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[1])) {
            start = ZeusVMUtils::bigIntegerToInt(*ip);
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[1])) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                start = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
            }
        }

        if (const ZeusType::BigInteger* ip = std::get_if<ZeusType::BigInteger>(&args[2])) {
            end = ZeusVMUtils::bigIntegerToInt(*ip);
        } else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&args[2])) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                end = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(vp->data));
            }
        }

        if (start < 0) start = 0;
        if (end > (int)bytes->data.size()) end = bytes->data.size();
        if (start > end) start = end;

        ZeusType::Bytes result;
        for (int i = start; i < end; ++i) {
            result.data.push_back(bytes->data[i]);
        }

        return ZeusType::UniversalVariant(ZeusType::Variant(result));
    }

    // __bytes_decode(bytes, encoding): 将字节数组解码为字符串
    inline ZeusType::UniversalVariant builtin_bytes_decode(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) throw std::runtime_error("__bytes_decode expects at least 1 argument");

        const auto& bytesArg = args[0];
        const ZeusType::Bytes* bytes = nullptr;

        // 尝试直接获取Bytes
        if (const ZeusType::Bytes* bp = std::get_if<ZeusType::Bytes>(&bytesArg)) {
            bytes = bp;
        } else if (const ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&bytesArg)) {
            bytes = std::get_if<ZeusType::Bytes>(vp);
        }

        if (!bytes) {
            throw std::runtime_error("__bytes_decode: first argument must be bytes");
        }

        std::string result;
        for (uint8_t byte : bytes->data) {
            result += static_cast<char>(byte);
        }

        return ZeusType::UniversalVariant(result);
    }

    // __bytes_to_list(bytes): 将字节数组转换为整数列表
    inline ZeusType::UniversalVariant builtin_bytes_to_list(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__bytes_to_list expects 1 argument");

        const auto& bytesArg = args[0];
        const ZeusType::Bytes* bytes = nullptr;

        // 尝试直接获取Bytes
        if (const ZeusType::Bytes* bp = std::get_if<ZeusType::Bytes>(&bytesArg)) {
            bytes = bp;
        } else if (const ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&bytesArg)) {
            bytes = std::get_if<ZeusType::Bytes>(vp);
        }

        if (!bytes) throw std::runtime_error("__bytes_to_list: argument must be bytes");

        ZeusType::Array result;
        result.impl = std::make_shared<ZeusType::ArrayData>();

        for (uint8_t byte : bytes->data) {
            result.impl->data.push_back(ZeusType::UniversalVariant(ZeusType::BigInteger(byte)));
        }

        return ZeusType::UniversalVariant(ZeusType::Variant(result));
    }

    // 注册字节数组相关内置函数
    inline void registerBytesBuiltins() {
        registerBuiltin("bytes", builtin_bytes_func, 0, 1);
        registerBuiltin("__bytes_create", builtin_bytes_func, 0, 1);  // bytes的别名，兼容旧代码
        registerBuiltin("__bytes_get", builtin_bytes_get, 2, 2);
        registerBuiltin("__bytes_set", builtin_bytes_set, 3, 3);
        registerBuiltin("__bytes_len", builtin_bytes_len, 1, 1);
        registerBuiltin("__bytes_slice", builtin_bytes_slice, 3, 3);
        registerBuiltin("__bytes_decode", builtin_bytes_decode, 1, 2);
        registerBuiltin("__bytes_to_list", builtin_bytes_to_list, 1, 1);
    }

} // namespace ZeusBuiltins
