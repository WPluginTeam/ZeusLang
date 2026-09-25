# Zeus 语法规范（按当前实现）

> 本文档基于当前代码实现整理，主参考为 `ZeusParser/TreeParser.hpp`、`ZeusParser/LexerParser.hpp`、`ZeusIR/IRGenerator.hpp`、`ZeusIR/IRExprFold.hpp`，并结合现有 `docs/` 示例。  
> 若文档与实现冲突，以实现行为为准。

## 1. 词法规则

### 1.1 注释

```ebnf
line_comment  ::= "//" <any_char_except_newline>* ;
block_comment ::= "/*" <any_char>* "*/" ;
```

### 1.2 标识符与关键字

```ebnf
identifier ::= [A-Za-z_][A-Za-z0-9_]* ;
```

实现中词法阶段将标识符与关键字都记为 `Keyword`，语义在语法/IR 阶段区分。常用语法关键字包括：

`var let const shared if else for func class return break delete throw try catch with import using original decl static new true false nil and or`

### 1.3 字面量

```ebnf
int_lit      ::= [0-9]+ ;
float_lit    ::= [0-9]+"."[0-9]+ ;
string_lit   ::= "\"" <char_or_escape>* "\"" ;
bytes_lit    ::= ("b" | "B") ("\"" <byte_char_or_escape>* "\"" | "'" <byte_char_or_escape>* "'") ;
bool_lit     ::= "true" | "false" ;
nil_lit      ::= "nil" ;
list_lit     ::= "[" [expr ("," expr)*] "]" ;
dict_lit     ::= "{" [expr ":" expr ("," expr ":" expr)*] "}" ;
```

字符串支持转义：`\n \t \r \\ \" \' \0 \b \f \v`。  
字节串支持常见转义与 `\xHH`。

### 1.4 运算符与分隔符

```text
+ - * / % = == != > < >= <= and or
. , : ; ( ) [ ] { } &
```

`and` / `or` 在词法后被当作操作符处理。

## 2. 程序与语句

```ebnf
program ::= { stmt_or_block } ;

stmt_or_block ::=
    var_decl ";"
  | assign_stmt ";"
  | expr_stmt ";"
  | return_stmt ";"
  | break_stmt ";"
  | delete_stmt ";"
  | throw_stmt ";"
  | import_stmt ";"
  | using_stmt ";"
  | func_fwd_decl ";"
  | if_block
  | for_block
  | func_block
  | class_block
  | try_block
  | with_block
  | using_block ;
```

### 2.1 变量声明

```ebnf
var_decl ::= ("var" | "let") ["const"] ["shared"] identifier "=" expr ;
```

说明：

- 顶层 `let`/`var` 均按全局处理；函数内 `let` 为局部，`var` 为全局。
- `shared` 变量仅允许与 `var` 组合（`let shared` 会报错）。

### 2.2 赋值

```ebnf
assign_stmt ::= lvalue "=" expr ;
lvalue      ::= identifier | expr "[" expr "]" | expr "." identifier ;
```

### 2.3 其他简单语句

```ebnf
expr_stmt   ::= expr ;
return_stmt ::= "return" [expr] ;
break_stmt  ::= "break" ;
delete_stmt ::= "delete" identifier { "," identifier } ;
throw_stmt  ::= "throw" expr ;
```

## 3. 控制流

### 3.1 if / else

```ebnf
if_block ::= "if" "(" expr ")" block_or_single
             [ "else" (if_block | block_or_single) ] ;

block_or_single ::= block | stmt_or_block_without_block ;
stmt_or_block_without_block ::=
    var_decl ";"
  | assign_stmt ";"
  | expr_stmt ";"
  | return_stmt ";"
  | break_stmt ";"
  | delete_stmt ";"
  | throw_stmt ";"
  | import_stmt ";"
  | using_stmt ";"
  | func_fwd_decl ";" ;
block ::= "{" { stmt_or_block } "}" ;
```

实现支持单语句 `if` / `else`（即不写 `{}`）。

### 3.2 for（四形态）

```ebnf
for_block ::= "for" "(" for_head ")" [for_binding_zone] block ;

for_head ::=
    cond_expr
  | iterable_expr
  | count_expr ;

cond_expr     ::= expr ;
iterable_expr ::= expr ;
numeric_expr  ::= expr ;

count_expr ::= expr ".." expr [ ".." ["="] expr ] | numeric_expr ;

for_binding_zone ::= "[" for_binding "]"
                   | "[" dict_binding ":" dict_binding "]" ;

for_binding  ::= ["const"] ["&"] identifier ;
dict_binding ::= ["const"] ["&"] identifier ;
```

