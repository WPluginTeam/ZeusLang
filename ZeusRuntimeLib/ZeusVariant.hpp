#pragma once

#include <variant>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

#include "ZeusType.hpp"
#include "ZeusReference.hpp"
#include "../ZeusParser/LexerType.hpp"

namespace ZeusType {

    // 命名参数包装器：在函数调用时,包装参数值和参数名
    struct NamedArg;  // 前向声明

    using UniversalVariant = std::variant<Variant, BigInteger, BigDecimal, bool, char, std::string, LexerType::Value, ClosureHandle, NativeFuncHandle, NativeClosureHandle, ClassHandle, ObjectHandle, NamespaceHandle, ArgInfo, std::shared_ptr<NamedArg>, ModuleProxy, SocketHandle, Bytes, SharedHandle, SharedClassHandle, SharedObjectHandle, LLVMClassHandle, ReferenceHandle>;

    // 命名参数包装器定义(需要在 UniversalVariant 之后)
    struct NamedArg {
        std::string name;           // 参数名
        UniversalVariant value;     // 参数值
    };

    // 类字段定义（默认值与是否 const）
    struct FieldDef {
        UniversalVariant defaultValue;
        bool isConst = false;
    };

    // 类信息表（运行期注册）
    struct ClassInfo {
        std::string name;
        // when non-empty, holds the alias name introduced by `using &alias;` within the class (e.g. "self").
        // empty string means no default alias declared.
        std::string usingThisAlias;

        std::unordered_map<std::string, FieldDef> fields;
        std::unordered_map<std::string, int>      staticMethods;
        std::unordered_map<std::string, int>      instanceMethods;

        // 运算符重载：运算符符号 -> 方法函数ID
        // 支持的运算符: +, -, *, /, %, ==, !=, <, >, <=, >=, [], () 等
        std::unordered_map<std::string, int>      operatorMethods;
    };

    // 对象实例（运行期）
    struct ObjectInstance {
        int classId = -1;
        int generation = 0;              // generation 用于检测 slot 复用后的悬空引用
        std::unordered_map<std::string, UniversalVariant> values;
        std::unordered_map<std::string, uint8_t>          flags; // 0x1 const

        int refCount = 1;                // 引用计数（初始为1）
        bool markedForDeletion = false;  // 标记为待删除（用于垃圾回收）

        // 增加引用计数
        void retain() { ++refCount; }

        // 减少引用计数，返回是否应该释放
        bool release() { return --refCount <= 0; }

        // 清空对象数据（释放内存）
        void clear() {
            values.clear();
            flags.clear();
            classId = -1;
            refCount = 0;
            markedForDeletion = true;
            // 注意: 不重置 generation，在 slot 复用时由分配函数递增
        }
    };

    using NativeCallback = ZeusType::UniversalVariant(*)(const std::vector<ZeusType::UniversalVariant>&);

    enum class VMOperatorArgs {
        Mutable,
        Const,
    };

    enum class VMStatus {
        Running,
        Stopped,
        Error
    };

    // 延后定义容器数据，使用 UniversalVariant
    struct ObjectData {
        std::unordered_map<std::string, ZeusType::UniversalVariant> data;
        // 维护插入顺序（只在首次出现 key 时追加；覆盖不改变顺序）
        std::vector<std::string> order;
    };

    struct ArrayData {
        std::vector<ZeusType::UniversalVariant> data;
    };

    // 包命名空间数据：存储包的导出项映射到全局变量索引
    struct NamespaceData {
        std::string packageName;
        std::unordered_map<std::string, int> exports;  // 导出项名 -> 全局变量索引
    };

    // Socket 数据：网络套接字
    struct SocketData {
        int sockfd = -1;           // socket 文件描述符
        bool isConnected = false;  // 连接状态
        std::string remoteAddr;    // 远程地址
        int remotePort = 0;        // 远程端口
    };

    namespace debug {
        inline std::string VMOperatorArgs_toString(VMOperatorArgs arg) {
            switch (arg) {
                case VMOperatorArgs::Mutable: return "Mutable";
                case VMOperatorArgs::Const: return "Const";
                default: return "Unknown";
            }
        }
    }

}
