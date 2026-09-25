#ifndef ZEUS_COMPILE_HPP
#define ZEUS_COMPILE_HPP

#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include "./ZeusParser/LexerParser.hpp"
#include "./ZeusParser/TreeParser.hpp"
#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_COMPILE
#include "./ZeusDebug/Utils.hpp"
#include "./ZeusIR/IRGenerator.hpp"
#include "./ZeusIR/IRStaticOperator.hpp"
#include "./ZeusIR/IRStaticCall.hpp"
#include "./ZeusIRReader/IRWrite.hpp"
#include "./ZeusIRReader/IRRead.hpp"
#include "./ZeusRunner/ZeusVM.hpp"
#include "./ZeusIR/IRHotspot.hpp"

// 先定义 ZeusCompile，再定义 Test 使用其符号
namespace ZeusCompile {
    // 计时开关，由 CLI (-t/--time) 设置（inline 防止多重定义）
    inline bool enableTiming = false;

    struct Result {
        std::vector<std::vector<IRType::IRInstr>> program;
        std::vector<std::string> globalNames;
        bool strictDecl = false; // 记录是否严格模式（影响 VM 行为：禁用 late resolve）
        std::string sourceFilePath; // 源文件路径（用于错误信息）
        std::string sourceCode;     // 源代码（用于错误信息显示行内容）
        // 统计信息（毫秒）
        double lexTimeMs = 0.0;
        double parseTimeMs = 0.0;
        double irTimeMs = 0.0;
    };

    inline bool containsHotspot(const std::vector<std::vector<IRType::IRInstr>>& program) {
        for (const auto& chunk : program) {
            for (const auto& ins : chunk) {
                if (ins.op == VMOperator::HOTSPOT) {
                    return true;
                }
            }
        }
        return false;
    }

    inline void indexifyLateResolves(Result& r) {
        if (r.globalNames.empty() || r.program.empty()) return;

        std::unordered_map<std::string, int> nameToIndex;
        nameToIndex.reserve(r.globalNames.size());
        std::unordered_map<std::string, int> suffixToIndex;
        std::unordered_set<std::string> suffixConflicts;

        for (int i = 0; i < (int)r.globalNames.size(); ++i) {
            const auto& name = r.globalNames[i];
            if (name.empty()) continue;
            nameToIndex[name] = i;

            size_t dotPos = name.rfind('.');
            if (dotPos != std::string::npos && dotPos + 1 < name.size()) {
                std::string suffix = name.substr(dotPos + 1);
                auto it = suffixToIndex.find(suffix);
                if (it == suffixToIndex.end()) {
                    suffixToIndex.emplace(std::move(suffix), i);
                } else {
                    suffixConflicts.insert(suffix);
                }
            }
        }

        for (auto& chunk : r.program) {
            for (auto& ins : chunk) {
                if (ins.op != VMOperator::LOAD_VALUE) continue;
                if (ins.args.size() != 1 || !std::holds_alternative<std::string>(ins.args[0])) continue;

                const std::string& name = std::get<std::string>(ins.args[0]);
                int idx = -1;

                auto it = nameToIndex.find(name);
                if (it != nameToIndex.end()) {
                    idx = it->second;
                } else {
                    auto it2 = suffixToIndex.find(name);
                    if (it2 != suffixToIndex.end() && suffixConflicts.find(name) == suffixConflicts.end()) {
                        idx = it2->second;
                    }
                }

                if (idx >= 0) {
                    std::vector<IRType::IRArgs> newArgs;
                    newArgs.push_back(std::string("global"));
                    newArgs.push_back(LexerType::Value(idx));
                    newArgs.push_back(name);
                    ins.args = std::move(newArgs);
                }
            }
        }
    }

    inline void optimizeHotspots(Result& r) {
        // 注册热点模式（幂等）
        IRHotspotList::registerHotspots();

        for (auto& chunk : r.program) {
            IRHotspotCompiler::HotspotCompiler comp(chunk);
            comp.compile();

            // 用优化后的指令替换原 chunk
            chunk = std::move(comp.instrs);
        }
    }

