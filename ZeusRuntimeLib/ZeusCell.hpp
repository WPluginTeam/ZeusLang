#pragma once

#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_MEM
// 使用 --debug-channels=MEM 开启 Cell/GC 相关调试输出

#include "ZeusVariant.hpp"
#include "../ZeusDebug/Utils.hpp"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <map>
#include <vector>
#include <chrono>
#include <algorithm>

// 前向声明 Cell 以供 HeapStats 使用
struct Cell;

// 全局 Heap 统计计数器
namespace HeapStats {
    inline std::atomic<uint64_t> totalCellsCreated{0};
    inline std::atomic<uint64_t> totalCellsDeleted{0};
    inline std::atomic<uint64_t> currentCellCount{0};
    
    // Cell 注册表（用于追踪所有存活的 Cell）
    inline std::unordered_set<Cell*> liveCells;
    inline std::mutex cellRegistryMutex;

    inline uint64_t getCellsCreated() { return totalCellsCreated.load(); }
    inline uint64_t getCellsDeleted() { return totalCellsDeleted.load(); }
    inline uint64_t getCurrentCount() { return currentCellCount.load(); }

    inline void recordCreate() {
        ++totalCellsCreated;
        ++currentCellCount;
    }
    
    inline void registerCell(Cell* c) {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        liveCells.insert(c);
    }
    
    inline void unregisterCell(Cell* c) {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        liveCells.erase(c);
    }
    
    inline std::vector<Cell*> getLiveCells() {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        return std::vector<Cell*>(liveCells.begin(), liveCells.end());
    }

    inline void recordDelete() {
        ++totalCellsDeleted;
        --currentCellCount;
    }

    inline void printStats() {
        std::cout << "[HEAP STATS] Created: " << getCellsCreated()
                  << ", Deleted: " << getCellsDeleted()
                  << ", Current: " << getCurrentCount() << std::endl;
    }
}


// 统一的运行期变量单元：
//  - value: 实际值（UniversalVariant）
//  - flags: 位标记 (bit0 = const)
struct Cell {
    ZeusType::UniversalVariant value;
    uint8_t flags; // 0 = var, 1 = const
    uint32_t debugId; // 唯一调试ID
    int refCount = 1; // 引用计数：持有者数量（局部槽、全局槽、upvalue 等）
    
    // 调试信息：创建时间戳（毫秒）
    uint64_t createdAtMs = 0;
    // 调试信息：创建来源（如 "STORE_LOCAL", "MAKE_CLOSURE" 等）
    const char* createdBy = "unknown";

    void retain() { 
        ++refCount; 
    }
    void release() {
        if (refCount <= 0) {
            // 双重释放检测！
            printf("[DOUBLE-RELEASE] Cell#%u refCount=%d createdBy=%s\n", debugId, refCount, createdBy);
            return;  // 避免进一步破坏
        }
        --refCount;
        if (refCount == 0) {
            HeapStats::unregisterCell(this);
            HeapStats::recordDelete();

            // 追踪ObjectHandle Cell的释放（调试通道）
            if (std::holds_alternative<ZeusType::ObjectHandle>(value)) {
                auto& oh = std::get<ZeusType::ObjectHandle>(value);
                ZEUS_DEBUG_EXEC({
                    std::cout << "[CELL-TRACK] Released ObjectHandle Cell #" << debugId 
                              << " for obj#" << oh.id << std::endl;
                });
            }

            // 内存回收日志 - 使用MEM调试通道
            ZEUS_DEBUG_EXEC({
                auto typeName = [](const ZeusType::UniversalVariant& v) -> const char* {
                    return std::visit([](auto&& arg) -> const char* {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr (std::is_same_v<T, ZeusType::BigInteger>) return "int";
                        else if constexpr (std::is_same_v<T, ZeusType::BigDecimal>) return "float";
                        else if constexpr (std::is_same_v<T, ZeusType::ReferenceHandle>) return "reference";
                        else if constexpr (std::is_same_v<T, ZeusType::LLVMClassHandle>) return "llvm-class";
                        else if constexpr (std::is_same_v<T, bool>) return "bool";
                        else if constexpr (std::is_same_v<T, char>) return "char";
                        else if constexpr (std::is_same_v<T, std::string>) return "string";
                        else if constexpr (std::is_same_v<T, LexerType::Value>) return "token";
                        else if constexpr (std::is_same_v<T, ZeusType::ClosureHandle>) return "closure";
                        else if constexpr (std::is_same_v<T, ZeusType::NativeFuncHandle>) return "native";
                        else return "variant"; // ZeusType::Variant 或其他复合类型
                    }, v);
                };
                std::cout << "[MEMORY   ] Clear cell: #" << debugId
                          << " type: " << typeName(value) << std::endl;
            });
            delete this;
        }
    }
    
