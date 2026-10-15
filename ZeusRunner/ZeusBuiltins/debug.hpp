// ZeusBuiltins/debug.hpp - 调试和统计内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"
#include "gc.hpp"
#include "../ZeusThread.hpp"
#include "../../ZeusRuntimeLib/ZeusCell.hpp"
#include "../../ZeusRuntimeLib/ZeusSocketABI.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <psapi.h>
#endif

namespace ZeusBuiltins {

    // ===== 调试和统计函数 =====
    
    // __closure_stats(): 输出闭包统计信息，返回统计字符串
    inline ZeusType::UniversalVariant builtin_closure_stats(const std::vector<ZeusType::UniversalVariant>& args) {
        size_t total = g_vmClosuresSize ? *g_vmClosuresSize : 0;
        size_t free = g_vmGetFreeClosureSlotsSize ? g_vmGetFreeClosureSlotsSize() : 0;
        std::ostringstream oss;
        oss << "CLOSURE STATS: Total=" << total << ", Free=" << free << ", Active=" << (total - free);
        std::string info = oss.str();
        std::cout << info << std::endl;
        return ZeusType::UniversalVariant(info);
    }

    inline ZeusType::UniversalVariant builtin_closure_details(const std::vector<ZeusType::UniversalVariant>& args) {
        if (g_vmDumpClosureDetails) {
            std::string s = g_vmDumpClosureDetails();
            std::cout << s << std::endl;
            return ZeusType::UniversalVariant(s);
        }
        return ZeusType::UniversalVariant(std::string("No VM dump callback"));
    }

    // __vm_stats: 输出 VM 关键结构统计信息（对象、闭包、socket、原型、全局名、内存等）
    inline ZeusType::UniversalVariant builtin_vm_stats(const std::vector<ZeusType::UniversalVariant>& args) {
        // memory
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc;
        double memoryMB = -1.0;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            memoryMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        }
#else
        double memoryMB = -1.0;
#endif

        size_t objects = g_vmObjects ? g_vmObjects->size() : 0;
        size_t freeObjects = g_vmFreeObjectSlotsSize ? *g_vmFreeObjectSlotsSize : 0;
        size_t closures = g_vmClosuresSize ? *g_vmClosuresSize : 0;
        size_t freeClosures = g_vmGetFreeClosureSlotsSize ? g_vmGetFreeClosureSlotsSize() : 0;
        const size_t sockets = static_cast<size_t>(zeus_socket_open_count());
        const size_t connectedSockets =
            static_cast<size_t>(zeus_socket_connected_count());
        size_t prototypesCount = g_vmPrototypesSize ? *g_vmPrototypesSize : 0;
        size_t globalNames = g_vmGlobalNames ? g_vmGlobalNames->size() : 0;

        std::ostringstream oss;
        oss << "VM STATS: Memory=" << memoryMB << "MB, Objects=" << objects << ", FreeObjSlots=" << freeObjects
            << ", ClosuresCreated=" << closures << ", FreeClosSlots=" << freeClosures
            << ", Sockets(total=" << sockets << ", connected=" << connectedSockets << ")"
            << ", Prototypes=" << prototypesCount << ", GlobalNames=" << globalNames;

