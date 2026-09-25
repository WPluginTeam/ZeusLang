## 变量语法

Zeus 通过 let/var关键字进行声明并定义变量：

```
let a = 123; // 声明局部的变量123
var b = 456; // 声明全局的变量456
```

同时支持const关键字禁止对变量进行修改：

```
let const a = 123; // 声明局部的常量123
var const b = 456; // 声明全局的常量456

a = 456; // error: 不能修改常量
b = 789; // error: 不能修改常量
```

## Capture-Copy 语义

Zeus 提供 Capture-Copy 语义，通过该语义可在func、for、import等代码中实现不同功能。

Capture 语义，即引用捕获，使用 & 符号表示，表示捕获外部变量的引用，可直接修改外部变量，不会产生复制。
Copy 语义，即复制捕获，直接使用变量名表示，表示复制外部变量的值，修改该变量不会影响外部变量。

此外，Capture-Copy 语义还支持 const 关键字，表示捕获/复制的变量为常量，不能修改。

```
[&item]          引用捕获
[item]           复制捕获
[const &item]    常量引用捕获
[const item]     常量复制捕获
```

### function独有

```
[&]              全捕获 (捕获函数内所有外部变量的引用)
[=]              全复制 (复制函数内所有外部变量)
```

### for独有

```
[const? &? key : ... value] 字典遍历 (支持引用捕获直接修改键或值)
[const? &? index]           数值遍历 (支持引用捕获直接修改索引变量)
```

## func 函数语法

``func name([const? &? param, ...]) [const? &? var, ...] {}``

为了支持闭包语法，函数语法通过支持捕获、复制外部函数的变量进行闭包。

```
func outer () {
    let a = 0;
    let b = 1;

    func inner() [&a, b] {
        a = a + 1;   // 捕获外部变量a的引用，可以修改a
        b = b + 1;   // 捕获外部变量b的复制，可以修改b，但不会影响外部的b
        print(a);
        print(b);
    }
    return inner; // 返回闭包函数
}
```

当一个内部函数不使用捕获/复制语法时，默认不捕获任何外部变量，即内部的local变量栈**不会包含任何除去全局变量的外部变量**。

## 函数变长、默认参数

```
func foo (arg1, arg2, *args, **kwargs) {

    print( type(kwargs));
}

func doo (arg1=123) {
    print(arg1);
}

foo(1,2,"test", "what?!", default="test");
// <Array> <dict>
```

## decl 声明语法

Zeus 提供严格模式与宽松模式，通过 decl 关键字避免 backward-declaration call 问题：

```
func test () {
    test2(); // call undef func
}

func test2 () {
    print("test2");
}
```

你可以在非 --strict-decl 模式下运行上述代码，因为 Zeus 允许 later-function call。

但是为了避免项目混乱，Zeus 提供 --strict-decl 模式，在编译时开启：
``Zeus.exe -c source.zeu --strict-decl``
会禁止所有 later-function call，要求所有函数必须在调用前声明。

此时可以使用 decl 关键字进行函数声明：

```
decl func test2 ();

function test () {
    test2(); // ok
}
func test2 () {
    print("test2");
}
```

## for 循环语法

```
for (list) [const? &? item] {}
for (dict) [const? &? key : ... value] {}
for (numExpr) [const? &? index] {}
for (num1..num2..step) [const? &? index] {}
for (condExpr)  {}
```

注:
``for (condExpr)`` 无捕获/复制语法

## 面向对象语法（预）

```
class Object {
    let const a = 123; // 成员常量
    let b = 456;       // 成员变量

    func Object (const? &? param1, ...) [&this] { // 构造函数，使用类名作为函数名
        this.a = param1;
        this.b = 2;
    }

    func getA () [&this] {
        return this.a;
    }

    func getClassName () { // class 全局没有用using &...; 并且函数没有捕获&this，为静态函数，static关键字可写可不写
        return "Object";
    }
}

class Object2 {
    let a = 123;
  
    using &self;
  
    func getA() {
        return self.a;
    }
    func static getB() {  // class 全局有using &self; 并且函数有static声明，为静态函数。如果没有static关键字则会报错
        return "B";
    }
}
```

在对象定义阶段使用var声明公共变量，使用let声明私有变量

案1：
使用 func getA(&this) 作为成员函数的定义方式，this指向当前对象实例
当参数中第一个参数不为"&"开头的变量时，定义为静态成员函数，如 func getClassName()

案2：
使用 func getA() 作为成员函数的定义方式，this隐式指向当前对象实例
使用 func static getClassName() 作为静态成员函数的定义方式

案3：
使用 func getA() [&this] 捕获复制语义作为成员函数的定义方式，this指向当前对象实例，
成员函数不允许使用捕获/复制语义，仅能使用 [&varName] 将varName作为指向当前对象的引用。
不存在捕获复制语义时，定义为静态成员函数，如 func getClassName()

