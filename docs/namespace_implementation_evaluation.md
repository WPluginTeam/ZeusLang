# Zeus命名空间实现方案评估

## 问题总结

### 当前问题
在 `ZeusBuiltins/zeus/cppfunctions.zeu` 中，包内代码调用内置函数 `input()` 失败：

```zeu
class io {
    func static input() { return input(); }  // 错误：无法解析input
}
```

**错误信息**：
```
[Runtime Error E1000] Line 57: NameError: undefined variable 'ZeusBuiltins.cppfunctions.input'
```

### 根本原因

1. **编译时解析顺序**（IRGenerator.hpp 第760-815行）：
   ```cpp
   auto s = IRScope::resolve(name);  // 优先解析局部/全局变量
   if (s.kind != IRScope::Kind::UNDEFINED) {
       // 生成 LOAD_VALUE 指令
   } else {
       int bid = ZeusBuiltins::findId(name);  // 其次才查找内置函数
       if (bid >= 0) {
           // 生成 LOAD_NATIVE 指令
       }
   }
   ```

2. **包全局变量注册**（ZeusPackageLoader.hpp 第270行）：
   - 包编译时，`io.input()` 函数被注册为 `ZeusBuiltins.cppfunctions.input` 到 `IRScope::globals`
   - 包内代码的 `input()` 引用也会尝试解析为全局变量

3. **命名冲突**：
   - 包内的 `input()` 应该解析为内置函数（builtin）
   - 但由于同名的包函数 `ZeusBuiltins.cppfunctions.input` 存在于globals中
   - 解析器可能误匹配，导致循环引用

4. **内置函数系统独立**：
   - 内置函数通过 `registerBuiltin()` 注册到 `ZeusBuiltins::g_registry`
   - **不在** `IRScope::globals` 中
   - 解析优先级：局部变量 > 全局变量 > 内置函数

### 为什么以前可能工作？

可能的原因：
1. **解析顺序变化**：之前可能先查 builtin 再查 globals
2. **包前缀逻辑不同**：之前可能不为包内引用添加前缀
3. **全局变量处理**：之前 `io.input` 可能未注册到 globals

---

## 方案一：修复当前全局前缀系统

### 实现难度：⭐⭐ (简单)

### 核心思路
在包编译时，区分包内引用和包外引用，包内调用内置函数时不添加前缀。

### 需要修改的文件

#### 1. IRGenerator.hpp (_parsePrimary 函数，第760-815行)

**当前逻辑**：
```cpp
auto s = IRScope::resolve(name);  // 总是先查全局
if (s.kind != IRScope::Kind::UNDEFINED) {
    // 使用全局/局部变量
} else {
    int bid = ZeusBuiltins::findId(name);  // 再查builtin
}
```

**修改方案A：调整解析优先级**
```cpp
// 先检查是否是内置函数（如果在包编译上下文中）
if (ctx.isCompilingPackage) {
    int bid = ZeusBuiltins::findId(name);
    if (bid >= 0) {
        // 生成 LOAD_NATIVE 指令
        IRType::IRInstr ins;
        ins.op = VMOperator::LOAD_NATIVE;
        ins.args.push_back(LexerType::Value(bid));
        out.push_back(ins);
        calleeKind = CalleeKind::Native;
        goto _after_resolve;
    }
}

auto s = IRScope::resolve(name);
// ... 原有逻辑

_after_resolve:
// 继续后续处理
```

**修改方案B：包内名称不查全局**
```cpp
auto s = IRScope::resolve(name);

// 如果在包内且解析失败，不要匹配带前缀的全局名
if (ctx.isCompilingPackage && s.kind == IRScope::Kind::UNDEFINED) {
    // 清除可能的前缀匹配
    std::string prefixedName = ctx.packageName + "." + name;
    if (IRScope::globals.find(prefixedName) != IRScope::globals.end()) {
        // 忽略这个匹配，继续查builtin
        s.kind = IRScope::Kind::UNDEFINED;
    }
}

if (s.kind != IRScope::Kind::UNDEFINED) {
    // 使用解析到的变量
} else {
    int bid = ZeusBuiltins::findId(name);
    // ...
}
```

#### 2. IRScope.hpp (resolve 函数，第153-265行)

添加包上下文感知：