        std::string info = oss.str();
        std::cout << info << std::endl;
        return ZeusType::UniversalVariant(info);
    }
    
    // __gc_stats(): 输出 GC/内存统计信息
    inline ZeusType::UniversalVariant builtin_gc_stats(const std::vector<ZeusType::UniversalVariant>& args) {
        HeapStats::printStats();
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }
    
    // __dump_live_cells(): 输出所有存活Cell的详细信息
    inline ZeusType::UniversalVariant builtin_dump_live_cells(const std::vector<ZeusType::UniversalVariant>& args) {
        std::string info = HeapStats::getLiveCellsInfo();
        std::cout << info << std::endl;
        return ZeusType::UniversalVariant(info);
    }
    
    // __dump_oldest_cells(n=10): 输出存活时间最长的 n 个 object Cell
    inline ZeusType::UniversalVariant builtin_dump_oldest_cells(const std::vector<ZeusType::UniversalVariant>& args) {
        int maxCount = 10;
        if (!args.empty()) {
            if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
                maxCount = static_cast<int>(std::get<ZeusType::BigInteger>(args[0]).toInt64());
            } else if (std::holds_alternative<LexerType::Value>(args[0])) {
                auto& val = std::get<LexerType::Value>(args[0]);
                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                    maxCount = static_cast<int>(std::get<ZeusType::BigInteger>(val.data).toInt64());
                }
            }
        }
        
        std::string info = HeapStats::getOldestObjectCells(maxCount);
        std::cout << info << std::endl;
        return ZeusType::UniversalVariant(info);
    }
    
    // __get_live_cells_summary(): 获取存活Cell类型统计的字典
    inline ZeusType::UniversalVariant builtin_get_live_cells_summary(const std::vector<ZeusType::UniversalVariant>& args) {
        auto cells = HeapStats::getLiveCells();
        
        std::map<std::string, int> typeCounts;
        for (Cell* c : cells) {
            if (c) {
                std::string type = c->getTypeString();
                typeCounts[type]++;
            }
        }
        
        // 构建一个简单的统计字符串，格式: "type1:count1,type2:count2,..."
        std::ostringstream oss;
        oss << "total=" << cells.size();
        for (const auto& [type, count] : typeCounts) {
            oss << "," << type << "=" << count;
        }
        return ZeusType::UniversalVariant(oss.str());
    }
    
    // __dump_objects(): 输出objects数组的状态信息
    inline ZeusType::UniversalVariant builtin_dump_objects(const std::vector<ZeusType::UniversalVariant>& args) {
        if (!g_vmObjects || !g_vmClasses) {
            return ZeusType::UniversalVariant(std::string("Objects info not available"));
        }
        
        int total = static_cast<int>(g_vmObjects->size());
        int alive = 0;
        int deleted = 0;
        std::map<int, int> classIdCounts; // classId -> count
        
        for (const auto& obj : *g_vmObjects) {
            if (obj.markedForDeletion || obj.classId == -1) {
                deleted++;
            } else {
                alive++;
                classIdCounts[obj.classId]++;
            }
        }
        
        std::ostringstream oss;
        oss << "=== Objects Array Status ===\n";
        oss << "Total slots: " << total << "\n";
        oss << "Alive: " << alive << "\n";
        oss << "Deleted: " << deleted << "\n\n";
        oss << "By class:\n";
        for (const auto& [classId, count] : classIdCounts) {
            std::string className = "unknown";
            if (classId >= 0 && classId < static_cast<int>(g_vmClasses->size())) {
                className = (*g_vmClasses)[classId].name;
            }
            oss << "  " << className << " (id=" << classId << "): " << count << "\n";
        }
        
        std::cout << oss.str() << std::endl;
        return ZeusType::UniversalVariant(oss.str());
    }
    
    // __get_memory_usage(): 获取当前进程内存占用（MB）
    inline ZeusType::UniversalVariant builtin_get_memory_usage(const std::vector<ZeusType::UniversalVariant>& args) {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            // 返回工作集大小（实际物理内存占用），单位 MB
            double memoryMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
            return ZeusType::UniversalVariant(LexerType::Value(memoryMB));
        }
