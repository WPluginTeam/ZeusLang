# ZeusAOT：AOT 工具链验证环（Emitter 动工前的 Phase 0.5）

目标：证明 `Zeus IR → .ll → .obj → .exe` 整条路在 MSVC + LLVM exe 模式下走得通，
先不写真正的 `--emit-llvm`，`hello.ll` 为手写的“Emitter 目标输出”。

## 前置

- MSVC：`F:\Code\Env\VisualStudio2022`（`build_aot.bat` 会自己调 `vcvars64.bat`）
- LLVM 工具：`winget install LLVM.LLVM`（只要 `clang` 驱动，版本 15+ 均可，
  `ptr` 为不透明指针语法；注：新版 winget 包不再带独立 `llc`，`clang -c .ll`
  走同一套 codegen，等价）

## 跑

```
ZeusAOT\build_aot.bat
```

期望输出 `hello`，exit code 0。

## 文件

- `runtime/zeus_rt.h`：demo 入口头（含正式 ABI 契约 + `zeus_entry` 声明）
- `runtime/zeus_rt.cpp`：`main` 常驻，进 `libzeus_rt.lib`（`/MT`，一次编译到处链接）
- `../ZeusRuntimeLib/ZeusABI.hpp`：C ↔ IR 的 ABI 边界（全部 `extern "C"`）
- `../ZeusRuntimeLib/ZeusABI.cpp`：ABI 第一批实现（`ZeusBox` 装箱、`zeus_int_add`、
  `zeus_print`、极简全局量槽；对象/句柄随 Phase B 的 Context 到来）
- `../ZeusRuntimeLib/ZeusDynamicABI.hpp/.cpp`：独立的 array/dict 构造、读写、keys
  和迭代器 ABI；不经过 `zeus_push/zeus_pop`
- `../ZeusRuntimeLib/ZeusDynamicBinaryABI.hpp/.cpp`：boxed 二元运算 ABI；内建数值运算
  和用户对象运算符都从这里进入，后者通过显式 `ZeusRuntimeContext` 回调分派
- `../ZeusRuntimeLib/ZeusABIInternal.hpp`：仅供运行库实现共享 `ZeusBox` 内部布局
- `../ZeusRuntimeLib/ZeusClosureABI.cpp`：闭包环境与捕获单元 ABI；捕获单元和闭包环境都是弱注册的 GC 节点
- `../ZeusRuntimeLib/ZeusGCABI.cpp`：VM 与 LLVM 共用的标记-拆边收集器；根是装箱值、永久槽根和捕获单元句柄
- `../ZeusLLVM/ZeusDynamicABI.hpp`：LLVM 侧动态 ABI 的声明文本和指令分类，
  不把容器实现塞回 `ZeusEmitter.hpp`
- `../ZeusLLVM/ZeusDynamicBinaryABI.hpp`：LLVM 侧二元运算 opcode 与声明文本
- `demo/hello.ll`：`print("hello")` 的 ABI 生成物（装箱→打印→归还）
- `demo/add.ll`：`x = 40 + 2; print(x)` 的 ABI 生成物（期望输出 `42`）
- `build_aot.bat`：`cl → lib → clang -c → link` 全流程，两个 demo 依次构建运行

## Emitter（`--emit-llvm`，v0 naive 栈机）

`../ZeusLLVM/ZeusEmitter.hpp`：主程序单编 IR → `.ll` 文本。

```
Zeus.exe --emit-llvm prog.zeu [out.ll]   # 默认输出 prog.zeu.ll
```

* 输入是 **freestanding 主程序单编**（无包合并）：`print` 按名走本征函数；class
  声明提供布局与函数签名元信息，不在 AOT 入口执行运行时注册；import/包初始化仍不支持。
* 支持：PUSH int/bool/string/nil、LOAD/STORE/ASSIGN global/local、boxed 二元运算、
  JUMP/JUMP_IF/LOOP_BREAK、`print`/`delayPrint` 的位置参数多参调用（通过共享的
  `argc + ZeusValue*[] + result-out` ABI 传递；固定签名函数仍使用原生 LLVM 参数类型），
  `LOAD "print"` 与 `LOAD_NATIVE` 两种形态）；函数参数按注解逐个选 ABI：已有原生
  映射的 `int`/`bool`/用户 class 保持 LLVM 类型，未标注及暂无原生映射的类型使用
  `ZeusValue*`（调用期间借用）；未标注返回值通过调用方提供的 `ZeusValue` out 指针返回；
  带 `class_layout` 元信息的用户类可生成具名 LLVM struct，原生函数内的 `obj.field` 可直接
  lowering 为 `extractvalue`，完整签名的普通函数调用会生成
  typed LLVM call，静态运算仍复用 `DirectCall`。LLVM class 指针可在函数 ABI 边界装箱为
  非拥有的 typed handle；`delete` 调析构后使动态别名失效，动态值不会延长对象生命周期。
  顶层 `new Class(...)` 可按 class 布局分配对象、初始化支持的字段默认值，并用已标注的
  `init` 签名调用构造函数；构造结果可保留为 class 指针，传给 `print` 或固定签名函数。
  `-> nil` 函数允许省略 `return` 并在函数末尾隐式返回 `nil`；裸 `return` 等价于 `return nil`。
  `@static` 函数必须通过严格 LLVM 类型检查。