```cpp
inline SymRef resolve(const std::string& name, bool isInPackage = false, const std::string& packageName = "") {
    // ... 原有局部/upvalue解析逻辑 ...
    
    _resolve_globals:
    // 如果在包内编译，不要解析带自身前缀的全局名
    if (isInPackage && !packageName.empty()) {
        std::string selfPrefixed = packageName + "." + name;
        auto g = globals.find(name);  // 只查不带前缀的
        if (g != globals.end()) return SymRef{ Kind::GLOBAL, g->second };
        // 不查 selfPrefixed
    } else {
        auto g = globals.find(name);
        if (g != globals.end()) return SymRef{ Kind::GLOBAL, g->second };
    }
    
    // ... 其余逻辑 ...
}
```

#### 3. IRGenerator.hpp (ParseContext 结构)

添加包上下文字段（约第100-150行）：

```cpp
struct ParseContext {
    // ... 现有字段 ...
    bool isCompilingPackage = false;  // 新增
    std::string packageName = "";     // 新增
    // ...
};
```

#### 4. ZeusPackageLoader.hpp (compilePackage 函数，第43行开始)

设置包编译标志：

```cpp
inline CompiledPackage compilePackage(const std::string& packageName, const std::string& sourceFilePath, bool strictDecl = false) {
    // ... 初始化代码 ...
    
    auto irgen = IRGen::IRGenerator();
    irgen.strictDecl = strictDecl;
    
    // 新增：设置包编译上下文
    // 需要在 IRGenerator 类中添加这些字段
    irgen.currentPackageName = packageName;
    irgen.isCompilingPackage = true;
    
    irgen.parse(trees);
    // ...
}
```

### 优点
- 改动最小，不破坏现有架构
- 不影响非包代码的编译
- 保持向后兼容

### 缺点
- 增加了编译逻辑复杂度
- 包内外解析规则不一致
- 仍依赖全局前缀系统

### 工作量估计
- 代码修改：4个文件，约100行代码
- 测试工作：验证包内外调用、嵌套包、循环引用
- 预计时间：2-3小时

---

## 方案二：真命名空间对象

### 实现难度：⭐⭐⭐⭐ (困难)

### 核心思路
将包作为运行时对象，包含所有导出成员作为属性。不再使用全局前缀。

### 数据结构设计

#### 1. 新增 PackageObject 类型（ZeusType.hpp）

```cpp
struct PackageObject {
    std::string packageName;
    std::unordered_map<std::string, ZeusType::Value> exports;  // 导出的函数、类、变量
};
```

#### 2. Value 类型扩展

```cpp
using Value = std::variant<
    ValueType::None,
    ValueType::Bool,
    ValueType::Int,
    ValueType::Float,
    ValueType::String,
    ValueType::Closure,
    ValueType::NativeHandle,
    ValueType::UserClassObject,
    ValueType::BigInteger,
    ValueType::BigDecimal,
    std::shared_ptr<PackageObject>  // 新增
>;
```

### 需要修改的内容

#### 1. 包加载系统（ZeusPackageLoader.hpp）

**当前逻辑**（第43-150行）：
```cpp
// 包编译后，将全局变量展开到主程序globals
for (size_t i = 0; i < pkg->globalNames.size(); ++i) {
    std::string prefixedName = pkg->packageName + "." + pkg->globalNames[i];
    newGlobalNames[baseIndex + i] = prefixedName;
}
```

**新逻辑**：
```cpp
// 包编译后，创建PackageObject
auto pkgObj = std::make_shared<PackageObject>();
pkgObj->packageName = packageName;

// 将包的导出内容（类、函数）存入pkgObj->exports
for (const auto& exportName : pkg->exports) {
    // 需要从VM运行时获取对应的闭包/类对象
    pkgObj->exports[exportName] = /* 提取导出值 */;
}

// 将包对象注册为单个全局变量
int pkgGlobalIndex = IRScope::declareGlobal(packageName);
globals[pkgGlobalIndex] = ZeusType::Value(pkgObj);
```

#### 2. 属性访问指令（VMOperator）

新增指令：
```cpp
enum class VMOperator {
    // ... 现有指令 ...
    PACKAGE_LOAD_ATTR,  // 从包对象加载属性
    PACKAGE_STORE_ATTR, // 向包对象存储属性（用于运行时修改）
};
```