#endif
        return ZeusType::UniversalVariant(LexerType::Value(-1.0));
    }
    
    // __get_thread_count(): 获取 ThreadManager 中的线程数量
    inline ZeusType::UniversalVariant builtin_get_thread_manager_count(const std::vector<ZeusType::UniversalVariant>& args) {
        size_t count = ZeusThread::getThreadManager().threadCount();
        return ZeusType::UniversalVariant(LexerType::Value(static_cast<int>(count)));
    }

    // 注册调试相关内置函数

    // __leak_summary(): report live objects by class and cell createdBy to help locate leaks
    inline ZeusType::UniversalVariant builtin_leak_summary(const std::vector<ZeusType::UniversalVariant>& args) {
        if (!g_vmObjects || !g_vmClasses) {
            return ZeusType::UniversalVariant(std::string("Objects info not available"));
        }

        // Recursively scan a UniversalVariant for ObjectHandle references (including inside Variant(Array/Object) and NamedArg).
        auto scanForObjectHandles = [&](const ZeusType::UniversalVariant& v, const auto& self, const auto& onObjId) -> void {
            if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
                onObjId(std::get<ZeusType::ObjectHandle>(v).id);
                return;
            }
            if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) {
                auto ptr = std::get<std::shared_ptr<ZeusType::NamedArg>>(v);
                if (ptr) self(ptr->value, self, onObjId);
                return;
            }
            if (!std::holds_alternative<ZeusType::Variant>(v)) return;

            const auto& var = std::get<ZeusType::Variant>(v);
            std::visit([&](const auto& inner) {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, ZeusType::Array>) {
                    if (!inner.impl) return;
                    for (const auto& it : inner.impl->data) self(it, self, onObjId);
                } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
                    if (!inner.impl) return;
                    for (const auto& kv : inner.impl->data) self(kv.second, self, onObjId);
                } else {
                    // primitives / Bytes / Function / Null: no-op
                }
            }, var);
        };

        auto isNil = [](const ZeusType::UniversalVariant& v) -> bool {
            if (!std::holds_alternative<ZeusType::Variant>(v)) return false;
            const auto& var = std::get<ZeusType::Variant>(v);
            return std::holds_alternative<ZeusType::Null>(var);
        };

        int threadClassId = -1;
        int socketClassId = -1;
        int clientResClassId = -1;
        for (int i = 0; i < (int)g_vmClasses->size(); ++i) {
            const auto& name = (*g_vmClasses)[i].name;
            if (name == "Thread") threadClassId = i;
            else if (name == "Socket") socketClassId = i;
            else if (name == "ClientRes") clientResClassId = i;
        }

        struct FieldHealth {
            int alive = 0;
            int targetNonNil = 0;
            int targetIsClosure = 0;
            int argsNonNil = 0;
            int nameNonNil = 0;
            int handleNonNil = 0;
            int connectedNonNil = 0;
        } threadHealth, socketHealth;

        struct ClassStats {
            int alive = 0;
            long long refSum = 0;
            int maxRef = 0;
            int refGt1 = 0;
            std::map<std::string, int> byCreatedBy;
        };

        std::unordered_map<int, ClassStats> stats;
        struct TopRef {
            int ref = 0;
            int objId = -1;
            int classId = -1;
        };
        std::vector<TopRef> topRefs;
        auto considerTop = [&](int ref, int objId, int classId) {
            if (objId < 0) return;
            topRefs.push_back(TopRef{ref, objId, classId});
            std::sort(topRefs.begin(), topRefs.end(), [](const TopRef& a, const TopRef& b) {
                return a.ref > b.ref;
            });
            if (topRefs.size() > 5) topRefs.resize(5);
        };

        // Build inbound reference counts based on object fields (ObjectHandle only).
        // This helps distinguish "referenced by other objects" vs "kept alive by non-object roots (stack/cell/closure/etc)".
        std::vector<int> inboundByField;
        inboundByField.resize(g_vmObjects->size(), 0);
        // Like inboundByField, but also counts ObjectHandle references nested inside Variant(Array/Object) field values.
        std::vector<int> inboundByFieldDeep;
        inboundByFieldDeep.resize(g_vmObjects->size(), 0);
        // Count ObjectHandle references reachable from live Cells (including Variant containers).
        std::vector<int> inboundByCellDeep;
        inboundByCellDeep.resize(g_vmObjects->size(), 0);
        struct InboundDetail {
            int fromClientResThreadField = 0;
        } inboundDetail;

        for (size_t fromIdx = 0; fromIdx < g_vmObjects->size(); ++fromIdx) {
            const auto& fromObj = (*g_vmObjects)[fromIdx];
            if (fromObj.classId < 0 || fromObj.classId >= static_cast<int>(g_vmClasses->size()) ||
                fromObj.markedForDeletion) continue;
            const auto& classInfo = (*g_vmClasses)[fromObj.classId];
            for (const auto& kv : ZeusType::snapshotObjectFields(fromObj, classInfo)) {
                const auto& v = kv.second;
                // Direct field edges (ObjectHandle only)
                if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
                    int toId = std::get<ZeusType::ObjectHandle>(v).id;
                    if (toId >= 0 && toId < (int)g_vmObjects->size()) {
                        const auto& toObj = (*g_vmObjects)[toId];
                        if (toObj.classId >= 0 && !toObj.markedForDeletion) {
                            inboundByField[(size_t)toId]++;
                            if (clientResClassId >= 0 && fromObj.classId == clientResClassId && kv.first == "thread") {
                                inboundDetail.fromClientResThreadField++;
                            }
                        }
                    }
                }

                // Deep field edges (ObjectHandle nested inside containers)
                scanForObjectHandles(v, scanForObjectHandles, [&](int toId) {
                    if (toId < 0 || toId >= (int)g_vmObjects->size()) return;
                    const auto& toObj = (*g_vmObjects)[toId];
                    if (toObj.classId < 0 || toObj.markedForDeletion) return;
                    inboundByFieldDeep[(size_t)toId]++;
                });
            }
        }

        // Deep cell edges (roots): scan all live Cells and count any ObjectHandle references inside.
        {
            auto live = HeapStats::getLiveCells();
            for (Cell* c : live) {
                if (!c) continue;
                const auto value = c->readValue();
                scanForObjectHandles(value, scanForObjectHandles, [&](int toId) {
                    if (toId < 0 || toId >= (int)g_vmObjects->size()) return;
                    const auto& toObj = (*g_vmObjects)[toId];
                    if (toObj.classId < 0 || toObj.markedForDeletion) return;
                    inboundByCellDeep[(size_t)toId]++;
                });
            }
        }

        // Count alive objects and refCount sums
        for (size_t objIdx = 0; objIdx < g_vmObjects->size(); ++objIdx) {
            const auto& obj = (*g_vmObjects)[objIdx];
            if (obj.classId >= 0 && !obj.markedForDeletion) {
                auto& st = stats[obj.classId];
                st.alive++;
                st.refSum += obj.refCount;
                if (obj.refCount > st.maxRef) st.maxRef = obj.refCount;
                if (obj.refCount > 1) st.refGt1++;
                considerTop(obj.refCount, static_cast<int>(objIdx), obj.classId);

                // extra per-class health checks (field state)
                if (obj.classId == threadClassId) {
                    threadHealth.alive++;
                    const auto& classInfo = (*g_vmClasses)[obj.classId];
                    if (ZeusType::objectHasField(obj, classInfo, "target")) {
                        const auto target = ZeusType::readObjectField(obj, classInfo, "target");
                        if (!isNil(target)) {
                            threadHealth.targetNonNil++;
                            if (std::holds_alternative<ZeusType::ClosureHandle>(target)) threadHealth.targetIsClosure++;
                        }
                    }
                    if (ZeusType::objectHasField(obj, classInfo, "args") &&
                        !isNil(ZeusType::readObjectField(obj, classInfo, "args"))) threadHealth.argsNonNil++;
                    if (ZeusType::objectHasField(obj, classInfo, "name") &&
                        !isNil(ZeusType::readObjectField(obj, classInfo, "name"))) threadHealth.nameNonNil++;
                } else if (obj.classId == socketClassId) {
                    socketHealth.alive++;
                    const auto& classInfo = (*g_vmClasses)[obj.classId];
                    if (ZeusType::objectHasField(obj, classInfo, "handle") &&
                        !isNil(ZeusType::readObjectField(obj, classInfo, "handle"))) socketHealth.handleNonNil++;
                    if (ZeusType::objectHasField(obj, classInfo, "isConnected") &&
                        !isNil(ZeusType::readObjectField(obj, classInfo, "isConnected"))) socketHealth.connectedNonNil++;
                }
            }
        }

        // Walk live cells to attribute createdBy of object handles
        auto cells = HeapStats::getLiveCells();
        int objCells = 0;
        int staleCells = 0;
        for (Cell* c : cells) {
            if (!c) continue;
            const auto value = c->readValue();
            if (!std::holds_alternative<ZeusType::ObjectHandle>(value)) continue;
            objCells++;
            const auto oh = std::get<ZeusType::ObjectHandle>(value);
            if (oh.id < 0 || oh.id >= (int)g_vmObjects->size()) { staleCells++; continue; }
            const auto& obj = (*g_vmObjects)[oh.id];
            if (obj.generation != oh.generation || obj.classId < 0 || obj.markedForDeletion) {
                staleCells++;
                continue;
            }
            auto& st = stats[obj.classId];
            st.byCreatedBy[c->createdBy]++;
        }

        // Build output
        std::vector<std::pair<std::string, ClassStats>> sorted;
        sorted.reserve(stats.size());
        for (const auto& kv : stats) {
            int cid = kv.first;
            std::string cname = (cid >= 0 && cid < (int)g_vmClasses->size()) ? (*g_vmClasses)[cid].name : "unknown";
            sorted.push_back({cname + " (id=" + std::to_string(cid) + ")", kv.second});
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.second.alive > b.second.alive; });

        int totalAlive = 0;
        for (const auto& kv : stats) totalAlive += kv.second.alive;

        std::ostringstream oss;
        oss << "=== Leak/Object Summary ===\n";
        oss << "Live objects: " << totalAlive << "\n";
        oss << "Object cells: " << objCells << ", stale-handle cells: " << staleCells << "\n\n";
        for (const auto& [cname, st] : sorted) {
            oss << cname << ": alive=" << st.alive;
            if (st.alive > 0) {
                double avgRef = static_cast<double>(st.refSum) / static_cast<double>(st.alive);
                oss << ", avgRef=" << avgRef << ", maxRef=" << st.maxRef << ", ref>1=" << st.refGt1;
            }
            if (!st.byCreatedBy.empty()) {
                oss << "\n  by createdBy: ";
                bool first = true;
                for (const auto& kv : st.byCreatedBy) {
                    if (!first) oss << ", ";
                    oss << kv.first << "=" << kv.second;
                    first = false;
                }
            }
            oss << "\n";
        }
        if (!topRefs.empty()) {
            oss << "\nTop ref objects:\n";
            for (const auto& t : topRefs) {
                std::string cname = "unknown";
                if (t.classId >= 0 && t.classId < (int)g_vmClasses->size()) {
                    cname = (*g_vmClasses)[t.classId].name;
                }
                oss << "  obj#" << t.objId << " class=" << cname << " (id=" << t.classId << ") ref=" << t.ref << "\n";
            }
        }

        if (threadClassId >= 0) {
            oss << "\nThread field health:\n";
            oss << "  alive=" << threadHealth.alive
                << " targetNonNil=" << threadHealth.targetNonNil
                << " targetIsClosure=" << threadHealth.targetIsClosure
                << " argsNonNil=" << threadHealth.argsNonNil
                << " nameNonNil=" << threadHealth.nameNonNil << "\n";

            int threadWithInboundField = 0;
            int threadNoInboundButRefGt0 = 0;
            for (size_t objIdx = 0; objIdx < g_vmObjects->size(); ++objIdx) {
                const auto& obj = (*g_vmObjects)[objIdx];
                if (obj.classId != threadClassId || obj.markedForDeletion) continue;
                if (inboundByField[objIdx] > 0) threadWithInboundField++;
                else if (obj.refCount > 0) threadNoInboundButRefGt0++;
            }
            oss << "  inboundByField: withInbound=" << threadWithInboundField
                << " noInbound=" << threadNoInboundButRefGt0 << "\n";

            int threadWithInboundFieldDeep = 0;
            int threadWithInboundCellDeep = 0;
            for (size_t objIdx = 0; objIdx < g_vmObjects->size(); ++objIdx) {
                const auto& obj = (*g_vmObjects)[objIdx];
                if (obj.classId != threadClassId || obj.markedForDeletion) continue;
                if (inboundByFieldDeep[objIdx] > 0) threadWithInboundFieldDeep++;
                if (inboundByCellDeep[objIdx] > 0) threadWithInboundCellDeep++;
            }
            oss << "  inboundDeep: byField=" << threadWithInboundFieldDeep
                << " byCell=" << threadWithInboundCellDeep << "\n";
        }
        if (socketClassId >= 0) {
            oss << "Socket field health:\n";
            oss << "  alive=" << socketHealth.alive
                << " handleNonNil=" << socketHealth.handleNonNil
                << " isConnectedNonNil=" << socketHealth.connectedNonNil << "\n";

            int socketWithInboundField = 0;
            int socketNoInboundButRefGt0 = 0;
            for (size_t objIdx = 0; objIdx < g_vmObjects->size(); ++objIdx) {
                const auto& obj = (*g_vmObjects)[objIdx];
                if (obj.classId != socketClassId || obj.markedForDeletion) continue;
                if (inboundByField[objIdx] > 0) socketWithInboundField++;
                else if (obj.refCount > 0) socketNoInboundButRefGt0++;
            }
            oss << "  inboundByField: withInbound=" << socketWithInboundField
                << " noInbound=" << socketNoInboundButRefGt0 << "\n";

            int socketWithInboundFieldDeep = 0;
            int socketWithInboundCellDeep = 0;
            for (size_t objIdx = 0; objIdx < g_vmObjects->size(); ++objIdx) {
                const auto& obj = (*g_vmObjects)[objIdx];
                if (obj.classId != socketClassId || obj.markedForDeletion) continue;
                if (inboundByFieldDeep[objIdx] > 0) socketWithInboundFieldDeep++;
                if (inboundByCellDeep[objIdx] > 0) socketWithInboundCellDeep++;
            }
            oss << "  inboundDeep: byField=" << socketWithInboundFieldDeep
                << " byCell=" << socketWithInboundCellDeep << "\n";
        }

        if (clientResClassId >= 0) {
            oss << "\nInbound details:\n";
            oss << "  from ClientRes.thread field -> " << inboundDetail.fromClientResThreadField << "\n";
        }

        std::string out = oss.str();
        std::cout << out << std::endl;
        return ZeusType::UniversalVariant(out);
    }

    // 注册调试相关内置函数
    inline void registerDebugBuiltins() {
        registerBuiltin("__gc_stats", builtin_gc_stats, 0, 0);
        registerBuiltin("__closure_stats", builtin_closure_stats, 0, 0);
        registerBuiltin("__vm_stats", builtin_vm_stats, 0, 0);
        registerBuiltin("__closure_details", builtin_closure_details, 0, 0);
        registerBuiltin("__dump_live_cells", builtin_dump_live_cells, 0, 0);
        registerBuiltin("__dump_oldest_cells", builtin_dump_oldest_cells, 0, 1);
        registerBuiltin("__get_live_cells_summary", builtin_get_live_cells_summary, 0, 0);
        registerBuiltin("__dump_objects", builtin_dump_objects, 0, 0);
        registerBuiltin("__get_memory_usage", builtin_get_memory_usage, 0, 0);
        registerBuiltin("__leak_summary", builtin_leak_summary, 0, 0);
        registerGcBuiltins();
    }

} // namespace ZeusBuiltins
