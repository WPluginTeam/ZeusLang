# Zeus 包系统实现文档

## 概述

Zeus 语言现在支持基于文件的模块/包系统,允许代码分割到多个文件中并通过 `import` 语句引入。

## 语法

```zeus
// 导入包
import package.math [&math];

// 直接使用导入的函数(无前缀)
var result = add(3, 5);
var product = multiply(4, 7);
var squared = square(6);
```

### 模块导出名单

模块可在顶层声明 `using {名称, ...};`。名单中的顶层变量、函数和 class 可通过全局导入或命名空间导入访问；未列出的名称仍可在模块内部使用。没有导出名单的模块继续导出全部全局名称。

```zeus
let version = "1.0";
func add(a, b) { return a + b; }
class Point {}

using {version, add, Point};
```

## 文件结构

```
project/
├── main.zeu              # 主程序
└── package/              # 包目录
    ├── math.zeu          # 数学包
    └── string.zeu        # 字符串包
```

## 模块文件寻址

点号分隔的模块名对应目录层级，文件后缀为 `.zeu`。编译器内置模块的根目录固定为编译器可执行文件所在目录下的 `ZeusBuiltins/`：

```text
import zeus.mem;          -> <compiler-dir>/ZeusBuiltins/zeus/mem.zeu
import zeus.net.http;     -> <compiler-dir>/ZeusBuiltins/zeus/net/http.zeu
import ZeusBuiltins.mem;  -> 旧名，规范化为 zeus.mem
```

所有内置 `.zeu` 模块都放在 `ZeusBuiltins/zeus/` 下。`zeus.*` 是内置模块命名空间，未找到时会直接报模块不存在，不会改从项目目录加载同名文件。旧的 `ZeusBuiltins.*` 名称会规范化为对应的 `zeus.*` 名称，并加载同一个模块。其他项目模块先相对于导入它的源文件目录解析，再相对于进程当前目录解析。

例如，项目文件仍可这样组织：

```text
project/
├── main.zeu
└── package/
    └── math.zeu
```

```zeus
import package.math;
```

## 实现架构

### 1. 四阶段编译流程

#### 阶段 1: 预扫描导入语句
- 快速词法分析源文件
- 提取所有 `import` 语句
- 收集需要加载的包名列表

#### 阶段 2: 加载包并注册全局变量
- 编译每个包(独立的 IR 生成)
- 提取包的全局变量列表
- 注册到 IRScope 的全局符号表
- 记录每个包的全局变量起始索引

#### 阶段 3: 编译主程序
- 重新注册所有包的全局变量
- 编译主程序(能识别包中的函数)
- 主程序的全局变量从包全局变量后开始

#### 阶段 4: 合并包字节码
- 替换 IMPORT 指令为包的字节码块
- 调整全局变量索引
- 调整 MAKE_FUNCTION 的块索引

### 2. 核心组件

#### ZeusPackageLoader.hpp
- `compilePackage()`: 编译单个包文件
  - 清空 IRScope 避免包间污染
  - 独立编译每个包
  - 提取全局变量列表

- `loadPackage()`: 加载包(带缓存)
  - 解析包路径
  - 检查缓存
  - 调用编译

- `mergePackageChunks()`: 合并字节码
  - 替换 IMPORT 指令
  - 调整全局变量索引 (+globalStartIndex)
  - 调整 MAKE_FUNCTION 块索引 (+chunkOffset)

#### IRImport.hpp
- `resolvePackagePath()`: 解析包路径
  - 将模块点号转换为目录分隔符，并返回绝对路径
  - `zeus.*` 从编译器目录下的 `ZeusBuiltins/zeus/` 解析
  - `ZeusBuiltins.*` 作为兼容名称规范化为 `zeus.*`
  - 项目模块相对于导入源文件目录和进程当前目录解析

#### Zeus.cpp
- 实现四阶段编译流程
- 管理包加载和全局变量注册
- 维护 `packageGlobalOffsets` 映射

### 3. 关键技术点

#### 全局变量索引管理
```
package.math:  Global[0-2]   (add, multiply, square)
package.string: Global[3-6]   (concat, repeat, result, i)
main program:  Global[7+]    (sum, text, repeated, ...)
```

#### 块索引调整
当包被插入时,块索引需要调整:
```
包编译时:    MAKE_FUNCTION 0 (引用包的 Chunk #0)
合并后:      MAKE_FUNCTION N (引用主程序的 Chunk #N)
```

#### IRScope 清理
每次编译包前清空 IRScope,避免全局变量污染:
```cpp
IRScope::globals.clear();
IRScope::funcStack.clear();
IRScope::scopeStack.clear();
IRScope::refSymbols.clear();
```

## 使用示例

### 示例 1: 单包导入

**package/math.zeu:**
```zeus
func add(a, b) {
    return a + b;
}

func multiply(a, b) {
    return a * b;
}

func square(x) {
    return x * x;
}
```