#### 3. IR生成器（IRGenerator.hpp）

**处理包成员访问**（约第1300-1500行，属性链解析部分）：

```cpp
// 当前：cppfunctions.io.input 解析为 globals["ZeusBuiltins.cppfunctions.io.input"]
// 新逻辑：
// 1. 解析 "cppfunctions" -> LOAD_VALUE global <pkgIndex>
// 2. 解析 ".io" -> PACKAGE_LOAD_ATTR "io"
// 3. 解析 ".input" -> OBJ_GET_ATTR "input"（如果io是类）
//                  或 PACKAGE_LOAD_ATTR "input"（如果io也是子包）
```

需要在属性链解析时识别包对象类型。

#### 4. VM执行器（ZeusVM.hpp）

**实现 PACKAGE_LOAD_ATTR**（新增，约在第3500行之后）：

```cpp
case VMOperator::PACKAGE_LOAD_ATTR: {
    if (instr.args.size() < 1) throw VMException::VMException("PACKAGE_LOAD_ATTR missing attr name", __line, instr);
    
    auto attrName = std::get<std::string>(instr.args[0]);
    ZeusType::Value& objVal = stack[sp - 1];
    
    if (!std::holds_alternative<std::shared_ptr<PackageObject>>(objVal)) {
        throw VMException::VMException("PACKAGE_LOAD_ATTR: not a package object", __line, instr);
    }
    
    auto& pkg = std::get<std::shared_ptr<PackageObject>>(objVal);
    auto it = pkg->exports.find(attrName);
    if (it == pkg->exports.end()) {
        throw VMException::VMException("Package '" + pkg->packageName + "' has no attribute '" + attrName + "'", __line, instr);
    }
    
    stack[sp - 1] = it->second;  // 替换栈顶为属性值
    break;
}
```

#### 5. Import语句处理（IRImport.hpp）

**当前逻辑**（约第50-200行）：
```cpp
// import ZeusBuiltins.cppfunctions [&]
// 生成：LOAD_VALUE global <index_of_ZeusBuiltins.cppfunctions.func1>
//       STORE_VALUE local <local_func1>
//       ...（每个导出成员一条）
```

**新逻辑**：
```cpp
// import ZeusBuiltins.cppfunctions [&]
// 生成：LOAD_VALUE global <index_of_ZeusBuiltins>  // 加载包对象
//       PACKAGE_LOAD_ATTR "cppfunctions"           // 获取子包
//       STORE_VALUE local <local_ref_to_cppfunctions>
// 
// 然后对于 cppfunctions.io.input() 调用：
//       LOAD_VALUE local <local_ref_to_cppfunctions>
//       PACKAGE_LOAD_ATTR "io"    // 获取io类
//       OBJ_GET_ATTR "input"      // 获取input方法
//       CALL_CLOSURE 0
```

需要重写整个import逻辑，包括：
- 解析 `[&]` 展开语法
- 处理嵌套包路径
- 区分包对象和类对象

### 优点
- 真正的命名空间隔离，无全局命名污染
- 运行时可动态修改包内容
- 符合现代语言设计（Python、JavaScript）
- 包可以像对象一样传递

### 缺点
- **工作量极大**，几乎重写包系统
- 性能开销：属性查找需要哈希表访问
- 破坏性改动：所有现有包需要重新测试
- import语句语义变化，可能影响用户代码

### 关键技术难点

1. **导出值提取**：包编译完成时，如何从IR/字节码中提取闭包和类对象？
   - 需要在编译后立即运行包的初始化代码
   - 或者延迟求值（lazy evaluation）

2. **循环依赖**：A包依赖B包，B包依赖A包
   - 需要分阶段加载机制
   - 或禁止循环依赖

3. **静态成员访问**：`cppfunctions.io.input()` 中 `io` 是类，不是包
   - 需要统一属性访问语义
   - 或区分 PACKAGE_LOAD_ATTR 和 CLASS_GET_ATTR

### 工作量估计
- 代码修改：10+个文件，1000+行代码
- 新增测试：包对象序列化、属性修改、循环依赖
- 重构现有包系统
- 预计时间：3-5天

---

## 方案三：原生类集成（Native Class）

### 实现难度：⭐⭐⭐ (中等)

### 核心思路
将包及其成员编译为C++类，通过 `CALL_NATIVE_CLASS` 指令直接调用，绕过命名空间系统。

