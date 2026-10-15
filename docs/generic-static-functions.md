# 泛型静态函数

用 `@type(T)` 声明一个由调用实参推断的类型参数，不需要 `<T>` 调用语法。

```zeus
@type(T)
func identity(value: T) -> T {
    return value;
}

let number: int = identity(123);
let text: string = identity("hello");
```

LLVM 为每种实际类型生成一个具体函数，同类型调用共用该函数。参数和返回值沿用该类型已有的原生表示；该类型本来需要运行时支持时，泛型不会消除这部分支持。VM 执行原函数，参数按现有动态调用规则传递。

## 内存操作

`T` 可以用于 `@faddr` 的读取和写入类型。地址仍然是 `int`，可操作的类型与普通 `@faddr` 一致。

```zeus
@type(T)
func save(address: int, value: T) {
    @faddr(address) -> T = value;
}

let value = 7;
save(@addr(value), 123);
print(value); // 123

@type(T)
func read(address: int, exemplar: T) -> T {
    return @faddr(address) -> T;
}

print(read(@addr(value), 0)); // exemplar 提供 T=int
```

地址本身不携带类型，`read` 需要一个 `T` 类型的实参供推断。VM 从对应形参的实际值确定读取类型。

## 内联与嵌套调用

```zeus
@type(T)
inline func duplicate(value: T) -> T {
    return value + value;
}

print(duplicate(12));   // 24
print(duplicate("ab")); // abab
print(identity(identity(123)));
```

依赖 `T` 的操作在 LLVM 生成具体函数时按具体类型处理。`inline` 和 `@no_safepoint` 仍遵循已有规则。

## 当前边界

- 一次声明一个类型参数，且它必须出现在形参类型中。
- LLVM 调用时实参类型必须可静态确定；多个 `T` 形参必须推断为同一类型。
- LLVM 支持普通函数、类静态方法、实例方法和 `impl` 方法的直接调用，以及嵌套泛型调用。函数和方法都支持位置、命名、默认参数；引用参数沿用 `&value: T` 语法。
- LLVM 暂不支持泛型函数作为普通函数值传递或闭包捕获。
- 类型参数名称不能覆盖已有基础类型或类名称。
- 无返回值的函数继续使用已有的 `nil` 返回约定；专门化不会自动移除 safepoint。

## 类方法和 impl

实例方法的接收者保持所属类型，只有标为 `T` 的形参参与类型推断。给方法做专门化不会把类本身变成泛型类。

```zeus
class MemoryWriter {
    using &this;
    var ptr: int = 0;

    func init(address: int) { this.ptr = address; }

    @type(T)
    func save(value: T) {
        @faddr(this.ptr) -> T = value;
    }

    @type(T)
    func static identity(value: T) -> T { return value; }
}

let value = 7;
let writer: MemoryWriter = new MemoryWriter(@addr(value));
writer.save(123);
print(value); // 123
print(MemoryWriter.identity("hello"));

impl string {
    using &this;
    @type(T)
    inline func echo(value: T) -> T { return value; }
}

let text = "receiver";
print(text.echo(456)); // T=int，接收者仍为 string
```

`MemoryWriter` 借用地址，不管理对应内存的寿命。`@faddr` 的读写规则与普通函数相同。VM 继续调用原方法，LLVM 调用具体类型的方法版本；接收者沿用该类型已有的原生表示。

字节码 v4 会保存类型标记、引用形参和实参来源。旧 v2/v3 文件继续可读；需要引用语义的旧字节码应从源文件重新编译。