语义上可识别：

- 条件循环：`for (cond) { ... }`
- 列表循环：`for (listExpr) [item] { ... }` / `for (listExpr) [&item] { ... }`
- 字典循环：`for (dictExpr) [key: value] { ... }`
- 计数循环：`for (N) [i] { ... }`、`for (start..end..step) [i] { ... }`

> 说明：`..=` 相关包含式范围在实现中已存在标记位，但与 `step` 写法耦合，仍建议以实际测试为准。

## 4. 函数与闭包

### 4.1 函数定义

```ebnf
func_block ::= "func" ["static"] [func_name | operator_decl] ["static"] "(" [param_list] ")" [capture_list] block ;

func_name ::= identifier ;
param_list ::= param { "," param } ;
param ::= identifier [ "=" default_expr ] | "*" identifier | "**" identifier ;
```

默认参数语法可写任意表达式 token；但当前 VM 侧稳定支持的是单字面量/`true`/`false`/`nil` 默认值。

### 4.2 捕获列表

```ebnf
capture_list ::= "[" capture_items "]" ;
capture_items ::= "" | "&" | capture_item { "," capture_item } ;
capture_item ::= ["&"] identifier ;
```

语义：

- `[]`：copy-all
- `[&]`：capture-all-by-ref
- `[&a, b]`：混合引用/拷贝捕获

### 4.3 前向声明

```ebnf
func_fwd_decl ::= "decl" "func" identifier "(" [identifier { "," identifier }] ")" ;
```

## 5. 类与对象

### 5.1 类声明

```ebnf
class_block ::= ("class" | "shared class" | "class shared") identifier block ;
```

类体常见成员：

- 字段：`var/let ... = ...;`
- 方法：`func ... { ... }`
- 类内实例别名声明：`using &alias;`

### 5.2 对象创建

```ebnf
new_expr ::= "new" class_expr [ "(" [arg_list] ")" ] ;
class_expr ::= identifier { "." identifier } ;
```

当前实现支持 `new ClassName(args...)`，并走 `init(...)` 调用链（若类定义了 `init`）。

## 6. 运算符重载

```ebnf
operator_decl ::= "operator" "[" operator_symbol+ "]" ;
```

运行时已接入的典型符号：

- 算术/比较：`+ - * / % == != > < >= <=`
- 下标：`[]`、`[]=`（声明写法分别为 `operator[[]]`、`operator[[]=]`）
- 属性：`.`、`.=`
- 生命周期/上下文：`with`、`~with`、`~`

## 7. 模块与 using

### 7.1 import

```ebnf
import_stmt ::= "import" package_path [ "[" "&" [identifier] "]" ] ;
package_path ::= identifier { "." identifier } ;
```

支持 3 种模式：

- `import pkg.name;`（全局导入）
- `import pkg.name [&];`（命名空间导入，别名取最后一段）
- `import pkg.name [&alias];`（命名空间导入，显式别名）

### 7.2 using

```ebnf
using_stmt  ::= "using" "original" ;
using_block ::= "using" "original" block ;
```

另：类内 `using &alias;` 属于类语义，不是 `using original` 语义。

## 8. 异常与 with

```ebnf
try_block   ::= "try" block "catch" "(" ["&" identifier] ")" block ;
with_block  ::= "with" "(" expr ")" ["[" ["&"] identifier "]"] block ;
```

支持：

- `catch ()`
- `catch (&e)`
- `with (obj) { ... }`
- `with (obj) [&x] { ... }`

## 9. 表达式优先级（高到低）

```ebnf
expr          ::= logical_or_expr ;
arg_list      ::= arg { "," arg } ;
arg           ::= [identifier "="] expr ;
default_expr  ::= expr ;
operator_symbol ::= <operator token or keyword token in operator[...]> ;
```

1. 后缀：调用 `()`, 成员 `.`, 下标 `[]`
2. 一元：`+x`, `-x`
3. 乘除模：`* / %`
4. 加减：`+ -`
5. 比较：`== != > < >= <=`
6. 逻辑与：`and`
7. 逻辑或：`or`

## 10. 当前实现状态备注

- `continue` 已有 `NodeType` 枚举，但当前解析流程未接入关键字识别与 IR 生成。
- `while` 在当前解析器中未作为独立语法块接入（文档中的旧示例不代表当前可用）。
- 部分旧文档提到 “构造函数参数不支持” 已过时；当前实现支持 `new Class(args...)`。
- 运算符重载语法文档中 `operator[[]= ]` 的“必须空格”说法并非实现强约束，实际以解析结果为准。