### 实现概述

#### 1. 包到C++类的映射

**ZeusBuiltins/zeus/cppfunctions.zeu**：
```zeu
class io {
    func static input() { return input(); }
}
```

**自动生成C++代码**（新工具：ZeusPkgCompiler）：
```cpp
// Generated from ZeusBuiltins/zeus/cppfunctions.zeu

namespace ZeusNative {
    class cppfunctions {
    public:
        class io {
        public:
            static ZeusType::Value input(VM& vm) {
                // 调用内置input
                return ZeusBuiltins::call(vm, "input", {});
            }
        };
    };
}

// 注册到VM
void registerCppfunctionsPackage(VM& vm) {
    vm.registerNativeClass("ZeusBuiltins.cppfunctions.io", 
        ZeusNative::cppfunctions::io::getMethodTable());
}
```

#### 2. IR生成修改

**IRGenerator.hpp**（属性链解析，约第1400行）：

```cpp
// 遇到 cppfunctions.io.input() 时：
// 检查 "ZeusBuiltins.cppfunctions.io" 是否注册为原生类
if (NativeClassRegistry::has("ZeusBuiltins.cppfunctions.io")) {
    // 生成指令：
    IRType::IRInstr call;
    call.op = VMOperator::CALL_NATIVE_CLASS_METHOD;
    call.args.push_back("ZeusBuiltins.cppfunctions.io");  // 类名
    call.args.push_back("input");                         // 方法名
    call.args.push_back(Value(0));                        // argc
    out.push_back(call);
} else {
    // 正常的属性链解析
}
```

#### 3. VM执行器新增指令

**ZeusVM.hpp**（新增case，约第3600行）：

```cpp
case VMOperator::CALL_NATIVE_CLASS_METHOD: {
    std::string className = std::get<std::string>(instr.args[0]);
    std::string methodName = std::get<std::string>(instr.args[1]);
    int argc = std::get<int>(instr.args[2]);
    
    auto* nativeClass = NativeClassRegistry::get(className);
    if (!nativeClass) throw VMException::VMException("Native class not found: " + className, __line, instr);
    
    auto* method = nativeClass->getMethod(methodName);
    if (!method) throw VMException::VMException("Method not found: " + className + "." + methodName, __line, instr);
    
    // 调用原生方法
    ZeusType::Value result = method->call(*this, stack + sp - argc, argc);
    sp -= argc;
    stack[sp++] = result;
    break;
}
```

#### 4. 需要新增的文件

**ZeusRunner/NativeClassRegistry.hpp**（新文件，约300行）：

```cpp
#pragma once
#include <unordered_map>
#include <functional>
#include "ZeusType.hpp"

namespace ZeusNative {
    class VM;  // 前向声明
    
    struct NativeMethod {
        std::string name;
        std::function<ZeusType::Value(VM&, ZeusType::Value*, int)> callback;
        int minArity, maxArity;
    };
    
    struct NativeClass {
        std::string name;
        std::unordered_map<std::string, NativeMethod> methods;
        
        const NativeMethod* getMethod(const std::string& name) const;
    };
    
    class NativeClassRegistry {
        static std::unordered_map<std::string, NativeClass> classes;
    public:
        static void registerClass(const std::string& name, NativeClass cls);
        static const NativeClass* get(const std::string& name);
        static bool has(const std::string& name);
    };
}
```

**tools/ZeusPkgCompiler**（新工具，Python或C++）：
- 解析 .zeu 包文件
- 生成对应的C++类和注册代码
- 自动处理嵌套类、静态方法

#### 5. 包加载流程

**修改 ZeusPackageLoader.hpp**（约第43行）：

```cpp
inline CompiledPackage compilePackage(const std::string& packageName, const std::string& sourceFilePath, bool strictDecl = false) {
    // 检查是否有对应的原生实现
    std::string nativeImplPath = "ZeusNative/" + packageName + ".hpp";
    if (fileExists(nativeImplPath)) {
        // 使用原生实现，不编译.zeu源文件
        return CompiledPackage::fromNative(packageName);
    }
    
    // 否则使用原有的编译流程
    // ...
}
```

### 优点
- **性能最优**：直接C++函数调用，无解释器开销
- 包内实现不受命名空间系统影响
- 可以访问任何C++ API（文件、网络、线程等）
- 适合核心库和性能敏感代码

