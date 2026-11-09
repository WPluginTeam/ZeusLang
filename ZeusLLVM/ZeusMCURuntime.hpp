#ifndef ZeusMCURuntime_hpp
#define ZeusMCURuntime_hpp

#include <string>
#include <vector>
#include <set>
#include <sstream>

namespace ZeusLLVM {

    class ZeusMCURuntime {
        public:
            // 判定是否应当启用 MCU 裸机运行时注入：
            // 1. 显式开启（explicitMcuFlag 为 true）
            // 2. 自适应推导：源码中导出了 zeus_setup / zeus_loop 等单片机入口符号
            static bool shouldEnable(
                bool explicitMcuFlag,
                const std::set<std::string>& nativeExportSymbols
            ) {
                if (explicitMcuFlag) return true;
                return nativeExportSymbols.count("zeus_setup") != 0 ||
                       nativeExportSymbols.count("zeus_loop") != 0;
            }

            // 发射内置 MCU 微运行时 LLVM IR 字符串
            static std::string emitRuntime(
                const std::set<std::string>& nativeExportSymbols,
                bool emitArduinoLifecycle
            ) {
                std::ostringstream out;
                out << "\n; ========================================================\n";
                out << "; Zeus Built-in MCU Bare-Metal Micro-Runtime\n";
                out << "; ========================================================\n\n";

                // 1. 全局标量槽（静态固定 1024 槽，零堆分配）
                out << "@zeus_global_scalar_slots = internal global [1024 x i64] zeroinitializer, align 8\n\n";

                out << "define void @zeus_global_store_i64(i32 %idx, i64 %val) {\n";
                out << "entry:\n";
                out << "  %idx64 = sext i32 %idx to i64\n";
                out << "  %ptr = getelementptr inbounds [1024 x i64], ptr @zeus_global_scalar_slots, i64 0, i64 %idx64\n";
                out << "  store i64 %val, ptr %ptr, align 8\n";
                out << "  ret void\n";
                out << "}\n\n";

                out << "define i64 @zeus_global_load_i64(i32 %idx) {\n";
                out << "entry:\n";
                out << "  %idx64 = sext i32 %idx to i64\n";
                out << "  %ptr = getelementptr inbounds [1024 x i64], ptr @zeus_global_scalar_slots, i64 0, i64 %idx64\n";
                out << "  %val = load i64, ptr %ptr, align 8\n";
                out << "  ret i64 %val\n";
                out << "}\n\n";

                // 2. 运行时基本桩
                out << "define void @zeus_gc_safepoint() {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_i64_check_division(i64 %left, i64 %right, i32 %line) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_range_check_i64(i64 %val, i32 %kind, i32 %line) {\nentry:\n  ret void\n}\n\n";
                out << "define i64 @zeus_checked_signed_integer(i64 %val, i32 %bits) {\nentry:\n  ret i64 %val\n}\n\n";
                out << "define i64 @zeus_checked_unsigned_integer(i64 %val, i32 %bits) {\nentry:\n  ret i64 %val\n}\n\n";
                out << "define ptr @zeus_string_literal_from_data(ptr %data, i64 %len) {\nentry:\n  ret ptr %data\n}\n\n";
                out << "define void @zeus_value_init_i64(ptr %slot, i64 %val) {\nentry:\n  store i64 %val, ptr %slot, align 8\n  ret void\n}\n\n";
                out << "define i64 @zeus_value_require_i64(ptr %slot) {\nentry:\n  %val = load i64, ptr %slot, align 8\n  ret i64 %val\n}\n\n";
                out << "define i64 @zeus_value_require_signed_integer(ptr %slot, i32 %bits) {\nentry:\n  %val = load i64, ptr %slot, align 8\n  ret i64 %val\n}\n\n";
                out << "define i64 @zeus_value_require_unsigned_integer(ptr %slot, i32 %bits) {\nentry:\n  %val = load i64, ptr %slot, align 8\n  ret i64 %val\n}\n\n";
                out << "define void @zeus_closure_bind_arguments(i32 %a, ptr %b, i64 %c, i32 %d, i32 %e, i32 %f, ptr %g, ptr %h, ptr %i, ptr %j, ptr %k, ptr %l, ptr %m, ptr %n) {\nentry:\n  ret void\n}\n\n";

                // 3. Arduino 标准入口（setup / loop）自动接管
                // 同时导出 C 符号（setup/loop）与 C++ Mangled 符号（_Z5setupv / _Z4loopv）
                if (emitArduinoLifecycle && nativeExportSymbols.count("zeus_setup") != 0 && nativeExportSymbols.count("zeus_loop") != 0) {
                    out << "; --- Arduino Standard Lifecycle Entrypoints ---\n";
                    out << "define void @setup() {\n";
                    out << "entry:\n";
                    out << "  %entry_code = call i32 @zeus_entry()\n";
                    out << "  call void @\"zeus_setup\"()\n";
                    out << "  ret void\n";
                    out << "}\n\n";

                    out << "define void @_Z5setupv() {\n";
                    out << "entry:\n";
                    out << "  call void @setup()\n";
                    out << "  ret void\n";
                    out << "}\n\n";

                    out << "define void @loop() {\n";
                    out << "entry:\n";
                    out << "  call void @\"zeus_loop\"()\n";
                    out << "  ret void\n";
                    out << "}\n\n";

                    out << "define void @_Z4loopv() {\n";
                    out << "entry:\n";
                    out << "  call void @loop()\n";
                    out << "  ret void\n";
                    out << "}\n\n";
                }

                return out.str();
            }

            // 从最终生成的 LLVM IR 中剥离已被内置定义的 declare 声明，避免与 define 重复冲突
            static void stripDefinedDeclarations(std::string& module) {
                static const std::vector<std::string> symbols = {
                    "@zeus_global_store_i64(",
                    "@zeus_global_load_i64(",
                    "@zeus_gc_safepoint(",
                    "@zeus_i64_check_division(",
                    "@zeus_range_check_i64(",
                    "@zeus_checked_signed_integer(",
                    "@zeus_checked_unsigned_integer(",
                    "@zeus_string_literal_from_data(",
                    "@zeus_value_init_i64(",
                    "@zeus_value_require_i64(",
                    "@zeus_value_require_signed_integer(",
                    "@zeus_value_require_unsigned_integer(",
                    "@zeus_closure_bind_arguments("
                };

                std::string rewritten;
                rewritten.reserve(module.size());

                size_t lineStart = 0;
                while (lineStart < module.size()) {
                    const size_t lineEnd = module.find('\n', lineStart);
                    const size_t end = (lineEnd == std::string::npos) ? module.size() : lineEnd;
                    const std::string line = module.substr(lineStart, end - lineStart);
                    bool remove = false;

                    if (line.rfind("declare ", 0) == 0) {
                        for (const auto& sym : symbols) {
                            if (line.find(sym) != std::string::npos) {
                                remove = true;
                                break;
                            }
                        }
                    }

                    if (!remove) {
                        rewritten.append(line);
                        if (lineEnd != std::string::npos) rewritten.push_back('\n');
                    }
                    if (lineEnd == std::string::npos) break;
                    lineStart = lineEnd + 1;
                }
                module = std::move(rewritten);
            }
    };

} // namespace ZeusLLVM

#endif