    inline Result compile(const std::string& src, bool strictDecl=false, bool enableHotspot=true) {
        // 注意：源文件信息的清除应在整个编译流程开始前进行（在 Zeus.cpp 中）
        // 这样包编译期间注册的源文件信息不会被清除
        
        auto t0 = std::chrono::steady_clock::now();
        std::vector<LexerType::Token> tokens = LexerParser::parse(src);
        auto t1 = std::chrono::steady_clock::now();
        std::vector<TreeParser::NodeAndBlock> ast = TreeParser::parse(tokens);
        auto t2 = std::chrono::steady_clock::now();
        IRGen::IRGenerator irGen;
        irGen.strictDecl = strictDecl;
        irGen.parse(ast);
        auto t3 = std::chrono::steady_clock::now();

        Result r;
        r.program = irGen.bytecodes;
        r.globalNames = irGen.globalNamesByIndex;
        r.strictDecl = strictDecl;
        r.lexTimeMs   = std::chrono::duration<double, std::milli>(t1 - t0).count();
        r.parseTimeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        r.irTimeMs    = std::chrono::duration<double, std::milli>(t3 - t2).count();

        if (irGen.status != IRType::IRCompilerStatus::Running) {
            r.program.clear();
        }

        if (!r.program.empty()) {
            // 类型已明确的类运算符复用函数签名/DirectCall 元信息；VM 忽略该标注。
            IRStaticOperator::annotate(r.program);
            // 普通静态函数调用补齐目标签名；VM 继续执行原 CALL_CLOSURE，LLVM 使用 typed call。
            IRStaticCall::annotate(r.program);
            indexifyLateResolves(r);
        }

        // 基于模式的热点优化（在 IR 生成成功后）
        if (enableHotspot && !r.program.empty()) {
            optimizeHotspots(r);
        }

        if (enableTiming) {
            double total = r.lexTimeMs + r.parseTimeMs + r.irTimeMs;
            std::cout << "[time] compile  total = " << total << " ms (lex=" << r.lexTimeMs
                      << ", parse=" << r.parseTimeMs << ", ir=" << r.irTimeMs << ")" << std::endl;
        }
        return r;
    }

    inline bool write(const Result& r, const std::string& outPath, std::string* err) {
        return IRBinary::writeIRProgram(r.program, outPath, &r.globalNames, nullptr, err);
    }

    inline Result read(const std::string& path, std::string* err, bool enableHotspot=true) {
        std::vector<std::string> globalNames;
        IRBinary::IRMeta meta;
        auto programVec = IRBinary::readIRProgram(path, err, &globalNames, &meta);

        if (programVec.empty()) {
            std::string err2;
            auto single = IRBinary::readIR(path, &err2);

            if (!single.empty()) {
                programVec.push_back(std::move(single));
            }
        }

        Result r;
        r.program = std::move(programVec);
        r.globalNames = std::move(globalNames);

        if (enableHotspot && !r.program.empty() && !containsHotspot(r.program)) {
            optimizeHotspots(r);
        }

        return r;
    }

