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

`var let const shared if else for func class interface impl return break delete throw try catch with import using original decl static new true false nil and or`

### 1.3 字面量

```ebnf
int_lit      ::= [0-9]+ ;
float_lit    ::= [0-9]+"."[0-9]+ ;
string_lit   ::= "\"" <char_or_escape>* "\"" ;
bytes_lit    ::= ("b" | "B") ("\"" <byte_char_or_escape>* "\"" | "'" <byte_char_or_escape>* "'") ;
bool_lit     ::= "true" | "false" ;
nil_lit      ::= "nil" ;
list_lit     ::= "[" [expr {"," expr} [","]] "]" ;
dict_lit     ::= "{" [expr ":" expr {"," expr ":" expr} [","]] "}" ;
```

字符串支持转义：`\n \t \r \\ \" \' \0 \b \f \v`。  
字节串支持常见转义与 `\xHH`。

### 1.4 运算符与分隔符

```text
+ - * / % = == != > < >= <= ! and or ||
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
  | interface_block
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
                   | "[" dict_binding ":" ["..."] dict_binding "]" ;

for_binding  ::= ["const"] ["&"] identifier ;
dict_binding ::= ["const"] ["&"] identifier ;
```

`for (init; condition; step) { ... }` 不属于 Zeus 语法；使用条件、迭代或范围计数形式。

语义上可识别：

- 条件循环：`for (cond) { ... }`
- 列表循环：`for (listExpr) [item] { ... }` / `for (listExpr) [&item] { ... }`
- 字典循环：`for (dictExpr) [key: value] { ... }`
- 计数循环：`for (N) [i] { ... }`、`for (start..end..step) [i] { ... }`

> 说明：`..=` 相关包含式范围在实现中已存在标记位，但与 `step` 写法耦合，仍建议以实际测试为准。

## 4. 函数与闭包

### 4.1 函数定义

```ebnf
func_block ::= "func" ["static"] (func_name | operator_decl) ["static"] "(" [param_list] ")" [capture_list] block ;

func_name ::= identifier ;
param_list ::= param { "," param } [","] ;
param ::= identifier [":" type_name] [ "=" default_expr ] | "*" identifier | "**" identifier ;
type_name ::= ["&"] identifier {"." identifier} ;
```

默认参数可以是表达式。参数未传入时，VM 会在每次调用中按参数顺序求值；已显式传入的参数（包括 `nil`）不会触发默认表达式。

### 4.2 捕获列表

```ebnf
capture_list ::= "[" capture_items "]" ;
capture_items ::= "" | "&" | capture_item { "," capture_item } [","] ;
capture_item ::= ["&"] identifier ;
```

语义：

- `[]`：copy-all
- `[&]`：capture-all-by-ref
- `[&a, b]`：混合引用/拷贝捕获

### 4.3 前向声明

```ebnf
func_fwd_decl ::= "decl" "func" identifier "(" [identifier { "," identifier } [","]] ")" ;
```

### 4.4 函数修饰符

```ebnf
static_function ::= "@static" func_block ;
no_safepoint_function ::= "@no_safepoint" func_block ;
inline_function ::= "inline" func_block ;
native_function ::= "@native" "(" string_lit ")" func_fwd_decl ;
```

当前支持 `@static`、`@no_safepoint` 函数定义，`inline func` 内联请求，以及 `@native("symbol") decl func ...;` 原生函数声明。修饰符必须紧邻对应函数声明；未知修饰符、错误参数和不匹配的声明形式会产生语法诊断。

## 5. 类与对象

### 5.1 类声明

```ebnf
class_block ::= ("class" | "shared class" | "class shared") identifier
                [ "[" type_path { "," type_path } "]" ] block ;
interface_block ::= "interface" identifier "{" { interface_method_decl | receiver_alias } "}" ;
interface_method_decl ::= "decl" "func" ["static"] identifier ["static"]
                          "(" [param_list] ")"
                          [ "->" type_name ] ";" ;
```

类体常见成员：

- 字段：`var/let ... = ...;`
- 方法：`func ... { ... }`
- 类内实例别名声明：`using &alias;`

接口只参与编译期检查，不生成运行时对象。`class HashMap [Map] {}` 声明实现关系；接口用 `decl func` 列出签名，编译器会检查实现类的方法名、接收者形式、参数类型/引用形式和返回类型。

