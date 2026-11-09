# `with` 展开与 `fin` 关键字

状态：设计已定，实现分步进行。

## 1. 为什么改

`with` 现在有专门的 IR（`WITH_BEGIN` / `WITH_END`），两个后端各维护一套栈：

- VM：`withContextStack`（`ZeusRunner/ZeusVM.hpp`）
- AOT：`withValues`（`ZeusLLVM/ZeusEmitter.hpp`）

清理逻辑因此散在三处：VM 的 `WITH_END`、VM 的异常展开、AOT 的 `WITH_END`。
每多一种「提前离开作用域」的方式，就得再补一处，而且容易补漏。

现状实测（VM）：

| 离开方式 | `operator[~with]` |
|---|---|
| 正常走到块尾 | 跑 |
| `throw` 冒出去 | 跑（本次已修） |
| `return` | **不跑**（`RETURN_CLOSED false`） |
| `break` / `continue` | 未验证，按实现推断不跑 |

`socket.zeu` / `ssl.zeu` 用 `with` 做 RAII，所以这两条漏的路径会直接漏掉 close。

## 2. 约定

`operator[~with]` 是「离开作用域」的钩子，和 `[~]` 对称：**进来了就必须走**。

`fin`（finally）在所有离开方式上执行：

| 离开方式 | 跑不跑 |
|---|---|
| 正常走到 `fin` 前 | 跑 |
| `throw` 冒出去 | 跑 |
| `return` | 跑 |
| `break` / `continue` | 跑 |
| 进入前就失败（`operator[with]` 自己抛） | 不跑 |
| 进程 abort / `exit()` | 不跑 |

`with (obj) [&x] { body }` 展开为：

```zeus
let x = obj.operator[with]();
try {
    ...body...
} fin {
    obj.operator[~with]();
}
```

进入失败时没有 `fin`——那时 `x` 还没赋值。

## 3. 实现策略：把 `fin` 复制到每条退出边

不引入「pending return / pending break」这类运行期状态，而是**把 `fin` 块的指令复制到每条退出边**：

- 正常结束：块尾顺序执行 `fin`
- `return` / `break` / `continue`：先发 `fin` 的指令，再发原来的跳转
- `throw`（异常边）：pad 里发 `fin` 的指令，然后继续上抛

理由：

1. 对 `with` 来说 `fin` 体只有一条调用，复制的代价可以忽略；
2. 不需要在两个后端各加一份「延迟返回」状态机（那是真正难对齐的部分）；
3. 语义天然正确：`fin` 只有单一出口，不存在「跑两次」——上一轮担心的「块内 catch 接住导致 `~with` 双调」也就自动消失了。

代价：嵌套 `with` 深 k 层、退出点多 n 个时，`fin` 指令最多复制 k×n 份。实际嵌套很浅，可以接受；若将来 `fin` 体变大，再换成「跳转到共享的 fin 标签 + 一个小状态位」。

限制（第一版）：

- `fin` 块里不允许 `return` / `break` / `continue`（会破坏「单一语义」），编译器报错；
- `fin` 块里允许再 `throw`：按「新异常替换旧异常」处理（旧异常丢失，不引入 suppressed 链）。

## 4. 迁移步骤

1. **语法**：`try { } catch (&e) { } fin { }`；`fin` 可单独跟 `try`（`catch` 可省）。
2. **IR**：`TRY_BEGIN` 记录 `finPc`，`TRY_END` 标记区域结束；`fin` 块体单独成段。
3. **VM**：`RETURN` / `BREAK` / `CONTINUE` 执行前跑当前区域的 `fin`；异常展开时按 handler 深度跑 `fin`（现有 `handler.withDepth` 那套换成通用的 fin 深度）。
4. **AOT**：`RETURN` / `BREAK` / `CONTINUE` 发射点先复制 `fin` 指令；异常 pad 里同样复制。
5. **展开 `with`**：IR 生成器把 `WITH_BEGIN/END` 换成 `CALL operator[with]` + `try/fin` + `CALL operator[~with]`。
6. **删除**：`WITH_BEGIN` / `WITH_END` 指令、`withContextStack`、`withValues`、`withDepth`、以及两个后端的 `WITH_*` 分支。

第 5、6 步做完后，后端不再认识 `with`，只剩 `try/fin` 一套机制。

## 5. 回归矩阵

每条都要 VM 与 AOT 输出逐字一致：

| 用例 | 期望 |
|---|---|
| 正常走到块尾 | `~with` 一次 |
| 块内 `throw`，块外 catch | 一次 |
| 块内 `try/catch` 接住 | 一次 |
| 块内 `return` | 一次 |
| 块内 `break` / `continue` | 各一次 |
| 嵌套 `with`（两层） | 逆序各一次，共两次 |
| `operator[with]` 自己抛 | 零次（没进去） |
| `fin` 里再 `throw` | 新异常可见，`~with` 已执行 |

## 6. 命名

`fin` 与 `func` / `let` / `var` / `impl` / `with` 保持同一风格：短、无下划线、不带 `finally` 那样的长尾。