### 缺点
- **混合语言开发**：需要维护.zeu和C++两套代码
- 需要额外的编译工具链（ZeusPkgCompiler）
- 调试困难：C++和Zeus代码栈交织
- 修改包需要重新编译C++代码

### 适用场景
- 标准库包（ZeusBuiltins.cppfunctions, ZeusBuiltins.math等）
- 性能关键的包
- 需要使用系统API的包

### 工作量估计
- 新增文件：2个（NativeClassRegistry.hpp, ZeusPkgCompiler工具）
- 修改文件：3个（IRGenerator.hpp, ZeusVM.hpp, ZeusPackageLoader.hpp）
- 代码量：约500行C++ + 300行Python（工具）
- 预计时间：2-3天

---

## 方案四：作用域链解析

### 实现难度：⭐⭐⭐⭐⭐ (非常困难)

### 核心思路
引入作用域链（Scope Chain）概念，类似JavaScript的作用域查找：  
`局部作用域 → 函数作用域 → 包作用域 → 内置作用域 → 全局作用域`

### 架构设计

#### 1. 新的作用域层级

**IRScope.hpp 重构**（几乎重写，约500行改动）：

```cpp
class Scope {
public:
    enum class Type { LOCAL, FUNCTION, PACKAGE, BUILTIN, GLOBAL };
    
    Type type;
    std::string name;  // 作用域名称（如包名）
    std::unordered_map<std::string, int> symbols;
    Scope* parent;  // 父作用域
    
    SymRef resolve(const std::string& name);
};

class ScopeChain {
    std::vector<Scope*> chain;
public:
    void pushScope(Scope::Type type, const std::string& name = "");
    void popScope();
    SymRef resolve(const std::string& name);  // 沿链查找
};
```

#### 2. 包作用域管理

**ZeusPackageLoader.hpp**（约第43-150行大幅重构）：

```cpp
inline CompiledPackage compilePackage(...) {
    // 创建包作用域
    auto packageScope = new Scope(Scope::Type::PACKAGE, packageName);
    
    // 设置父作用域为内置作用域
    packageScope->parent = BuiltinScope::getInstance();
    
    // 注册包内可见的内置函数
    for (const auto& [name, id] : ZeusBuiltins::g_nameToId) {
        packageScope->parent->symbols[name] = id;
    }
    
    // 编译时使用包作用域
    IRGenerator irgen;
    irgen.scopeChain.pushScope(Scope::Type::PACKAGE, packageName);
    irgen.parse(trees);
    irgen.scopeChain.popScope();
    
    // ...
}
```

#### 3. 解析逻辑变更

**IRScope.hpp (resolve函数，第153行起)**：

```cpp
SymRef ScopeChain::resolve(const std::string& name) {
    // 从当前作用域开始向上查找
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Scope* scope = *it;
        
        // 在当前作用域查找
        auto found = scope->symbols.find(name);
        if (found != scope->symbols.end()) {
            return SymRef::fromScope(scope, name, found->second);
        }
        
        // 如果是包作用域，检查是否是内部成员
        if (scope->type == Scope::Type::PACKAGE) {
            std::string prefixed = scope->name + "." + name;
            auto prefixFound = scope->symbols.find(prefixed);
            if (prefixFound != scope->symbols.end()) {
                // 找到了包成员，但不使用前缀（内部访问）
                return SymRef::fromScope(scope, name, prefixFound->second);
            }
        }
        
        // 如果当前作用域是函数，继续向上（允许闭包）
        // 如果当前作用域是包，跳到内置作用域（不允许包间直接访问）
        if (scope->type == Scope::Type::PACKAGE && scope->parent) {
            // 跳过其他包，直接查内置
            return resolveInScope(scope->parent, name);
        }
    }
    
    return SymRef{ Kind::UNDEFINED, -1 };
}
```

#### 4. Import语句重构

**IRImport.hpp**（第50-200行完全重写）：