```zeus
interface Map {
    using &this;
    decl func get(key: string) -> string;
    decl func static version() -> int;
}

class HashMap [Map] {
    using &this;
    func get(key: string) -> string { return key; }
    func static version() -> int { return 1; }
}
```

### 5.1.1 接收者（receiver）的两种写法

`this` **不是关键字**，语言也不会凭空造一个接收者名字。运行时总是把接收者作为**第一个实参**压栈
（方法调用、`operator[.]`、`operator[.=]`、以及构造时的 `init` 都是这个顺序），所以形参按位置依次绑定。
接收者怎么被命名，只有两种合法写法：

写法一，类内声明别名，由编译器把别名**注入**成第一个形参：

```
class Point {
    using &me;                 // 注入点：me 成为第一个形参
    var x = 0;
    func get() { return me.x; }        // 直接用 me
    func operator[+](count) { ... }    // 运算符同样拿到注入的 me
}
```

写法二，不声明别名，方法**自己**把接收者写成第一个形参：

```
class Point {
    var x = 0;
    func operator[+](self, other) { return self.x + other.x; }
}
```

判定规则（普通方法与运算符一致，运算符没有例外）：

- 类声明了 `using &alias`：没有 `static` 修饰的方法都是实例方法，别名注入为第一个形参；
  此时再写 `[&alias]` 捕获或再次声明同名形参都是错误（接收者会被重复绑定并使实参错位）。
- 类未声明 `using &alias`：带显式捕获 `[&name]` 的方法是实例方法，别名为 `name`；
  其余方法按静态方法处理。
- 标记 `static` 的方法不接收接收者。

未声明别名也未捕获时，方法拿不到接收者。若此时在方法体里写 `this`，
编译期报 `Cannot find local variable 'this'`；运算符则会因为形参数量不足而在调用时报
`too many arguments`。

> 注意：`init` 只在实例方法表中查找，因此需要构造函数的类必须使用写法一。

### 5.1.2 为已有类型挂接方法

```ebnf
impl_block ::= "impl" type_path "{" [receiver_alias] { func_block | static_function } "}" ;
receiver_alias ::= "using" "&" identifier ";" ;
type_path ::= identifier { "." identifier } ;
```

`impl T` 使用与类相同的方法和运算符表，但不创建新类、不声明字段。写 `using &alias;` 后，未标记 `static` 的方法会以 `alias: T` 为接收者；没有声明接收者别名时，未标记 `static` 的方法按静态方法处理。`operator[new]` 始终是静态工厂。`impl` 可扩展原生类型和已有类；已有类会把这些方法并入它的类方法表。

`operator[new]` 是静态工厂运算符，不接收隐式 `this`。`new T(args...)` 会调用 `T` 上登记的 `operator[new]` 并把它的返回值作为表达式结果；未定义工厂时，普通类仍按字段分配和 `init(...)` 构造。`impl T` 可以为原生类型登记工厂，例如 `impl string`。

### 5.1.3 原位替换实例状态

```ebnf
this_assignment ::= "@this" "=" expr ;
```

`@this = value` 只允许在实例方法中使用，且 `value` 必须与当前接收者属于同一个类。它会把 `value` 的字段状态复制到当前实例，同时保留当前实例的身份和地址；已经指向当前实例的其他变量也会看到更新。VM 对动态类型在运行时检查，LLVM 对静态类型在编译时检查。

```zeus
class Test {
    using &this;
    var value = 0;

    func reset() {
        @this = new Test();
    }
}

let a: Test = new Test();
let b: Test = a;
a.reset();
print(b); // 仍是同一个 Test 实例，但字段已重置
```

### 5.2 对象创建

```ebnf
new_expr ::= "new" class_expr [ "(" [arg_list] ")" ] ;
class_expr ::= identifier { "." identifier } ;
```

当前实现支持 `new T(args...)`。若 `T` 定义 `operator[new]`，该工厂优先执行；否则类类型按字段分配，并在定义了 `init` 时调用它。原生类型通过 `impl T` 提供工厂。

内置 `String` 是原生 `string` 的别名，没有独立包装对象；`new string(value)` 和 `new String(value)` 都调用 `impl string` 中的工厂并返回原生字符串。

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
using_block ::= "using" "original" block
             | "using" "LLVM" block
             | "using" "VM" block ;
llvm_intrinsic ::= "@addr" "(" expr ")"
                 | "@reveal" "(" expr ")"
                 | "@faddr" "(" expr ")" "->" llvm_value_type
                 | "@faddr" "(" expr "," expr ")" "->" llvm_value_type ;