**main.zeu:**
```zeus
import package.math [&math];

var result = add(3, 5);
print("Result: ", result);  // 输出: Result: 8
```

### 示例 2: 多包导入

**package/string.zeu:**
```zeus
func concat(a, b) {
    return a + b;
}

func repeat(str, times) {
    var result = "";
    var i = 0;
    while (i < times) {
        result = result + str;
        i = i + 1;
    }
    return result;
}
```

**main.zeu:**
```zeus
import package.math [&math];
import package.string [&string];

var sum = add(10, 20);
print("Sum: ", sum);  // 输出: Sum: 30

var text = concat("Hello", " World");
print("Text: ", text);  // 输出: Text: Hello World

var stars = repeat("*", 5);
print("Stars: ", stars);  // 输出: Stars: *****
```

## 字节码结构

### 主程序字节码结构
```
Chunk #0:  主程序的初始化代码
Chunk #1:  package.math 的 add 函数体
Chunk #2:  package.math 创建 add 函数
Chunk #3:  package.math 的 multiply 函数体
Chunk #4:  package.math 创建 multiply 函数
Chunk #5:  package.math 的 square 函数体
Chunk #6:  package.math 创建 square 函数
Chunk #7:  package.string 的 concat 函数体
Chunk #8:  package.string 创建 concat 函数
Chunk #9:  package.string 的 repeat 函数体
Chunk #10: package.string 创建 repeat 函数
Chunk #11: 主程序调用代码
...
```

## 名字可见性与绑定

一个编译单元能看到的名字，是它**依赖闭包**里的导出名：直接 `import` 的模块，以及
这些模块自己 `import` 的模块，只要名字被导出（没有 `using {…}` 名单，或列在名单
里），都可以用裸名字访问。

```zeus
// helper.zeu
let helperValue = 11;
func describeTag(value: string) -> string { return "helper:" + value; }

// bridge.zeu
import helper;
func bridgeTag(value: string) -> string { return describeTag(value); }

// main.zeu
import bridge;                    // 只 import bridge，没有 import helper
print(bridgeTag("a"));            // helper:a
print(describeTag("b"));          // helper:b —— helper 在 bridge 的依赖闭包里
print(helperValue);               // 11
```

三条规则：

1. **可见范围 = 依赖闭包**。解释器和 AOT 读同一份名字表：AOT 会为闭包里每个模块
   登记导出名及其最终槽位与限定函数名，因此不会出现“解释器能跑、AOT 报
   `indirect call target ... classref`”。
2. **声明优先**。模块里的 `let` / `func` 会遮蔽依赖闭包里的同名符号：声明拿到自己
   的新槽位，旧符号的槽位保持原值（不会被写坏），旧名字退到
   `<name>.__previous__`，此后裸名 `<name>` 指新声明。旧符号仍可用命名空间导入
   访问：

   ```zeus
   import zeus.string [&strmod];
   let str = "shadowed";          // 裸名 str 现在指这个变量
   print(strmod.str(41));         // 41 —— 库函数还在自己的槽位里
   ```
3. **跨模块调用携带限定名**。调用被标注为 `pkg.func`（例如 `zeus.string.str`），
   合并后的槽位号只用来取值，不再用来决定“调用谁”。

这三条把旧文档里“可能存在命名冲突”变成了确定结果，不再取决于包的加载顺序。

## 限制和未来改进

### 当前限制
1. 不支持包内函数的命名空间访问(如 `math.add()`)
2. 包中的所有函数都导入到全局作用域
3. 全局名字是扁平的：同一名字被多次声明时，最后一次声明接管裸名，早先的符号只能
   用限定名访问；模块级变量与库函数同名时尤其要注意
4. 循环导入未处理

### 未来改进方向
1. 支持命名空间访问语法
2. 选择性导入(只导入需要的函数)
3. 检测和报告循环导入
4. 包版本管理
5. 标准库包

## 调试

使用 `--debug-channels=ALL` 查看详细的编译过程:

```bash
Zeus.exe --debug-channels=ALL main.zeu
```

关键调试输出:
- `[Main] Pre-scanned import: package.math` - 发现导入
- `[PackageLoader] Package compiled: package.math (6 chunks, 3 globals)` - 包编译完成
- `[Main] Pre-registered global[0]: add` - 全局变量注册
- `[PackageLoader] Adjusted MAKE_FUNCTION chunk index: 0 -> 1` - 块索引调整
- `[PackageLoader] Adjusted STORE index: 0 -> 0` - 全局索引调整

## 总结

Zeus 的包系统通过四阶段编译实现了模块化:
1. 预扫描找到所有导入
2. 编译包并注册全局变量
3. 编译主程序(能识别包函数)
4. 合并字节码并调整索引

这种设计使得 Zeus 能够支持多文件项目,提高代码组织性和可维护性。
