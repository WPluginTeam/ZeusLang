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
* 不支持：`print` 的 `**kwargs`、用户函数的 `*args`/`**kwargs` ABI 打包、闭包捕获、顶层对象销毁、
  字段写入/动态方法分派、动态 class 字段/运算操作、容器 lowering、异常、import
  （报错指明 Phase）。
  容器和二元运算运行时 ABI 已独立存在，下一步只需新增对应 lowering/context 接线。
* `for(N)` 的 `COUNT_CHECK_*` 暂跳过（警告一次，随异常模型补齐）。

`build_aot.bat` 末尾已接 emitter 往返：`hello.zeu/add.zeu → *_gen.ll → *_gen.exe`。

## 下一步

动态对象 lowering（让 `LIST_*`/`DICT_*` 直接调用上述 ABI）；`--aot` 一键（Emitter + 工具链驱动收进 `Zeus.exe`）；函数/闭包（Phase C）；
`ZeusContext`（Phase B）到来后包能力与 partial VM 回退。