    inline void run(const Result& r) {
        VM vm;
        
        // 注意：不在这里清除源文件信息，因为编译期间已经注册了所有包的源文件
        // 清除操作已移至 compile() 开始处
        
        // 设置主文件的错误报告信息（如果有的话）
        if (!r.sourceFilePath.empty()) {
            VMException::setSourceFilePath(r.sourceFilePath);
        }
        if (!r.sourceCode.empty()) {
            VMException::setSourceCode(r.sourceCode);
        }
        
        // 设置全局指针以便builtin函数访问classes、objects和sockets
        ZeusBuiltins::g_vmClasses = &vm.classes;
        ZeusBuiltins::g_vmObjects = &vm.objects;
        ZeusBuiltins::g_vmSockets = &vm.sockets;
        ZeusBuiltins::g_vmClosuresSize = &vm.closuresCount;
        ZeusBuiltins::g_vmGetFreeClosureSlotsSize = [&vm]() -> size_t { return vm.freeClosureSlots.size(); };
        ZeusBuiltins::g_vmFreeObjectSlots = &vm.freeObjectSlots;
        ZeusBuiltins::g_vmFreeObjectSlotsSize = &vm.freeObjectSlotsCount;
        ZeusBuiltins::g_vmGlobalNames = &vm.globalNames;
        ZeusBuiltins::g_vmPrototypesSize = &vm.prototypesCount;
        ZeusBuiltins::g_vmFreeSocketSlots = &vm.freeSocketSlots;
        ZeusBuiltins::g_vmFreeSocketSlotsSize = &vm.freeSocketSlotsCount;
        ZeusBuiltins::g_vmSocketsMutex = &vm.socketsMutex;
        ZeusBuiltins::g_vmDumpClosureDetails = [&vm]() -> std::string { return vm.dumpClosureDetails(); };
        g_vmClassesPtr = &vm.classes;
        g_vmObjectsPtr = &vm.objects;

        vm.registerPrototypes(r.program);

        // 设置主程序到线程管理器（用于多线程支持）
        ZeusThread::getThreadManager().setMainProgram(r.program, r.globalNames);

        // 设置线程名解析器（用于错误报告显示线程名）
        VMException::setThreadNameResolver([]() -> std::string {
            auto nativeId = std::this_thread::get_id();
            std::string name = ZeusThread::getThreadManager().getThreadNameByNativeId(nativeId);

            if (name.empty()) {
                return "thread-main";
            }

            return name;
        });

        // 设置闭包解析器（用于线程支持，线程安全）
        ZeusThread::setGlobalClosureResolver([&vm](int closureId) -> int {
            std::shared_lock<std::shared_mutex> lock(vm.closuresMutex);
            if (closureId >= 0 && closureId < (int)vm.closures.size()) {
                return vm.closures[closureId].protoId;
            }
            return -1;
        });
        
        // 设置 BoundThis 解析器（用于线程支持绑定方法，线程安全）
        ZeusThread::setGlobalBoundThisResolver([&vm](int closureId) -> std::optional<ZeusType::ObjectHandle> {
            std::shared_lock<std::shared_mutex> lock(vm.closuresMutex);
            if (closureId >= 0 && closureId < (int)vm.closures.size()) {
                return vm.closures[closureId].boundThis;
            }
            return std::nullopt;
        });
        
        // 设置共享对象系统解析器（用于线程访问主 VM 的完整环境）
        ZeusThread::setGlobalSharedObjectResolver([&vm]() -> ZeusThread::SharedObjectSystem {
            ZeusThread::SharedObjectSystem shared;
            
            // 原有共享
            shared.classes = &vm.classes;
            shared.objects = &vm.objects;
            shared.mutex = &vm.objectsMutex;
            shared.freeObjectSlots = &vm.freeObjectSlots;  // GC: 共享空闲槽位
            
            // 扩展共享：全局变量、闭包系统等（使用 void* 转换）
            shared.global = static_cast<void*>(&vm.global);
            shared.globalNames = &vm.globalNames;
            shared.closures = static_cast<void*>(&vm.closures);
            shared.freeClosureSlots = &vm.freeClosureSlots;  // GC: 共享闭包空闲槽位
            shared.closuresMutex = &vm.closuresMutex;        // 闭包系统锁
            shared.prototypes = static_cast<void*>(&vm.prototypes);
            shared.nameToClosure = static_cast<void*>(&vm.nameToClosure);
            shared.nameToClass = static_cast<void*>(&vm.nameToClass);
            
            return shared;
        });
        
        // 设置 retain/release 函数解析器（用于线程参数的引用计数管理）
        ZeusThread::setGlobalRetainReleaseResolver([&vm]() -> std::tuple<
            std::function<void(int)>, std::function<void(int)>, 
            std::function<void(int)>, std::function<void(int)>> {
            return std::make_tuple(
                [&vm](int objId) { vm.retainObject(objId); },      // retainObject
                [&vm](int objId) { vm.releaseObject(objId); },     // releaseObject
                [&vm](int cloId) { vm.retainClosure(cloId); },     // retainClosure
                [&vm](int cloId) { vm.releaseClosure(cloId); }     // releaseClosure
            );
        });

        if (!r.globalNames.empty()) {
            vm.setGlobalNames(r.globalNames);
        }

        if (r.strictDecl) {
            vm.enableLateResolve = false;
        }

        auto t0 = std::chrono::steady_clock::now();
        for (size_t chunkIdx = 0; chunkIdx < r.program.size(); ++chunkIdx) {
            const auto& chunk = r.program[chunkIdx];
            ZEUS_DEBUG_PRINT("[ZeusCompile::run] Chunk #%zu (size=%zu)", chunkIdx, chunk.size());
            
            if (vm.vmstat != ZeusType::VMStatus::Running) {
                ZEUS_DEBUG_PRINT("[ZeusCompile::run] VM not running, breaking (status=%d)", (int)vm.vmstat);
                break;
            }

            if (!chunk.empty() && chunk.front().op == VMOperator::FUNC_BEGIN) {
                ZEUS_DEBUG_PRINT("[ZeusCompile::run] Skipping FUNC_BEGIN chunk");
                continue;
            }

            vm.currentChunk = (int)chunkIdx;
            ZEUS_DEBUG_PRINT("[ZeusCompile::run] Calling vm.runChunk(%zu)...", chunkIdx);
            vm.runChunk(chunk);
            ZEUS_DEBUG_PRINT("[ZeusCompile::run] vm.runChunk(%zu) returned", chunkIdx);

            if (vm.vmstat != ZeusType::VMStatus::Running) {
                ZEUS_DEBUG_PRINT("[ZeusCompile::run] VM not running after chunk %zu, breaking (status=%d)", chunkIdx, (int)vm.vmstat);
                break;
            }
        }
        ZEUS_DEBUG_PRINT("[ZeusCompile::run] All chunks completed");
        // Ensure all spawned threads finish before vm is destroyed, because
        // thread resolvers and shared object pointers capture references to vm.
        ZeusThread::getThreadManager().joinAll();
        auto t1 = std::chrono::steady_clock::now();
        if (enableTiming) {
            double execMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cout << "[time] execute total = " << execMs << " ms" << std::endl;
        }
    }

