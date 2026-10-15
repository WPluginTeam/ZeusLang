
#ifndef TreeType_hpp
#define TreeType_hpp

enum class CodeBlockType {
    IF,
    FOR,
    FUNC,
    CLASS,
    INTERFACE,
    IMPL,
    TRY,    // try block
    CATCH,  // catch block
    WITH,   // with block
    USING,  // using original { ... } block

    BLOCK, // undefined
    USING_LLVM,
    USING_VM
};

enum class NodeType {
    VAR_DECL_STMT,  // Variable declaration statement
    VAR_ASSIGN_STMT,
    EXP_STMT,       // Expression statement {func call, number operate, }
    RETURN_STMT,    // Return statement
    BREAK_STMT,
    CONTINUE_STMT,
    DELETE_STMT,
    THROW_STMT,     // throw expression;

    FUNC_FWD_DECL_STMT, // decl func foo(...);
    INTERFACE_METHOD_DECL_STMT, // decl func foo(...); inside an interface
    
    IMPORT_STMT,    // import package.name;
    USING_STMT,     // using original;
    USING_BLOCK_STMT, // using original { ... };  范围限制的using original

    IF_DECL_STMT,
    FOR_DECL_STMT,
    FUNC_DECL_STMT,
    CLASS_DECL_STMT,
    INTERFACE_DECL_STMT,
    IMPL_DECL_STMT,
    TRY_DECL_STMT,  // try {} catch (&e) {}
    WITH_DECL_STMT, // with (expr) {}

    FUNC_DEF_STMT,

    KWARG_NODE,     // Named argument in function call: foo(a=10)

    WAIT,

    MAIN,
    USING_LLVM_BLOCK_STMT,
    USING_VM_BLOCK_STMT
};

namespace Debug {
    inline const char* toString(NodeType type) {
        switch (type) {
            case NodeType::VAR_DECL_STMT: return "VAR_DECL_STMT";
            case NodeType::VAR_ASSIGN_STMT: return "VAR_ASSIGN_STMT";
            case NodeType::EXP_STMT: return "EXP_STMT";
            case NodeType::RETURN_STMT: return "RETURN_STMT";
            case NodeType::BREAK_STMT: return "BREAK_STMT";
            case NodeType::CONTINUE_STMT: return "CONTINUE_STMT";
            case NodeType::DELETE_STMT: return "DELETE_STMT";
            case NodeType::THROW_STMT: return "THROW_STMT";
            case NodeType::FUNC_FWD_DECL_STMT: return "FUNC_FWD_DECL_STMT";
            case NodeType::INTERFACE_METHOD_DECL_STMT: return "INTERFACE_METHOD_DECL_STMT";
            case NodeType::IMPORT_STMT: return "IMPORT_STMT";
            case NodeType::USING_STMT: return "USING_STMT";
            case NodeType::USING_BLOCK_STMT: return "USING_BLOCK_STMT";
            case NodeType::USING_LLVM_BLOCK_STMT: return "USING_LLVM_BLOCK_STMT";
            case NodeType::USING_VM_BLOCK_STMT: return "USING_VM_BLOCK_STMT";
            case NodeType::IF_DECL_STMT: return "IF_DECL_STMT";
            case NodeType::FOR_DECL_STMT: return "FOR_DECL_STMT";
            case NodeType::FUNC_DECL_STMT: return "FUNC_DECL_STMT";
            case NodeType::CLASS_DECL_STMT: return "CLASS_DECL_STMT";
            case NodeType::INTERFACE_DECL_STMT: return "INTERFACE_DECL_STMT";
            case NodeType::IMPL_DECL_STMT: return "IMPL_DECL_STMT";
            case NodeType::TRY_DECL_STMT: return "TRY_DECL_STMT";
            case NodeType::WITH_DECL_STMT: return "WITH_DECL_STMT";
            case NodeType::KWARG_NODE: return "KWARG_NODE";
            default: return "UNKNOWN";
        }
    }

    inline const char* toString(CodeBlockType t) {
        switch (t) {
            case CodeBlockType::IF: return "IF";
            case CodeBlockType::FOR: return "FOR";
            case CodeBlockType::FUNC: return "FUNC";
            case CodeBlockType::CLASS: return "CLASS";
            case CodeBlockType::INTERFACE: return "INTERFACE";
            case CodeBlockType::IMPL: return "IMPL";
            case CodeBlockType::TRY: return "TRY";
            case CodeBlockType::CATCH: return "CATCH";
            case CodeBlockType::WITH: return "WITH";
            case CodeBlockType::USING: return "USING";
            case CodeBlockType::USING_LLVM: return "USING_LLVM";
            case CodeBlockType::USING_VM: return "USING_VM";
            default: return "UNKNOWN";
        }
    }
}

enum class TreeParserStatus {

};

#endif
