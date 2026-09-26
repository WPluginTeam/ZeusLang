# ZeusLang
Zeus is an interpreted or compiled language that combines dynamic and static features.   

You can run Zeus code on multiple platforms with the ZeusVM or compile it to LLVM IR and run it on any platform that supports LLVM.

## Features
- Combines dynamic and static typing
```
// Both could be compiled to LLVM IR or interpreted in ZeusVM

// static typing
func add(a: int, b: int) -> int {
    return a + b
}

// will be compiled as dynamic type
func add(a, b) {
    return a + b
}
```
- Custom memory management and strict scope rules
```
let boo = new MyClass();
delete boo;
```
```
func foo() {
    let boo = new MyClass();

    func bar() [&boo] {
        // without capture boo, you could not access it here
        boo.doSomething();
    }
    delete boo;
}

foo();
```
- Convenient to use your C++ libraries
```
// With &int you can access the value directly

@native("get_int", from="mylib.dll")
decl func get_int(k: int) -> &int;
```
- Type inference and dynamic type
```
// will be compiled to ZeusAny type as a dymamic type
func add(a, b) {
    return a + b;
}

let a = 123;      // auto inferred as int
let b = "string"; // string
let c = new MyClass(); // MyClass
```
- Operator overloading
```
class MyClass {
    using &this;
    
    let value: int = 0;

    func operator[+](other: MyClass) -> MyClass {
        let result = new MyClass();
        result.value = this.value + other.value + 1;
        return result;
    }
}

let a = new MyClass();
let b = new MyClass();
let c = a + b; // will call operator[+]
```