    inline void printAll() {
        for (const auto& line : ZeusBuiltins::g_delayPrintBuffer) {
            ZEUS_DEBUG_PRINT("%s", line.c_str());
        }
    }
}

namespace Test {
    inline void testLexerParserIRGen(bool enableHotspot = true) {
        ZEUS_DEBUG_PRINT("===== Lexer, Parser and IR Generator Tests =====");
        std::string testStr = R"(


print("use --debug-channels=IR/PARSE to see IR details");
)";
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        std::vector<LexerType::Token> tokens = LexerParser::parse(testStr);
        auto t1 = clock::now();

        ZEUS_DEBUG_PRINT("------------------ Lex Tokens ------------------");

        for (auto &tk : tokens) {
            ZEUS_DEBUG_EXEC({ 
                std::visit([](auto const& v){ 
                    ZEUS_DEBUG_PRINT("%s", v.toString().c_str()); 
                }, tk); 
            });
        }

        auto ast = TreeParser::parse(tokens);
        auto t2 = clock::now();

        ZEUS_DEBUG_PRINT("------------------ AST Dump ---------------------");

        TreeParser::printAST(ast);
        IRGen::IRGenerator irGen; 
        irGen.parse(ast);

        auto t3 = clock::now();
        if (irGen.status != IRType::IRCompilerStatus::Running) {
            ZEUS_DEBUG_PRINT("IR Generation failed.");
            return;
        }

        // 对内置测试的 IR 做热点优化（可开关）
        std::vector<std::vector<IRType::IRInstr>> optimizedChunks = irGen.bytecodes;
        if (enableHotspot) {
            IRHotspotList::registerHotspots();
            for (auto& chunk : optimizedChunks) {
                IRHotspotCompiler::HotspotCompiler comp(chunk);
                comp.compile();
                chunk = std::move(comp.instrs);
            }
        }

