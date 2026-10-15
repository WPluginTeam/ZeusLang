# VM / LLVM 共用语义表（初版）

这张表记录同一段 Zeus 程序在 VM 与 LLVM/AOT 上应当具有的共同语义，并把已确认的后端边界单独标出。它是当前代码的首批盘点，不等于完整的语言规范。

状态含义：

- **一致**：两端现有实现走同一语义。
- **不一致**：两端对同一程序的接受范围或结果不同，应优先修正。
- **设计边界**：两端能力不同是当前明确选择，不作为迁移缺陷。
- **待定**：现有实现只能确认部分能力，先决定语言语义再补后端。

| 语义 | VM | LLVM / AOT | 状态与后续 |
|---|---|---|---|
| `if` / 条件跳转真值 | 支持 bool、整数、浮点、字符串；空字符串和零为假，未特殊处理的对象值为真 | 动态 `ZeusValue` 走运行时真值函数；静态路径目前只接受 bool 和整数，其他静态类型会报错 | **不一致，P0。** 先明确统一真值规则，再补 VM/AOT 对照覆盖，尤其是浮点、字符串、对象和 `nil`。证据：`ZeusRunner/ZeusVM.hpp::truthy`、`ZeusLLVM/ZeusEmitter.hpp` 条件跳转 lowering。 |
| `shared` 槽位单次读写 | 共享堆 `SharedCell` 对单次读写加互斥锁 | 通过共享堆 ABI 读写；整数、布尔值有专用 ABI，其他值使用动态值 ABI | **基本一致。** 这只保证一次读或写，不代表 `x = x + 1` 这样的读改写整体原子。证据：`ZeusRuntimeLib/ZeusSharedHeap.hpp`、`ZeusLLVM/ZeusEmitter.hpp`、`ZeusRuntimeLib/ZeusABI.cpp`。 |
| `SHARED_LOCK` / `SHARED_UNLOCK` | VM 有对应指令和执行路径，可包住复合操作 | emitter 没有对应 lowering | **不一致，P1。** 要么为 LLVM 增加同一锁语义，要么从共同语言语义中移除这组指令；不能只在 VM 依赖它。证据：`ZeusRunner/ZeusVMOperator.hpp`、`ZeusRunner/ZeusVM.hpp`。 |
| `@native` 外部符号 | IR 将受支持的声明编码为 native injection 句柄，VM 从独立注入表解析；普通内建函数仍走内建注册表 | LLVM/AOT 将 `@native` 声明接到外部 ABI 符号 | **部分一致。** 通用 VM `@native` 动态装载仍未实现；标准库注入项需在 `ZeusRunner/ZeusBuiltins/NativeInjectionRegistry.hpp` 中提供 VM 适配器。证据：`ZeusIR/IRGenerator.hpp`、`ZeusRunner/ZeusVM.hpp`、`ZeusLLVM/ZeusEmitter.hpp`。 |
| 泛型静态函数作为值、捕获泛型闭包 | 尚未确认与 LLVM 相同形式的覆盖 | 静态路径限制泛型函数值；泛型特化中的闭包捕获会被拒绝 | **待定，P2。** 先确定它们是否属于语言承诺，再统一报错或实现两端支持。直接调用泛型函数已有覆盖。证据：`ZeusTyping/TypeChecker.hpp`、`ZeusIR/IRGenericStatic.hpp`。 |
| `interface` | 编译前检查实现合约；没有运行时接口对象 | 同一份合约检查；IR 不生成 interface 运行时表示 | **两端当前一致。** 若需要运行时多态，另立语言功能设计；不要把编译期合约误认为动态分派。证据：`ZeusParser/InterfaceConformance.hpp`、`ZeusIR/IRGenerator.hpp`。 |
| 匿名函数表达式 | 解析阶段拒绝 | 解析阶段拒绝 | **共同缺口。** 不是后端迁移差异；若要支持，需先定义捕获、生命周期和函数值语义。证据：`ZeusParser/ExpressionSyntaxValidator.hpp`。 |

## 当前正在变动的语义

当前工作区还包含左值槽位、class 复制、字段默认值、动态 `new`、泛型 class、interface 和静态容器等未提交改动及相应用例。这些功能在完成 VM 与 AOT 的同源对照验证前，状态统一记为**实现中，尚未确认为共同语义**。

## 建议推进顺序

1. 先定条件真值规则，并让 VM 与 LLVM 接受相同类型、产生相同结果。
2. 再处理 `SHARED_LOCK` / `SHARED_UNLOCK`，同时写清共享读改写的原子性边界。
3. 对正在改动的 class、左值和容器功能建立 VM/AOT 对照用例；只记录经过两端验证的语义。
4. 最后决定泛型函数值、泛型闭包捕获和运行时 interface 是否进入语言目标。