```cpp
// import ZeusBuiltins.cppfunctions [&]

// 当前逻辑：展开为 LOAD_VALUE + STORE_VALUE（每个导出成员）
// 新逻辑：
// 1. 将 ZeusBuiltins.cppfunctions 的导出加入当前作用域符号表
// 2. 不生成任何IR指令（纯编译期操作）

void handleImport(...) {
    std::string packagePath = parsePackagePath(tokens);  // "ZeusBuiltins.cppfunctions"
    
    // 加载包的导出符号表
    auto* pkg = PackageRegistry::get(packagePath);
    if (!pkg) throw IRException("Package not found: " + packagePath);
    
    // 将导出符号添加到当前作用域
    for (const auto& [exportName, symInfo] : pkg->exports) {
        currentScope->symbols[exportName] = symInfo;
    }
    
    // [&] 展开语法：递归添加所有嵌套导出
    if (hasExpandSyntax) {
        for (const auto& nested : pkg->nestedExports) {
            currentScope->symbols[nested.first] = nested.second;
        }
    }
}
```

#### 5. 运行时支持

**ZeusVM.hpp**（约第100-200行，栈帧结构修改）：

```cpp
struct CallFrame {
    // ... 现有字段 ...
    Scope* scope;  // 新增：当前栈帧的作用域
};

// 函数调用时传递作用域
case VMOperator::CALL_CLOSURE: {
    // ...
    CallFrame frame;
    frame.scope = currentScope;  // 捕获调用时的作用域
    callStack.push(frame);
    // ...
}
```

### 优点
- **最符合语言设计理念**：清晰的作用域层级
- 内置函数自然优先于包成员（内置作用域在包上层）
- 无全局命名污染
- 支持嵌套包和闭包

### 缺点
- **工作量最大**：几乎重写整个作用域系统
- 性能开销：每次查找需遍历作用域链
- 复杂度极高：闭包、包、类三种作用域交织
- **风险最高**：可能引入大量隐藏bug

### 关键技术挑战

1. **闭包捕获与包作用域冲突**：
   ```zeu
   package mypack {
       let x = 10;
       func outer() {
           func inner() {
               return x + input();  // x来自包作用域，input来自内置
           }
           return inner;
       }
   }
   ```
   需要正确处理多层作用域的捕获。

2. **循环引用检测**：A包引用B包，B包引用A包
   - 需要在编译期检测
   - 或使用懒加载+引用计数

3. **性能优化**：作用域链查找是热点路径
   - 需要缓存机制（Inline Cache）
   - 或编译期静态解析优化

### 工作量估计
- 重构文件：IRScope.hpp, IRGenerator.hpp, ZeusPackageLoader.hpp, IRImport.hpp, ZeusVM.hpp
- 新增文件：ScopeChain.hpp, BuiltinScope.hpp
- 代码量：2000+行
- 测试工作：闭包+包+类的各种组合
- 预计时间：1-2周

---

## 推荐方案

### 短期方案（立即修复）：**方案一**
**原因**：
- 工作量最小（2-3小时）
- 风险最低，不影响现有功能
- 能立即解决 `cppfunctions.io.input()` 的问题

**实施步骤**：
1. 在 `IRGenerator.hpp` 的 `ParseContext` 中添加 `isCompilingPackage` 和 `packageName` 字段
2. 修改 `_parsePrimary` 函数，在包编译时优先查找内置函数
3. 在 `ZeusPackageLoader.hpp` 的 `compilePackage` 中设置上下文标志
4. 测试验证

### 长期方案（重构）：**方案三（部分）+ 方案一**
**原因**：
- 对于核心库（ZeusBuiltins.*），使用原生类实现（方案三）
  - 性能最优
  - 避免命名空间问题
  - 便于维护和扩展
- 对于用户包，保持当前系统 + 方案一的修复
  - 保持灵活性
  - 不增加用户负担

**实施路线图**：
1. **Phase 1（1周）**：实现方案一，确保当前系统可用
2. **Phase 2（2周）**：实现方案三的基础设施（NativeClassRegistry）
3. **Phase 3（1周）**：将 `ZeusBuiltins.cppfunctions` 迁移到原生实现
4. **Phase 4（可选）**：逐步迁移其他核心包

### 不推荐的方案
- **方案二**（真命名空间对象）：工作量与收益不成比例
- **方案四**（作用域链）：复杂度太高，风险太大，收益不明显

---

## 历史问题分析："为什么以前可以工作？"

基于代码审查，有以下可能：

