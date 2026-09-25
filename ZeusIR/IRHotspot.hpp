#pragma once

#include "IRScope.hpp"
#include "IRType.hpp"
#include "IRUtils.hpp"
#include "../ZeusDebug/Utils.hpp"
#include "../ZeusParser/LexerType.hpp"
#include "../ZeusRunner/ZeusVMOperator.hpp"
#include <string>
#include <vector>
#include <variant>


namespace IRHotspotList {
    // 记录要优化的 VM Operator 列表，编译为单独的指令
    struct HotspotCode {
        std::vector<VMOperator> ops;
        IRType::IRInstr compiled;
    };


    std::vector<HotspotCode> hotspots;

    void addHotspot (const HotspotCode& code) {
        hotspots.push_back(code);
    }

    void registerHotspots () {
        // 防止重复注册导致列表膨胀
        hotspots.clear();

        /* Hotspot 1
            PUSH [], STORE_VALUE_GLOBAL idx type
        */
        HotspotCode h1 {
            { VMOperator::PUSH, VMOperator::STORE_VALUE_GLOBAL },
            { VMOperator::HOTSPOT, { LexerType::Value(0), }}
        };
        addHotspot(h1);

    }

}





namespace IRHotspotCompiler {

    class HotspotCompiler {
        public:
        std::vector<IRType::IRInstr> instrs;

        HotspotCompiler(const std::vector<IRType::IRInstr>& instrs_): instrs(instrs_) {}


        void compile () {
            using namespace IRType;

            // 结果指令集
            std::vector<IRInstr> out;

            size_t i = 0;
            while (i < instrs.size()) {
                bool matched = false;

                // 依次尝试所有已注册的热点模式
                for (const auto& hotspot : IRHotspotList::hotspots) {
                    const auto& pattern = hotspot.ops;
                    const size_t m = pattern.size();

                    if (m == 0 || i + m > instrs.size()) {
                        continue;
                    }

                    // 匹配从 i 开始的 op 序列
                    bool ok = true;
                    for (size_t j = 0; j < m; ++j) {
                        if (instrs[i + j].op != pattern[j]) {
                            ok = false;
                            break;
                        }
                    }

                    if (!ok) {
                        continue;
                    }

                    // 命中热点：基于 compiled 生成新指令，并把匹配到的原指令的参数依次追加
                    IRInstr newInstr = hotspot.compiled; // 使用热点定义中的 compiled 作为模板

                    for (size_t j = 0; j < m; ++j) {
                        const auto& opj  = instrs[i + j].op;
                        const auto& args = instrs[i + j].args;

                        if (opj == VMOperator::STORE_VALUE_GLOBAL) {
                            // 仅拷贝前两个参数：slot(index) 和 mut/const，丢弃可选的 inline 变量名
                            if (!args.empty()) {
                                newInstr.args.push_back(args[0]);
                            }
                            if (args.size() > 1) {
                                newInstr.args.push_back(args[1]);
                            }
                            continue;
                        }

                        // 其它指令形态（例如 PUSH）保持原样
                        for (const auto& a : args) {
                            newInstr.args.push_back(a);
                        }
                    }

                    out.push_back(std::move(newInstr));
                    i += m; // 跳过已匹配的片段
                    matched = true;

                    break;
                }

                if (!matched) {
                    // 未命中热点，原样复制当前指令
                    out.push_back(instrs[i]);
                    ++i;
                }
            }

            instrs.swap(out);
        }
            
    };
    
}