llvm_faddr_assignment ::= "@faddr" "(" expr ")" "->" llvm_value_type "=" expr
                          | "@faddr" "(" expr "," expr ")" "->" llvm_value_type "=" expr ;
this_assignment ::= "@this" "=" expr ;
llvm_value_type ::= identifier ;
```

另：类内 `using &alias;` 属于类语义，不是 `using original` 语义。

using LLVM 和 using VM 是按作用域顺序配对的目标块。AOT 只编译 LLVM 块，VM 只编译 VM 块，缺少配对时跳过并警告。

`@addr(value)` 只接受命名左值并返回 int 地址。AOT 下它指向变量存储槽；VM 下它返回关联到变量 Cell 的地址句柄。两者都用于读写变量槽，string/bytes 的原始数据地址由 `@reveal` 返回。对 shared 变量取址时地址关联到 SharedCell 的值槽，不持有互斥锁，调用方负责并发；共享值被替换或从共享堆删除后，旧地址可能失效。

AOT 原生 `string` 和动态 ZeusValue 中的 string 都使用 `[u64 字节长度][UTF-8 字节][NUL]` 缓冲区，地址指向第一个字符，长度位于 `data - 8`，长度按字节计且不含 NUL。动态 ZeusValue 槽保存数据指针并拥有这段分配；复制 ZeusValue 时深拷贝缓冲区。原生字符串池按内容去重，并在进程生命周期内保持地址稳定。

`@reveal(value)` 返回 string/bytes 的原始数据地址。VM 与 AOT 共用 `[u64 字节长度][data]` 布局，地址指向 data 首字节，长度可从 `@reveal(value) - 8` 读取；string 的 data 末尾另有 NUL，bytes 没有。`@faddr(address) -> T` 按目标内存布局读写；完整 string/bytes 按该布局读取。`@faddr(address, width) -> int` 读取或写入 1、2、4、8 字节，宽度必须是整数字面量，窄读取按无符号字节序扩展为 int，写入保留低位。`@faddr(address, length) -> string` 从原始数据地址复制指定字节数；length 可为运行时 int，不依赖 NUL 结尾，结果进入原生字符串池以保持地址稳定。

`@addr(string/bytes)` 指向变量槽位，`@reveal(value)` 才返回数据首地址；`@faddr(@addr(value)) -> string/bytes` 读写的是完整变量值，带长度的 `@faddr` 则用于显式原始字节范围。bytes 的长度头与数据一起分配，替换槽位会更新该槽持有的完整值。bigint、float、容器和对象的布局尚未定义为跨后端稳定 ABI。原始地址访问只在动态位于 `try` 区域内时（包括 try 中代码调用到的函数）检查已登记的槽位或存活内存范围：越界、类型不符或对只读存储的写入会抛出可捕获异常；`try` 区域之外跳过这些检查，按地址直接读写，错误地址即野指针行为。指针仍不携带类型，不能用它推断未登记内存的布局。

`inline func` 为函数请求 LLVM 内联；用 `zeusc --opt-level 2` 等选项启用 LLVM 优化，默认等级为 `0`。`@no_safepoint` 让 LLVM 后端不在该函数体的循环回边和返回点插入 GC 检查；函数调用到的运行时或其他函数仍可能执行自己的检查。长时间运行且产生垃圾的循环应自行安排回收时机，避免延迟回收。

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
arg_list      ::= [arg { "," arg } [","]] ;
arg           ::= [identifier "="] expr ;
default_expr  ::= expr ;
operator_symbol ::= <operator token or keyword token in operator[...]> ;
```

1. 后缀：调用 `()`, 成员 `.`, 下标 `[]`
2. 一元：`+x`, `-x`, `!x`
3. 乘除模：`* / %`
4. 加减：`+ -`
5. 比较：`== != > < >= <=`
6. 逻辑与：`and`
7. 逻辑或：`or`、`||`

## 10. 当前实现状态备注

- `continue` 已有 `NodeType` 枚举，但当前解析流程未接入关键字识别与 IR 生成。
- `while` 在当前解析器中未作为独立语法块接入（文档中的旧示例不代表当前可用）。
- 不支持匿名函数或函数表达式；`func` 定义必须提供函数名或 `operator[...]`。
- 部分旧文档提到 “构造函数参数不支持” 已过时；当前实现支持 `new Class(args...)`。
- 运算符重载语法文档中 `operator[[]= ]` 的“必须空格”说法并非实现强约束，实际以解析结果为准。
