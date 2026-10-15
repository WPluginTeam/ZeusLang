#pragma once

#include <variant>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "ZeusType.hpp"
#include "ZeusReference.hpp"
#include "../ZeusParser/LexerType.hpp"

namespace ZeusType {

    // 命名参数包装器：在函数调用时,包装参数值和参数名
    struct NamedArg;  // 前向声明

    using UniversalVariant = std::variant<Variant, BigInteger, BigDecimal, bool, char, std::string, LexerType::Value, ClosureHandle, NativeFuncHandle, NativeClosureHandle, ClassHandle, ObjectHandle, NamespaceHandle, ArgInfo, std::shared_ptr<NamedArg>, ModuleProxy, SocketHandle, Bytes, SharedHandle, SharedClassHandle, SharedObjectHandle, LLVMClassHandle, ReferenceHandle, RawBytesRange>;

    // 命名参数包装器定义(需要在 UniversalVariant 之后)
    struct NamedArg {
        std::string name;           // 参数名
        UniversalVariant value;     // 参数值
    };

    // 类字段定义（默认值与是否 const）
    struct FieldDef {
        UniversalVariant defaultValue;
        bool isConst = false;
        int initializerProtoId = -1;
    };

    // 类信息表（运行期注册）
    struct ClassInfo {
        std::string name;
        // when non-empty, holds the alias name introduced by `using &alias;` within the class (e.g. "self").
        // empty string means no default alias declared.
        std::string usingThisAlias;

        std::unordered_map<std::string, FieldDef> fields;
        // Source declaration order; runtime construction uses this instead of
        // iterating the unordered field map.
        std::vector<std::string> fieldOrder;
        // Declared fields occupy indexed ZeusValue slots in each VM object.
        // The map is rebuilt from fieldOrder when class metadata is registered.
        std::unordered_map<std::string, std::size_t> fieldIndices;
        std::unordered_map<std::string, std::string> fieldInitializerNames;
        // Source-annotated field types are runtime contracts for dynamic writes.
        std::unordered_map<std::string, std::string> explicitFieldTypes;
        std::unordered_map<std::string, int>      staticMethods;
        std::unordered_map<std::string, int>      instanceMethods;

        // 运算符重载：运算符符号 -> 方法函数ID
        // 支持的运算符: +, -, *, /, %, ==, !=, <, >, <=, >=, [], () 等
        std::unordered_map<std::string, int>      operatorMethods;
    };

    class DeclaredFieldBuffer;

    // 对象实例（运行期）
    struct ObjectInstance {
        int classId = -1;
        int generation = 0;              // generation 用于检测 slot 复用后的悬空引用
        // Declared fields live in a malloc-backed contiguous ZeusValue payload.
        // Undeclared fields remain in this dynamic extension map.
        std::shared_ptr<DeclaredFieldBuffer> declaredFields;
        std::unordered_map<std::string, UniversalVariant> extensionValues;
        std::unordered_map<std::string, uint8_t>          flags; // 0x1 const

        int refCount = 1;                // 引用计数（初始为1）
        bool markedForDeletion = false;  // 标记为待删除（用于垃圾回收）
        bool destructorCalled = false;   // operator[~] 最多调用一次
        bool finalizing = false;         // 防止析构期间递归进入最终释放

        // 增加引用计数
        void retain() { ++refCount; }

        // 减少引用计数，返回是否应该释放
        bool release() { return --refCount <= 0; }

        // 清空对象数据（释放内存）
        void clear() {
            declaredFields.reset();
            extensionValues.clear();
            flags.clear();
            classId = -1;
            refCount = 0;
            markedForDeletion = true;
            destructorCalled = false;
            finalizing = false;
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
        // Keeps insertion order while allowing O(1) expected deletion. Deleted
        // slots remain tombstones until compaction; a reinserted key gets a new slot.
        std::vector<std::string> order;
        // Views point at the immutable keys owned by `data`; erase the index
        // before erasing a data key. This avoids a third copy of every key.
        std::unordered_map<std::string_view, std::size_t> orderPositions;
        std::size_t deletedOrderCount = 0;

        bool hasOrderKey(const std::string& key) const {
            return orderPositions.find(std::string_view(key)) != orderPositions.end();
        }

        bool isLiveOrderEntry(std::size_t position) const {
            if (position >= order.size()) return false;
            const auto found = orderPositions.find(std::string_view(order[position]));
            return found != orderPositions.end() && found->second == position;
        }

        void appendOrderKey(const std::string& key) {
            if (hasOrderKey(key)) return;
            const auto stored = data.find(key);
            if (stored == data.end()) {
                throw std::logic_error("ObjectData order key is missing from data");
            }
            const std::size_t position = order.size();
            order.push_back(key);
            try {
                const auto inserted = orderPositions.emplace(
                    std::string_view(stored->first), position);
                if (!inserted.second) order.pop_back();
            } catch (...) {
                order.pop_back();
                throw;
            }
        }

        void setValue(const std::string& key, const UniversalVariant& value) {
            auto found = data.find(key);
            if (found != data.end()) {
                found->second = value;
                return;
            }

            auto inserted = data.emplace(key, value);
            if (!inserted.second) {
                inserted.first->second = value;
                return;
            }
            try {
                appendOrderKey(key);
            } catch (...) {
                data.erase(inserted.first);
                throw;
            }
        }

        void eraseOrderKey(const std::string& key) {
            const auto found = orderPositions.find(std::string_view(key));
            if (found == orderPositions.end()) return;
            orderPositions.erase(found);
            ++deletedOrderCount;

            // Compact geometrically: total moved live keys stays linear over a
            // deletion run, while small dictionaries avoid compaction churn.
            constexpr std::size_t minimumSlots = 64;
            if (order.size() < minimumSlots ||
                deletedOrderCount < orderPositions.size()) {
                return;
            }

            std::size_t write = 0;
            for (std::size_t read = 0; read < order.size(); ++read) {
                auto current = orderPositions.find(std::string_view(order[read]));
                if (current == orderPositions.end() || current->second != read) {
                    continue;
                }
                if (write != read) {
                    order[write] = std::move(order[read]);
                    current->second = write;
                }
                ++write;
            }
            order.resize(write);
            deletedOrderCount = 0;
        }
        // Number of VM-owned Object values sharing this mutable storage.
        std::atomic<std::size_t> valueRefCount{1};
    };

    struct ArrayData {
        std::vector<ZeusType::UniversalVariant> data;
        // Number of VM-owned Array values sharing this mutable storage.
        std::atomic<std::size_t> valueRefCount{1};
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