        if (ZeusDebug::enabled() && ZeusDebug::channelEnabled(ZeusDebug::CH_IR)) {
            std::cout << "-------------------- IR Gen --------------------" << std::endl;
            int ci = 0; 
            for (auto &chunk : optimizedChunks) {
                std::cout << "== Chunk #" << ci++ << " ==" << std::endl;
                for (auto &ins : chunk) {
                    std::cout << ins.toString() << std::endl;
                }
            }
            std::cout << "-------------------- Output --------------------" << std::endl;
        } else {
            ZEUS_DEBUG_PRINT("------------------ Bytecode Chunks ------------------");

            int ci = 0;
            
            for (auto &chunk : optimizedChunks) {
                ZEUS_DEBUG_PRINT("== Chunk #%d ==", ci++);
                ZEUS_DEBUG_EXEC({
                    for (auto &ins : chunk)
                        ZEUS_DEBUG_PRINT("%s", ins.toString().c_str()); 
                });
            }
        }

        ZEUS_DEBUG_PRINT("------------------ Run ------------------");

        VM vm;
        
        // 设置全局指针以便builtin函数访问classes、objects和sockets
        ZeusBuiltins::g_vmClasses = &vm.classes;
        ZeusBuiltins::g_vmObjects = &vm.objects;
        ZeusBuiltins::g_vmSockets = &vm.sockets;
        ZeusBuiltins::g_vmClosuresSize = &vm.closuresCount;
        ZeusBuiltins::g_vmGetFreeClosureSlotsSize = [&vm]() -> size_t { return vm.freeClosureSlots.size(); };
        ZeusBuiltins::g_vmFreeObjectSlots = &vm.freeObjectSlots;
        ZeusBuiltins::g_vmFreeObjectSlotsSize = &vm.freeObjectSlotsCount;
        ZeusBuiltins::g_vmGlobalNames = &vm.globalNames;
        ZeusBuiltins::g_vmPrototypesSize = &vm.prototypesCount;
        ZeusBuiltins::g_vmFreeSocketSlots = &vm.freeSocketSlots;
        ZeusBuiltins::g_vmFreeSocketSlotsSize = &vm.freeSocketSlotsCount;
        ZeusBuiltins::g_vmSocketsMutex = &vm.socketsMutex;
        g_vmClassesPtr = &vm.classes;
        g_vmObjectsPtr = &vm.objects;
        
        vm.registerPrototypes(optimizedChunks);

        if (!irGen.globalNamesByIndex.empty())
            vm.setGlobalNames(irGen.globalNamesByIndex);

        auto tRunStart = clock::now();

        int ci = 0;

        for (auto &chunk : optimizedChunks) {
            if (vm.vmstat != ZeusType::VMStatus::Running)
                break;

            if (!chunk.empty() && chunk.front().op == VMOperator::FUNC_BEGIN) {
                ci++;
                continue;
            }

            vm.runChunk(chunk);
            ci++;

            if (vm.vmstat != ZeusType::VMStatus::Running)
                break;
        }
        auto tRunEnd = clock::now();

        if (ZeusCompile::enableTiming) {

            double lexMs   = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double parseMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
            double irMs    = std::chrono::duration<double, std::milli>(t3 - t2).count();
            double compTot = lexMs + parseMs + irMs;
            double execMs  = std::chrono::duration<double, std::milli>(tRunEnd - tRunStart).count();
            
            std::cout << "[time] builtin compile total = " << compTot
                      << " ms (lex=" << lexMs << ", parse=" << parseMs << ", ir=" << irMs << ")" << std::endl;
            std::cout << "[time] builtin execute total = " << execMs << " ms" << std::endl;
        }

        ZEUS_DEBUG_PRINT("------------------ Delayed Print Output ------------------");
        ZeusBuiltins::printAll();
    }
}

#endif // ZEUS_COMPILE_HPP
