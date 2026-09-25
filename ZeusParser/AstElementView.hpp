#pragma once

#include <variant>

#include "TreeParser.hpp"

namespace TreeParser {

    // NodeAndBlock 的只读访问封装。
    //
    // AST 暂时继续使用 variant 保存历史结构，但语义分析阶段不再需要
    // 在每个循环里重复 holds_alternative/get。后续如果 AST 改成统一基类，
    // 只需要替换这个访问层即可。
    class AstElementView {
        const NodeAndBlock* value = nullptr;

    public:
        explicit AstElementView(const NodeAndBlock& element)
            : value(&element) {}

        bool valid() const {
            return value != nullptr;
        }

        bool isNode() const {
            return valid() && std::holds_alternative<Node>(*value);
        }

        bool isBlock() const {
            return valid() && std::holds_alternative<std::unique_ptr<CodeBlock>>(*value);
        }

        NodeId id() const {
            if (isNode()) return node().id;

            const auto* codeBlock = block();
            return codeBlock ? codeBlock->id : InvalidNodeId;
        }

        const Node& node() const {
            return std::get<Node>(*value);
        }

        const CodeBlock* block() const {
            if (!isBlock()) return nullptr;
            return std::get<std::unique_ptr<CodeBlock>>(*value).get();
        }
    };

}
