# 实例接收者与返回值

普通 class 实例方法没有声明返回类型时，直接返回接收者会推断为共享返回：

```zeus
class Box {
    using &this;
    var value = 0;
    func add(n) {
        this.value = this.value + n;
        return this;  // 推断为 -> &Box
    }
}
let box = new Box();
box.add(2).add(3);  // box.value == 5
```

`using &me` 等接收者别名使用同一规则，括号不改变推断结果。
所有返回分支必须直接返回接收者；若同时返回其他值，或者可能不返回值，
必须显式声明返回类型。

显式 `-> Box` 保持值返回；显式 `-> &Box` 保持共享返回。
`return otherObject`、`let copy = this; return copy;` 仍遵循值语义。
规则在类型检查阶段生成返回类型，由 VM 和 LLVM 共用。

## this 槽位

普通 class 方法有独立的接收者参数槽。它保存对象句柄，句柄包含对象编号
和代次，用于找到实际对象。对象字段另存，不内嵌在 this 槽中。

```text
调用者 box 的槽 ── 对象句柄 ──┐
                            ├── 同一对象及其字段
方法 this 的槽 ─── 对象句柄 ──┘
```

- `this.value = 10` 修改共同指向的对象。
- `this = new Box()` 只重绑方法内部的槽，不改调用者变量。
- `using &this` 绑定接收者，不把调用者的变量槽传入方法。

## 相近规则

| 操作 | 当前行为 |
| --- | --- |
| `let b = a` | 默认复制 class 对象，复制钩子可改变结果 |
| 普通对象参数 `func f(b: Box)` | 参数槽独立，默认共享对象实例 |
| 引用参数 `func f(&b: Box)` | 共享调用者变量槽，重绑会影响调用者 |
| `let local = this` | 默认复制对象 |
| 普通值返回参数或字段 | 复制对象；返回已拥有的局部值可转移结果 |

对象实例共享与变量槽共享是两种不同的行为。
