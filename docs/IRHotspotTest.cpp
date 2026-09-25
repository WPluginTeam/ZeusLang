// IRHotspotTest.cpp
#include "./ZeusIR/IRHotspot.hpp"
#include "./ZeusRunner/ZeusVMOperator.hpp"
#include <iostream>

using namespace IRHotspotList;
using namespace IRHotspotCompiler;
using namespace IRType;

#include "./ZeusDebug/Utils.hpp"

int main () {
    IRHotspotList::registerHotspots();

    std::vector<IRType::IRInstr> test {
        IRType::IRInstr {
            VMOperator::PUSH,
            { LexerType::Value(42) }
        },
        IRType::IRInstr {
            VMOperator::STORE_VALUE_GLOBAL,
            { LexerType::Value(0), std::string("mutable") }
        },
    };

    IRHotspotList::registerHotspots();
    IRHotspotCompiler::HotspotCompiler comp(
        test
    );
    comp.compile();

    std::cout << "Compiled instructions:" << std::endl;
    for (const auto& instr : comp.instrs) {
        std::cout << instr.toString() << std::endl;
    }

}