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
                bool emitArduinoLifecycle,
                size_t globalSlotsCount = 8192
            ) {
                // 槽数量向上对齐到 64，最小保证 64 个槽
                if (globalSlotsCount < 64) globalSlotsCount = 64;
                globalSlotsCount = (globalSlotsCount + 63) & ~size_t(63);

                std::ostringstream out;
                out << "\n; ========================================================\n";
                out << "; Zeus Built-in MCU Bare-Metal Micro-Runtime\n";
                out << "; Global Slots Allocated: " << globalSlotsCount << " (" << (globalSlotsCount * 8) << " bytes)\n";
                out << "; ========================================================\n\n";

                // 1. 全局标量槽（根据程序实际最大槽位动态生成尺寸）
                out << "@zeus_global_scalar_slots = internal global [" << globalSlotsCount << " x i64] zeroinitializer, align 8\n\n";

                out << "define void @zeus_global_store_i64(i32 %idx, i64 %val) {\n";
                out << "entry:\n";
                out << "  %valid_idx = icmp ult i32 %idx, " << globalSlotsCount << "\n";
                out << "  br i1 %valid_idx, label %do_store, label %out_of_bounds\n";
                out << "do_store:\n";
                out << "  %idx64 = zext i32 %idx to i64\n";
                out << "  %ptr = getelementptr inbounds [" << globalSlotsCount << " x i64], ptr @zeus_global_scalar_slots, i64 0, i64 %idx64\n";
                out << "  store i64 %val, ptr %ptr, align 8\n";
                out << "  ret void\n";
                out << "out_of_bounds:\n";
                out << "  ret void\n";
                out << "}\n\n";

                out << "define i64 @zeus_global_load_i64(i32 %idx) {\n";
                out << "entry:\n";
                out << "  %valid_idx = icmp ult i32 %idx, " << globalSlotsCount << "\n";
                out << "  br i1 %valid_idx, label %do_load, label %out_of_bounds\n";
                out << "do_load:\n";
                out << "  %idx64 = zext i32 %idx to i64\n";
                out << "  %ptr = getelementptr inbounds [" << globalSlotsCount << " x i64], ptr @zeus_global_scalar_slots, i64 0, i64 %idx64\n";
                out << "  %val = load i64, ptr %ptr, align 8\n";
                out << "  ret i64 %val\n";
                out << "out_of_bounds:\n";
                out << "  ret i64 0\n";
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
                out << "define void @zeus_value_release(ptr %slot) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_global_store_bool(i32 %idx, i32 %val) {\nentry:\n  %val64 = zext i32 %val to i64\n  call void @zeus_global_store_i64(i32 %idx, i64 %val64)\n  ret void\n}\n\n";
                out << "define void @zeus_global_store_string(i32 %idx, ptr %val) {\nentry:\n  %val64 = ptrtoint ptr %val to i64\n  call void @zeus_global_store_i64(i32 %idx, i64 %val64)\n  ret void\n}\n\n";
                out << "define void @zeus_global_clear(i32 %idx) {\nentry:\n  call void @zeus_global_store_i64(i32 %idx, i64 0)\n  ret void\n}\n\n";
                out << "define void @zeus_value_global_store(i32 %idx, ptr %slot) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_value_from_bigdecimal_decimal(ptr %out, ptr %str) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_value_copy_class_owned(ptr %dst, ptr %src) {\nentry:\n  ret void\n}\n\n";
                // Class & Object Lifecycle Stubs for MCU
                out << "define void @zeus_class_copy_register(ptr %a, ptr %b) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_copy_register_default(ptr %a, ptr %b) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_ref_retain(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_ref_release(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_ref_require_live(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_ref_require_managed_live(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_ref_register(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_ref_retain_owned(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_copy_remember(ptr %a, ptr %b) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_copy_remember_allocated(ptr %a, ptr %b) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_class_gc_register(ptr %a) {\nentry:\n  ret void\n}\n\n";
                out << "define ptr @zeus_class_copy_find(ptr %a) {\nentry:\n  ret ptr null\n}\n\n";
                out << "define void @zeus_dynamic_field_trace_register(ptr %a, ptr %b) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_oom_abort() {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_value_clone(ptr %dst, ptr %src) {\nentry:\n  %p0 = load i32, ptr %src, align 4\n  store i32 %p0, ptr %dst, align 4\n  %src_flg = getelementptr inbounds %ZeusValue, ptr %src, i32 0, i32 1\n  %dst_flg = getelementptr inbounds %ZeusValue, ptr %dst, i32 0, i32 1\n  %p1 = load i32, ptr %src_flg, align 4\n  store i32 %p1, ptr %dst_flg, align 4\n  %src_pay = getelementptr inbounds %ZeusValue, ptr %src, i32 0, i32 2\n  %dst_pay = getelementptr inbounds %ZeusValue, ptr %dst, i32 0, i32 2\n  %p2 = load i64, ptr %src_pay, align 8\n  store i64 %p2, ptr %dst_pay, align 8\n  ret void\n}\n\n";
                out << "define void @zeus_value_dynamic_field_copy_state(ptr %dst, ptr %src) {\nentry:\n  ret void\n}\n\n";
                out << "define void @zeus_value_from_class_ptr(ptr %out, ptr %ptr, ptr %type) {\nentry:\n  store i32 11, ptr %out, align 4\n  %flg = getelementptr inbounds %ZeusValue, ptr %out, i32 0, i32 1\n  store i32 0, ptr %flg, align 4\n  %addr = ptrtoint ptr %ptr to i64\n  %pay = getelementptr inbounds %ZeusValue, ptr %out, i32 0, i32 2\n  store i64 %addr, ptr %pay, align 8\n  ret void\n}\n\n";
                out << "define void @zeus_value_init_nil(ptr %out) {\nentry:\n  store i32 0, ptr %out, align 4\n  ret void\n}\n\n";
                out << "define ptr @zeus_value_require_class_ptr(ptr %slot, ptr %type) {\nentry:\n  %pay = getelementptr inbounds %ZeusValue, ptr %slot, i32 0, i32 2\n  %addr = load i64, ptr %pay, align 8\n  %ptr = inttoptr i64 %addr to ptr\n  ret ptr %ptr\n}\n\n";
                out << "define ptr @zeus_value_try_class_ptr(ptr %slot, ptr %type) {\nentry:\n  %pay = getelementptr inbounds %ZeusValue, ptr %slot, i32 0, i32 2\n  %addr = load i64, ptr %pay, align 8\n  %ptr = inttoptr i64 %addr to ptr\n  ret ptr %ptr\n}\n\n";

                // 2.5 原生内存分配桥接 (桥接 libc malloc / free)
                out << "define i64 @\"__zeus_malloc\"(i64 %size) {\n";
                out << "entry:\n";
                out << "  %sz32 = trunc i64 %size to i32\n";
                out << "  %ptr = call ptr @malloc(i32 %sz32)\n";
                out << "  %addr = ptrtoint ptr %ptr to i64\n";
                out << "  ret i64 %addr\n";
                out << "}\n\n";
                out << "define i64 @\"__zeus_free\"(i64 %addr) {\n";
                out << "entry:\n";
                out << "  %addr32 = trunc i64 %addr to i32\n";
                out << "  %ptr = inttoptr i32 %addr32 to ptr\n";
                out << "  call void @free(ptr %ptr)\n";
                out << "  ret i64 0\n";
                out << "}\n\n";

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

            // 剥离 MCU 裸机不支持的 Windows SEH 异常处理结构（cleanuppad / cleanupret / __CxxFrameHandler3）
            static void stripExceptionHandling(std::string& module) {
                static constexpr char kPersonality[] = " personality ptr @__CxxFrameHandler3";
                size_t pos = 0;
                while ((pos = module.find(kPersonality, pos)) != std::string::npos) {
                    module.erase(pos, sizeof(kPersonality) - 1);
                }
                static constexpr char kDeclPersonality[] = "declare i32 @__CxxFrameHandler3(...)\n";
                pos = module.find(kDeclPersonality);
                if (pos != std::string::npos) {
                    module.erase(pos, sizeof(kDeclPersonality) - 1);
                }
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
                    "@zeus_closure_bind_arguments(",
                    "@zeus_value_release(",
                    "@zeus_global_store_bool(",
                    "@zeus_global_store_string(",
                    "@zeus_global_clear(",
                    "@zeus_value_global_store(",
                    "@zeus_value_from_bigdecimal_decimal(",
                    "@zeus_value_copy_class_owned(",
                    "@zeus_class_copy_register(",
                    "@zeus_class_copy_register_default(",
                    "@zeus_class_ref_retain(",
                    "@zeus_class_ref_release(",
                    "@zeus_class_ref_require_live(",
                    "@zeus_class_ref_require_managed_live(",
                    "@zeus_class_ref_register(",
                    "@zeus_class_ref_retain_owned(",
                    "@zeus_class_copy_remember(",
                    "@zeus_class_copy_remember_allocated(",
                    "@zeus_class_gc_register(",
                    "@zeus_class_copy_find(",
                    "@zeus_dynamic_field_trace_register(",
                    "@zeus_oom_abort(",
                    "@zeus_value_clone(",
                    "@zeus_value_dynamic_field_copy_state(",
                    "@zeus_value_from_class_ptr(",
                    "@zeus_value_init_nil(",
                    "@zeus_value_require_class_ptr(",
                    "@zeus_value_try_class_ptr(",
                    "@\"__zeus_malloc\"(",
                    "@\"__zeus_free\"(",
                    "@__zeus_malloc(",
                    "@__zeus_free("
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
                        if (line.find("@malloc(i64)") != std::string::npos) {
                            rewritten += "declare ptr @malloc(i32)\n";
                            lineStart = (lineEnd == std::string::npos) ? module.size() : lineEnd + 1;
                            continue;
                        }
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