* 闭包捕获：原生函数里的 `func` 走 `zeus_closure_create` + `zeus_capture_cell_*` ABI。
  引用捕获（`[&x]`）复用外层 cell，复制捕获（`[x]`）在闭包创建时快照成新 cell，
  之后该闭包再次调用时保留对这个捕获槽的修改。捕获单元和闭包环境都是弱注册的
  GC 节点：普通生命周期仍由 `shared_ptr` 引用计数负责，收集器只负责打断
  “捕获单元 ↔ 闭包”这类引用环，因此自环闭包在分配触发回收后会被释放。
  `__gc_collect()` / `__gc_native_nodes()` 两个内置在 VM 和 AOT 两侧都可用，
  用来强制回收和观察存活节点数。
* 全局量统一存放在运行库槽表（`zeus_value_global_store/load`）：顶层语句和原生函数
  读写的是同一个格子。声明为 `int` / `bool` / `new <Class>()` 的全局量走预扫描
  （`prescanGlobalTypes`）识别出的专用通道：标量用不装箱的
  `zeus_global_load_i64` / `store_i64`，class 实例用 `require_class_ptr` 拆箱回原生
  指针（字段访问因此能解析布局）。所有 `alloca`（顶层与原生函数 alike）在生成后统一
  提升到各自函数的 `entry` 块——`-O0` 下循环体内的 `alloca` 每轮都会撑大栈帧，只会让
  装箱循环爆栈。
* 已知限制（后端仍不一致，改动前请先确认）：
  * AOT 不支持 `throw` / `try-catch`：`new Exception(...)` 在原生函数里无法解析
    （解释器正常）。这是缺一整套异常 lowering + 跨 LLVM 边界的展开，不是一个小补丁。
  * AOT 不支持 `import`；被 import 的包必须已在同一程序里（内建包的原生函数已全部
    注册进 standalone 表，见 `ZeusBuiltins/BuiltinNativeABI.hpp`）。
  * String class 对象没有释放路径：顶层或函数里的字符串字面量各 `malloc` 一次，
    无人 `free`。要修需要生命周期分析（或把字面量降级成装箱字符串）。
  * 装箱容器（Array / Dict）成环不回收：共享收集器只覆盖闭包环境 + 捕获单元。
    也不能直接把容器塞进共享收集器——解释器从不把它的 Cell 注册成根，那会让收集器
    把活着的容器判成垃圾。正确的做法是在解释器自己的引用计数世界里做环检测
    （试删法之类）。
* 不支持：`print` 的 `**kwargs`、用户函数的 `*args`/`**kwargs` ABI 打包、顶层对象销毁、
  字段写入/动态方法分派、动态 class 字段/运算操作、容器 lowering、异常、import
  （报错指明 Phase）。
  容器和二元运算运行时 ABI 已独立存在，下一步只需新增对应 lowering/context 接线。
* `for(N)` 的负结束值与零步长检查由独立的 range-check runtime ABI 执行；动态值按 VM 的 BigInteger 检查规则处理。

`build_aot.bat` 末尾已接 emitter 往返：`hello.zeu/add.zeu → *_gen.ll → *_gen.exe`。

## 测试

```
ZeusZTest\all_features_test.zeu        REM 解释器全量冒烟（48 项）
ZeusZTest\gc_native_test.zeu           REM 同一脚本，VM 与 AOT 各跑一遍（24 项）
zeusc.bat ZeusZTest\gc_native_test.zeu -o %TEMP%\gc.exe && %TEMP%\gc.exe
ZeusZTest\global_slot_test.zeu         REM 顶层与函数的全局量共享（7 项）
ZeusZTest\object_in_loop_test.zeu      REM 循环体内 new/delete（5 项）
ZeusZTest\run_gc_collector_test.bat   REM 收集器 C++ 单测（10 项，含跨线程用例）
```

`gc_collector_test.cpp` 覆盖 Zeus 源码暂时表达不了的场景：自环闭包、互环闭包、
分配触发回收，以及收集与 store 并发时存活闭包不被拆掉。

循环回边 phi 的“值”和“前驱标签”都留到函数生成完再回填（`%loop_phi_*` /
`%loop_lbl_*`），因此 `new` / `delete` 的空检查在循环体里切分基本块也能正确落地。

`gc_collector_test.cpp` 覆盖 Zeus 源码暂时表达不了的场景：自环闭包、互环闭包、
分配触发回收，以及收集与 store 并发时存活闭包不被拆掉。

## 下一步

动态对象 lowering（让 `LIST_*`/`DICT_*` 直接调用上述 ABI）；`--aot` 一键（Emitter + 工具链驱动收进 `Zeus.exe`）；函数/闭包（Phase C）；
`ZeusContext`（Phase B）到来后包能力与 partial VM 回退。
