
#ifndef IRDebugUtils_hpp
#define IRDebugUtils_hpp

#include "IRType.hpp"
#include "../ZeusParser/LexerType.hpp"
#include "../ZeusParser/TreeParser.hpp"

namespace IRDebugUtils {

    void printTokens (std::vector<LexerType::Token> tokens) {
        for (const auto& token : tokens) {
            if (std::holds_alternative<LexerType::Keyword>(token)) {
                std::cout << std::get<LexerType::Keyword>(token).name << " ";
            } else if (std::holds_alternative<LexerType::Operator>(token)) {
                std::cout << std::get<LexerType::Operator>(token).symbol << " ";
            } else if (std::holds_alternative<LexerType::Value>(token)) {
                const auto& value = std::get<LexerType::Value>(token);
                std::cout << value.toString() << " ";
            }
        }
        std::cout << std::endl;
    }

    void printInstruction (const IRType::IRInstr& instr) {
        std::cout << instr.toString() << std::endl;
    }

    void printVectorInstructions (const std::vector<IRType::IRInstr>& instructions) {
        for (const auto& instr : instructions) {
            printInstruction(instr);
        }
        std::cout << std::endl;
    }
}

#endif