### 可能性1：解析顺序曾经不同
如果之前的 `_parsePrimary` 是先检查 `ZeusBuiltins::findId(name)` 再检查 `IRScope::resolve(name)`，那么内置函数会优先匹配。

**验证方法**：
```bash
git log -p --all -S "ZeusBuiltins::findId" -- ZeusIR/IRGenerator.hpp
```
查看是否有顺序调整的提交记录。

### 可能性2：io.input() 从未真正被调用过
查看 `ZeusBuiltins/zeus/cppfunctions.zeu` 的提交历史：
```bash
git log -p -- ZeusBuiltins/zeus/cppfunctions.zeu
```
可能 `io.input()` 方法是最近才添加的，之前都直接使用全局 `input()`。

### 可能性3：包编译逻辑变化
早期版本的 `compilePackage` 可能不会将包内函数注册到 `IRScope::globals`，而是用其他机制（如独立的符号表）。

**验证方法**：
```bash
git log -p --all -- ZeusPackage/ZeusPackageLoader.hpp | grep "globals\["
```
查找 globals 注册逻辑的变更。

### 可能性4：从未工作过
如果测试覆盖不足，这个bug可能一直存在但未被发现。只是最近才有用户尝试 `cppfunctions.io.input()` 的用法。

**建议**：添加回归测试确保修复后不再出现。

---

## 测试计划

无论选择哪个方案，都需要以下测试：

### 1. 单元测试
```zeu
// test_package_builtin_call.zeu

import ZeusBuiltins.cppfunctions [&];

// 测试1：直接调用内置函数
let result1 = input();  // 应该工作

// 测试2：通过包命名空间调用
let result2 = cppfunctions.io.input();  // 应该工作

// 测试3：嵌套类静态方法
class A {
    func static test() {
        return input();  // 应该工作
    }
}
print(A.test());

// 测试4：闭包中调用内置
func outer() {
    func inner() {
        return input();  // 应该工作
    }
    return inner;
}
let f = outer();
print(f());
```

### 2. 回归测试
- 所有现有的包导入测试
- 静态方法调用测试（test_static.zeu）
- 链式调用测试（test_chain_static.zeu）

### 3. 性能测试
```zeu
// benchmark_builtin_call.zeu

let start = time.now();
for i in range(100000) {
    let _ = input();  // 或 io.input()
}
let end = time.now();
print("Time: " + str(end - start) + "ms");
```

确保修复后的性能不低于修复前。

---

## 附录：相关代码位置速查

| 功能 | 文件 | 行号范围 | 说明 |
|------|------|----------|------|
| 标识符解析 | IRGenerator.hpp | 760-815 | _parsePrimary函数，决定LOAD_VALUE还是LOAD_NATIVE |
| 作用域查找 | IRScope.hpp | 153-265 | resolve函数，按LOCAL→GLOBAL→UPVALUE顺序 |
| 内置函数注册 | ZeusBuiltins.hpp | 49-67, 770-810 | registerBuiltin和所有内置函数的注册 |
| 包编译入口 | ZeusPackageLoader.hpp | 43-150 | compilePackage函数 |
| 包全局变量前缀 | ZeusPackageLoader.hpp | 270 | 合并时添加packageName前缀 |
| Import语句处理 | IRImport.hpp | 50-200 | 展开[&]语法，生成LOAD+STORE指令 |
| LOAD_VALUE执行 | ZeusVM.hpp | 3775-3817 | 加载局部/全局/upvalue变量 |
| CALL_NATIVE执行 | ZeusVM.hpp | 3512+ | 调用内置函数 |

---

## 总结

| 方案 | 难度 | 时间 | 适用场景 | 推荐度 |
|------|------|------|----------|--------|
| 方案一：修复全局前缀 | ⭐⭐ | 2-3小时 | 快速修复当前问题 | ⭐⭐⭐⭐⭐ |
| 方案二：真命名空间 | ⭐⭐⭐⭐ | 3-5天 | 完全重构包系统（未来考虑） | ⭐⭐ |
| 方案三：原生类 | ⭐⭐⭐ | 2-3天 | 核心库性能优化 | ⭐⭐⭐⭐ |
| 方案四：作用域链 | ⭐⭐⭐⭐⭐ | 1-2周 | 语言级重构（不推荐） | ⭐ |

**立即行动建议**：实施方案一，修复 `cppfunctions.io.input()` 问题。