    // 获取Cell存储的值类型字符串
    std::string getTypeString() const {
        return std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, ZeusType::BigInteger>) return "int";
            else if constexpr (std::is_same_v<T, ZeusType::BigDecimal>) return "float";
            else if constexpr (std::is_same_v<T, ZeusType::ReferenceHandle>) return "reference";
            else if constexpr (std::is_same_v<T, ZeusType::LLVMClassHandle>) return "llvm-class";
            else if constexpr (std::is_same_v<T, bool>) return "bool";
            else if constexpr (std::is_same_v<T, char>) return "char";
            else if constexpr (std::is_same_v<T, std::string>) return "string";
            else if constexpr (std::is_same_v<T, LexerType::Value>) return "token";
            else if constexpr (std::is_same_v<T, ZeusType::ClosureHandle>) return "closure";
            else if constexpr (std::is_same_v<T, ZeusType::NativeFuncHandle>) return "native";
            else if constexpr (std::is_same_v<T, ZeusType::ClassHandle>) return "class";
            else if constexpr (std::is_same_v<T, ZeusType::ObjectHandle>) return "object";
            else if constexpr (std::is_same_v<T, ZeusType::NamespaceHandle>) return "namespace";
            else if constexpr (std::is_same_v<T, ZeusType::ModuleProxy>) return "module";
            else if constexpr (std::is_same_v<T, ZeusType::SocketHandle>) return "socket";
            else if constexpr (std::is_same_v<T, ZeusType::Bytes>) return "bytes";
            else if constexpr (std::is_same_v<T, ZeusType::SharedHandle>) return "shared";
            else if constexpr (std::is_same_v<T, ZeusType::SharedClassHandle>) return "shared-class";
            else if constexpr (std::is_same_v<T, ZeusType::SharedObjectHandle>) return "shared-object";
            else if constexpr (std::is_same_v<T, ZeusType::ArgInfo>) return "arginfo";
            else if constexpr (std::is_same_v<T, std::shared_ptr<ZeusType::NamedArg>>) return "namedarg";
            else if constexpr (std::is_same_v<T, ZeusType::Variant>) {
                // 进一步细分Variant类型
                return std::visit([](auto&& inner) -> std::string {
                    using U = std::decay_t<decltype(inner)>;
                    if constexpr (std::is_same_v<U, ZeusType::String>) return "variant-string";
                    else if constexpr (std::is_same_v<U, ZeusType::Integer>) return "variant-int";
                    else if constexpr (std::is_same_v<U, ZeusType::Float>) return "variant-float";
                    else if constexpr (std::is_same_v<U, ZeusType::Boolean>) return "variant-bool";
                    else if constexpr (std::is_same_v<U, ZeusType::Null>) return "null";
                    else if constexpr (std::is_same_v<U, ZeusType::Object>) return "variant-object";
                    else if constexpr (std::is_same_v<U, ZeusType::Array>) return "variant-array";
                    else if constexpr (std::is_same_v<U, ZeusType::Function>) return "variant-function";
                    else if constexpr (std::is_same_v<U, ZeusType::Bytes>) return "variant-bytes";
                    else return "variant-other";
                }, arg);
            }
            else return "unknown";
        }, value);
    }
};

inline uint32_t __nextCellId() {
    static uint32_t g = 1;
    return g++;
}

// 获取当前时间戳（毫秒）
inline uint64_t __getCurrentTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 程序启动时间（用于计算相对时间）
inline uint64_t __getProgramStartTime() {
    static uint64_t startTime = __getCurrentTimeMs();
    return startTime;
}