最终决案：

```
class ...
    func getA() [&this] {} // 成员函数，使用capture-copy语义声明定义了实例化对象指针this

class ...
    using &this;

    func getA() {} // 成员函数，隐式使用this指向当前对象实例
    func static getClassName() {} // 静态成员函数

// 对于成员函数，无论static/normal函数都不允许使用capture-copy语义，因为无意义。

```

## 包（预）

包是Zeus的模块基础单元，通过 ``import`` 关键字导入包。
这是一个包示例：

```
// /package/math.zeu
func add(a, b) {
    return a + b;
}
```

使用相对路径进行导入包：

```
// /main.zeu
import package.math; // relative path

var result = package.math.add(1, 2);
print(result); // 3

func printZeus () {
    print("Hello, Zeus!");
}
```

使用相对路径导入相同路径下的包：

```
// /main2.zeu
import main;
main.printZeus(); // Hello, Zeus!
```

使用别名导入包，和使用部分导入，部分导入不会暴露包：

```
// /main3.zeu
import package.math [&math];
print(math.add(3, 4)); // 7

import main {printZeus};
printZeus(); // Hello, Zeus!

// main.printZeus();  // error: main包未暴露
```

## Zeus 文件后缀

Zeus 的源文件后缀名为 ``.zeu``，编译后的文件名为 ``.zeu.zir``.

``.zeu`` 是源文件，包含所有代码、注释、空行。
``.zeu.zir`` 是编译后的二进制文件，由 ZeusVM 直接加载执行。


## Thread
Zeus提供了对C++ std::thread的封装支持，并且引入```shared```关键字允许用户自行管理变量的生命周期，并且自动于线程间共享。

寻址：Upvalue -> Local -> Global -> Global Heap(Shared)  


```
import ZeusBuiltins.thread [&];

var shared counter = 0;

shared class ObjCounter {
    let val = 0;

    func init () [&t] {
        print("init obj counter with val:", t.val);
    }

    func get () [&t] {
        t.val = t.val + 1;
        return t.val;
    }
}

var Obj = new ObjCounter();

func test () {
    print("Thread started");
    print("Get:", Obj.get());
}

for (0..5) [&index] {
    Thread thread = new thread.Thread(test);
    thread.start(test);
}
print("Fin!");
```

使用 Zeus 的多线程无需在意线程间变量共享的问题， Zeus VM 使用引用主线程的数据实现数据共享。

目前shared关键字基本已经废弃，除去使用shared自行管理生命周期（需要使用delete手动回收）。

## With
Zeus 支持类似Python的With语法：
```
class Resource {
    func init () {
        this.data = 123;
    }

    func operator[with] () {
        print("Entering with block, data =", this.data);
        return this;
    }

    func operator[~with] () {
        print("Exiting with block, data =", this.data);
    }
}

let obj = new Resource();
with (obj) {
    print("Inside with block, data =", obj.data);
}

with (new Resource()) [&res] {
    print("Inside with block, data =", res.data);
}
```

## Zeus C++

使用:

```
using ZeusBuiltins.cppfunctions [&];

let socket = cppfunctions.socket.Socket();
socket.bind("localhost", 80);

cppfunctions.array.array_push(...);
cppfunctions.array.array_pop();

...
```

## Zeus 对象
在 Zeus 中有很多内置对象如 Array, Dict, String, Socket, Thread 等等。
这些对象都定义在 ZeusBuiltins 包中，无需通过 import 导入，直接使用即可。

## Zeus 调试方法

```
Zeus 
	--debug-channels=VM,IR,PARSER
        -d -debug
        -c [CompileFile]
        -r [Run Compiled File]
        [File path] - run zeu code (no compiled)
```

cl /O2 /MD /DNDEBUG /utf-8 /std:c++17 Zeus.cpp /Fe:Zeus.exe

[FOR LLM]实现过程中注意：

1. 不同逻辑块之间必须要有空行，例如赋值区、调用函数区
2. 不同代码块之间必须要有空行，例如函数定义代码、if代码块、else代码块、for代码块。你还是看不懂，我直接说if、for、函数定义前面必须空一行行了吧？
3. 严禁多个语句压缩在同一行内，如int x; dox();return;
4. 不允许将注释当作空行处理，注释不算空行，注释前一行必须是空行
5. 严禁深层代码块嵌套
6. 严禁写重复的代码，提取为inline或者函数调用与IRUtils或者ZeusVMUtils
7. 严禁在不属于某个区块的地方写别的代码
8. 严禁使用全局标记
9. 使用 Zeus --debug-channels=IR/PARSE 进行调试输出，避免直接使用Powershell进行文件写入，而是使用VSCode自带的接口进行文件读取和写入。
10. 写代码的时候注意不要拆东墙补西墙，修改某部分代码前想好可能会影响什么其他部分的代码，避免新实现了一个功能导致旧的功能失效出现问题。
