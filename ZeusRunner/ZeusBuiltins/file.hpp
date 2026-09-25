#pragma once

// ZeusBuiltins File Functions
// 文件操作函数：open, read, write, close, seek 等

#include "core.hpp"
#include <fstream>
#include <cstring>
#include <unordered_map>

namespace ZeusBuiltins {

    // 文件句柄管理
    inline std::unordered_map<int, std::fstream*> g_openFiles;
    inline int g_fileHandleCounter = 0;

    // =====================================================================
    // 文件操作函数实现
    // =====================================================================

    // __file_open(path, mode): 打开文件，返回句柄
    // mode: "r"(读), "w"(写), "a"(追加), "rb"(二进制读), "wb"(二进制写), "ab"(二进制追加)
    inline ZeusType::UniversalVariant builtin_file_open(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_open: requires at least 1 argument (path)");
        }

        std::string path;
        if (std::holds_alternative<std::string>(args[0])) {
            path = std::get<std::string>(args[0]);
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<std::string>(val.data)) {
                path = std::get<std::string>(val.data);
            } else {
                throw std::runtime_error("__file_open: path must be a string");
            }
        } else {
            throw std::runtime_error("__file_open: path must be a string");
        }

        std::string mode = "r";
        if (args.size() >= 2) {
            if (std::holds_alternative<std::string>(args[1])) {
                mode = std::get<std::string>(args[1]);
            } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                const auto& val = std::get<LexerType::Value>(args[1]);
                if (std::holds_alternative<std::string>(val.data)) {
                    mode = std::get<std::string>(val.data);
                } else {
                    throw std::runtime_error("__file_open: mode must be a string");
                }
            } else {
                throw std::runtime_error("__file_open: mode must be a string");
            }
        }

        // 转换 Zeus mode 到 C++ ios mode
        std::ios_base::openmode iomode = std::ios::in;
        if (mode == "w") {
            iomode = std::ios::out;
        } else if (mode == "a") {
            iomode = std::ios::app;
        } else if (mode == "r+") {
            iomode = std::ios::in | std::ios::out;
        } else if (mode == "w+") {
            iomode = std::ios::in | std::ios::out | std::ios::trunc;
        } else if (mode == "rb") {
            iomode = std::ios::in | std::ios::binary;
        } else if (mode == "wb") {
            iomode = std::ios::out | std::ios::binary;
        } else if (mode == "ab") {
            iomode = std::ios::app | std::ios::binary;
        } else if (mode == "r+b" || mode == "rb+") {
            iomode = std::ios::in | std::ios::out | std::ios::binary;
        } else if (mode == "w+b" || mode == "wb+") {
            iomode = std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary;
        }

        auto* file = new std::fstream(path, iomode);
        if (!file->is_open()) {
            delete file;
            throw std::runtime_error("__file_open: failed to open file: " + path);
        }

        int handle = g_fileHandleCounter++;
        g_openFiles[handle] = file;

        return ZeusType::UniversalVariant(ZeusType::BigInteger(handle));
    }

    // __file_read(handle, size): 读取文件内容
    // size < 0 表示读到文件末尾
    inline ZeusType::UniversalVariant builtin_file_read(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_read: requires at least 1 argument (handle)");
        }

        int handle = 0;
        if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
            handle = (int)std::get<ZeusType::BigInteger>(args[0]).toInt64();
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                handle = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
            } else {
                throw std::runtime_error("__file_read: handle must be an integer");
            }
        } else {
            throw std::runtime_error("__file_read: handle must be an integer");
        }

        auto it = g_openFiles.find(handle);
        if (it == g_openFiles.end()) {
            throw std::runtime_error("__file_read: invalid file handle");
        }

        std::fstream* file = it->second;
        int size = -1;
        if (args.size() >= 2) {
            if (std::holds_alternative<ZeusType::BigInteger>(args[1])) {
                size = (int)std::get<ZeusType::BigInteger>(args[1]).toInt64();
            } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                const auto& val = std::get<LexerType::Value>(args[1]);
                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                    size = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
                }
            }
        }

        std::string content;
        if (size < 0) {
            // 读到文件末尾
            std::stringstream buffer;
            buffer << file->rdbuf();
            content = buffer.str();
        } else {
            // 读指定字节数
            char* buffer = new char[size + 1];
            file->read(buffer, size);
            size_t bytesRead = file->gcount();
            buffer[bytesRead] = '\0';
            content = std::string(buffer, bytesRead);
            delete[] buffer;
        }

        return ZeusType::UniversalVariant(content);
    }

    // __file_write(handle, data): 写入文件内容
    inline ZeusType::UniversalVariant builtin_file_write(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) {
            throw std::runtime_error("__file_write: requires 2 arguments (handle, data)");
        }

        int handle = 0;
        if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
            handle = (int)std::get<ZeusType::BigInteger>(args[0]).toInt64();
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                handle = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
            } else {
                throw std::runtime_error("__file_write: handle must be an integer");
            }
        } else {
            throw std::runtime_error("__file_write: handle must be an integer");
        }

        auto it = g_openFiles.find(handle);
        if (it == g_openFiles.end()) {
            throw std::runtime_error("__file_write: invalid file handle");
        }

        std::fstream* file = it->second;

        // 提取要写入的数据
        std::string data;
        const auto& dataArg = args[1];

        if (std::holds_alternative<std::string>(dataArg)) {
            data = std::get<std::string>(dataArg);
        } else if (std::holds_alternative<LexerType::Value>(dataArg)) {
            const auto& val = std::get<LexerType::Value>(dataArg);
            if (std::holds_alternative<std::string>(val.data)) {
                data = std::get<std::string>(val.data);
            } else {
                data = val.toString();
            }
        } else if (std::holds_alternative<ZeusType::Variant>(dataArg)) {
            const auto& variant = std::get<ZeusType::Variant>(dataArg);
            if (std::holds_alternative<ZeusType::Bytes>(variant)) {
                const auto& bytes = std::get<ZeusType::Bytes>(variant);
                // 以二进制模式写入字节
                file->write(reinterpret_cast<const char*>(bytes.data.data()), bytes.data.size());
                return ZeusType::UniversalVariant(ZeusType::BigInteger(bytes.data.size()));
            }
        }

        file->write(data.c_str(), data.size());
        file->flush();

        return ZeusType::UniversalVariant(ZeusType::BigInteger(data.size()));
    }

    // __file_close(handle): 关闭文件
    inline ZeusType::UniversalVariant builtin_file_close(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_close: requires 1 argument (handle)");
        }

        int handle = 0;
        if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
            handle = (int)std::get<ZeusType::BigInteger>(args[0]).toInt64();
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                handle = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
            } else {
                throw std::runtime_error("__file_close: handle must be an integer");
            }
        } else {
            throw std::runtime_error("__file_close: handle must be an integer");
        }

        auto it = g_openFiles.find(handle);
        if (it == g_openFiles.end()) {
            throw std::runtime_error("__file_close: invalid file handle");
        }

        std::fstream* file = it->second;
        file->close();
        delete file;
        g_openFiles.erase(it);

        return ZeusType::UniversalVariant(ZeusType::BigInteger(0));
    }

    // __file_seek(handle, offset, whence): 移动文件指针
    // whence: 0 (SEEK_SET), 1 (SEEK_CUR), 2 (SEEK_END)
    inline ZeusType::UniversalVariant builtin_file_seek(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) {
            throw std::runtime_error("__file_seek: requires at least 2 arguments (handle, offset)");
        }

        int handle = 0;
        if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
            handle = (int)std::get<ZeusType::BigInteger>(args[0]).toInt64();
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                handle = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
            } else {
                throw std::runtime_error("__file_seek: handle must be an integer");
            }
        } else {
            throw std::runtime_error("__file_seek: handle must be an integer");
        }

        auto it = g_openFiles.find(handle);
        if (it == g_openFiles.end()) {
            throw std::runtime_error("__file_seek: invalid file handle");
        }

        int64_t offset = 0;
        if (std::holds_alternative<ZeusType::BigInteger>(args[1])) {
            offset = std::get<ZeusType::BigInteger>(args[1]).toInt64();
        } else if (std::holds_alternative<LexerType::Value>(args[1])) {
            const auto& val = std::get<LexerType::Value>(args[1]);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                offset = std::get<ZeusType::BigInteger>(val.data).toInt64();
            } else {
                throw std::runtime_error("__file_seek: offset must be an integer");
            }
        } else {
            throw std::runtime_error("__file_seek: offset must be an integer");
        }

        int whence = 0;
        if (args.size() >= 3) {
            if (std::holds_alternative<ZeusType::BigInteger>(args[2])) {
                whence = (int)std::get<ZeusType::BigInteger>(args[2]).toInt64();
            } else if (std::holds_alternative<LexerType::Value>(args[2])) {
                const auto& val = std::get<LexerType::Value>(args[2]);
                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                    whence = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
                }
            }
        }

        std::fstream* file = it->second;
        std::ios_base::seekdir dir = std::ios::beg;
        if (whence == 1) {
            dir = std::ios::cur;
        } else if (whence == 2) {
            dir = std::ios::end;
        }

        file->seekg(offset, dir);
        file->seekp(offset, dir);

        return ZeusType::UniversalVariant(ZeusType::BigInteger(0));
    }

    // __file_tell(handle): 获取当前文件指针位置
    inline ZeusType::UniversalVariant builtin_file_tell(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_tell: requires 1 argument (handle)");
        }

        int handle = 0;
        if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
            handle = (int)std::get<ZeusType::BigInteger>(args[0]).toInt64();
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                handle = (int)std::get<ZeusType::BigInteger>(val.data).toInt64();
            } else {
                throw std::runtime_error("__file_tell: handle must be an integer");
            }
        } else {
            throw std::runtime_error("__file_tell: handle must be an integer");
        }

        auto it = g_openFiles.find(handle);
        if (it == g_openFiles.end()) {
            throw std::runtime_error("__file_tell: invalid file handle");
        }

        std::fstream* file = it->second;
        std::streampos pos = file->tellg();

        return ZeusType::UniversalVariant(ZeusType::BigInteger((int64_t)pos));
    }

    // __file_exists(path): 检查文件是否存在
    inline ZeusType::UniversalVariant builtin_file_exists(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_exists: requires 1 argument (path)");
        }

        std::string path;
        if (std::holds_alternative<std::string>(args[0])) {
            path = std::get<std::string>(args[0]);
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<std::string>(val.data)) {
                path = std::get<std::string>(val.data);
            } else {
                throw std::runtime_error("__file_exists: path must be a string");
            }
        } else {
            throw std::runtime_error("__file_exists: path must be a string");
        }

        std::ifstream file(path);
        bool exists = file.good();
        file.close();

        return ZeusType::UniversalVariant(exists);
    }

    // __file_size(path): 获取文件大小
    inline ZeusType::UniversalVariant builtin_file_size(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_size: requires 1 argument (path)");
        }

        std::string path;
        if (std::holds_alternative<std::string>(args[0])) {
            path = std::get<std::string>(args[0]);
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<std::string>(val.data)) {
                path = std::get<std::string>(val.data);
            } else {
                throw std::runtime_error("__file_size: path must be a string");
            }
        } else {
            throw std::runtime_error("__file_size: path must be a string");
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("__file_size: cannot open file: " + path);
        }

        std::streamsize size = file.tellg();
        file.close();

        return ZeusType::UniversalVariant(ZeusType::BigInteger((int64_t)size));
    }

    // __file_delete(path): 删除文件
    inline ZeusType::UniversalVariant builtin_file_delete(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 1) {
            throw std::runtime_error("__file_delete: requires 1 argument (path)");
        }

        std::string path;
        if (std::holds_alternative<std::string>(args[0])) {
            path = std::get<std::string>(args[0]);
        } else if (std::holds_alternative<LexerType::Value>(args[0])) {
            const auto& val = std::get<LexerType::Value>(args[0]);
            if (std::holds_alternative<std::string>(val.data)) {
                path = std::get<std::string>(val.data);
            } else {
                throw std::runtime_error("__file_delete: path must be a string");
            }
        } else {
            throw std::runtime_error("__file_delete: path must be a string");
        }

        int result = std::remove(path.c_str());
        if (result != 0) {
            throw std::runtime_error("__file_delete: failed to delete file: " + path);
        }

        return ZeusType::UniversalVariant(ZeusType::BigInteger(0));
    }

    // =====================================================================
    // 注册文件操作函数
    // =====================================================================

    inline void registerFileBuiltins() {
        registerBuiltin("__file_open", builtin_file_open, 1, 2);
        registerBuiltin("__file_read", builtin_file_read, 1, 2);
        registerBuiltin("__file_write", builtin_file_write, 2, 2);
        registerBuiltin("__file_close", builtin_file_close, 1, 1);
        registerBuiltin("__file_seek", builtin_file_seek, 2, 2);
        registerBuiltin("__file_tell", builtin_file_tell, 1, 1);
        registerBuiltin("__file_exists", builtin_file_exists, 1, 1);
        registerBuiltin("__file_size", builtin_file_size, 1, 1);
        registerBuiltin("__file_delete", builtin_file_delete, 1, 1);
    }

}