namespace ZeusCellUtil {
    inline bool isConst(const Cell* c) { return c && (c->flags & 0x1); }
    inline void setConst(Cell* c) {      if (c) c->flags |= 0x1; }
    inline void clearConst(Cell* c) {    if (c) c->flags &= ~0x1; }
}

// 必须在Cell定义之后实现的HeapStats函数
namespace HeapStats {
    // 打印所有存活Cell的统计信息
    inline void dumpLiveCells() {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        
        std::map<std::string, int> typeCounts;
        for (Cell* c : liveCells) {
            if (c) {
                std::string type = c->getTypeString();
                typeCounts[type]++;
            }
        }
        
        std::cout << "\n[LIVE CELLS DUMP] Total: " << liveCells.size() << std::endl;
        for (const auto& [type, count] : typeCounts) {
            std::cout << "  " << type << ": " << count << std::endl;
        }
    }
    
    // 获取存活Cell的详细信息（用于Builtin）
    inline std::string getLiveCellsInfo() {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        
        std::map<std::string, std::vector<Cell*>> typeGroups;
        for (Cell* c : liveCells) {
            if (c) {
                std::string type = c->getTypeString();
                typeGroups[type].push_back(c);
            }
        }
        
        std::ostringstream oss;
        oss << "=== Live Cells Report ===\n";
        oss << "Total: " << liveCells.size() << " cells\n\n";
        
        for (const auto& [type, cells] : typeGroups) {
            oss << type << ": " << cells.size() << " cells\n";
            // 显示前5个的debugId和refCount
            int shown = 0;
            for (Cell* c : cells) {
                if (shown >= 5) {
                    oss << "  ... and " << (cells.size() - 5) << " more\n";
                    break;
                }
                oss << "  #" << c->debugId << " refCount=" << c->refCount;
                
                // 如果是ObjectHandle，显示objId
                if (std::holds_alternative<ZeusType::ObjectHandle>(c->value)) {
                    auto& oh = std::get<ZeusType::ObjectHandle>(c->value);
                    oss << " objId=" << oh.id << " gen=" << oh.generation;
                }
                
                oss << "\n";
                shown++;
            }
        }
        return oss.str();
    }
    
    // 获取存活时间最长的 object Cell（用于追踪泄漏）
    inline std::string getOldestObjectCells(int maxCount = 10) {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        
        // 收集所有 object 类型的 Cell
        std::vector<Cell*> objectCells;
        for (Cell* c : liveCells) {
            if (c && std::holds_alternative<ZeusType::ObjectHandle>(c->value)) {
                objectCells.push_back(c);
            }
        }
        
        // 按创建时间排序（最早创建的在前）
        std::sort(objectCells.begin(), objectCells.end(), 
            [](Cell* a, Cell* b) { return a->createdAtMs < b->createdAtMs; });
        
        uint64_t now = __getCurrentTimeMs();
        uint64_t startTime = __getProgramStartTime();
        
        std::ostringstream oss;
        oss << "=== Oldest Object Cells (top " << maxCount << ") ===\n";
        oss << "Current time: " << (now - startTime) << "ms since start\n";
        oss << "Total object cells: " << objectCells.size() << "\n\n";
        
        int shown = 0;
        for (Cell* c : objectCells) {
            if (shown >= maxCount) break;
            
            auto& oh = std::get<ZeusType::ObjectHandle>(c->value);
            uint64_t ageMs = now - c->createdAtMs;
            
            oss << "#" << c->debugId 
                << " objId=" << oh.id 
                << " gen=" << oh.generation
                << " refCount=" << c->refCount
                << " age=" << ageMs << "ms"
                << " createdBy=" << c->createdBy
                << "\n";
            shown++;
        }
        
        return oss.str();
    }
    
    // 获取泄漏的object Cell的ObjectHandle ID列表（用于深入分析）
    inline std::vector<std::pair<uint32_t, int>> getLeakedObjectCellIds() {
        std::lock_guard<std::mutex> lock(cellRegistryMutex);
        std::vector<std::pair<uint32_t, int>> result;
        
        for (Cell* c : liveCells) {
            if (c && std::holds_alternative<ZeusType::ObjectHandle>(c->value)) {
                auto& oh = std::get<ZeusType::ObjectHandle>(c->value);
                result.push_back({c->debugId, oh.id});
            }
        }
        
        return result;
    }
}
