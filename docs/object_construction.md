# Zeus 对象构造语法指南

## 1. 类定义语法

```zeus
class ClassName {
    using &self;  // 必须声明,才能在方法中使用 self
    
    // 字段声明 - 必须有默认值
    var fieldName1 = defaultValue1;
    let fieldName2 = defaultValue2;  // let 表示常量字段
    
    // 方法定义
    func methodName(param1, param2) {
        // 访问字段: self.fieldName
        // 调用方法: self.methodName()
    }
    
    // 运算符重载
    func operator[+](other) {
        // ...
    }
}
```

## 2. 对象创建

### 基本语法
```zeus
var obj = new ClassName();  // 不支持构造函数参数!
```

### 重要限制
- ❌ **不支持**带参数的构造函数
- ❌ **不能**写 `new ClassName(arg1, arg2)`
- ✅ **必须**在创建后手动设置字段

### 正确的初始化方式
```zeus
// 方式1: 逐个设置字段
var person = new Person();
person.name = "张三";
person.age = 25;

// 方式2: 使用链式调用(如果方法返回 self)
var list = new List();
list.append(1).append(2).append(3);

// 方式3: 提供工厂方法
func createPerson(name, age) {
    var p = new Person();
    p.name = name;
    p.age = age;
    return p;
}
var person = createPerson("李四", 30);
```

## 3. 字段声明

### 必须有默认值
```zeus
class Example {
    using &self;
    
    // ✅ 正确 - 有默认值
    var count = 0;
    var name = "Default";
    var items = [];
    
    // ❌ 错误 - 没有默认值
    var value;  // 这会报错!
}
```

### var vs let
```zeus
class Example {
    using &self;
    
    var mutable = 0;      // 可变字段
    let constant = 100;   // 常量字段(不可修改)
}
```

## 4. self 的使用

### 必须声明 using &self
```zeus
class MyClass {
    using &self;  // 必须!
    
    var value = 0;
    
    func getValue() {
        return self.value;  // 使用 self 访问字段
    }
    
    func setValue(v) {
        self.value = v;     // 使用 self 修改字段
    }
}
```

### 注意事项
- ❌ 不要用 `&this`,Zeus 使用 `&self`
- ✅ 在方法中访问字段必须用 `self.fieldName`
- ✅ 调用其他方法用 `self.methodName()`

## 5. 访问对象成员

### 字段访问
```zeus
var obj = new MyClass();
var x = obj.fieldName;        // 读取
obj.fieldName = newValue;     // 修改
```

### 方法调用
```zeus
obj.methodName();             // 无参数
obj.methodName(arg1, arg2);   // 有参数
```

### 链式调用
```zeus
// 方法返回 self 可以链式调用
obj.method1().method2().method3();
```

## 6. 完整示例

```zeus
class Vector {
    using &self;
    
    var x = 0;
    var y = 0;
    
    func set(newX, newY) {
        self.x = newX;
        self.y = newY;
        return self;  // 支持链式调用
    }
    
    func toString() {
        return "(" + str(self.x) + ", " + str(self.y) + ")";
    }
    
    func operator[+](other) {
        var result = new Vector();
        result.x = self.x + other.x;
        result.y = self.y + other.y;
        return result;
    }
}

// 使用
var v1 = new Vector();
v1.set(3, 4);

var v2 = new Vector();
v2.set(1, 2);

var v3 = v1 + v2;  // 运算符重载
print(v3.toString());  // 输出: (4, 6)
```

## 7. List 类示例

```zeus
class List {
    using &self;
    var data = [];  // 内部使用数组
    
    func append(item) {
        array_push(self.data, item);
        return self;
    }
    
    func get(index) {
        return array_get(self.data, index);
    }
    
    func length() {
        return len(self.data);
    }
}

// 使用
var list = new List();
list.append(10).append(20).append(30);
print(list.length());  // 3
print(list.get(1));    // 20
```

## 8. 常见错误

### 错误1: 尝试使用构造函数参数
```zeus
// ❌ 错误 - Zeus 不支持
class Person {
    func Person(name) {  // 这不会被调用!
        self.name = name;
    }
}

var p = new Person("张三");  // 参数会被忽略
```

### 错误2: 忘记声明 using &self
```zeus
// ❌ 错误
class MyClass {
    var value = 0;
    
    func getValue() {
        return self.value;  // 错误!没有声明 using &self
    }
}
```

### 错误3: 字段没有默认值
```zeus
// ❌ 错误
class MyClass {
    using &self;
    var value;  // 编译错误!必须有默认值
}

// ✅ 正确
class MyClass {
    using &self;
    var value = 0;  // 有默认值
}
```

### 错误4: 使用 this 而不是 self
```zeus
// ❌ 错误
class MyClass {
    using &this;  // 错误!应该是 &self
    var value = 0;
}

// ✅ 正确
class MyClass {
    using &self;  // 正确
    var value = 0;
}
```

## 总结

Zeus 的对象系统特点:
- ✅ 支持类和对象
- ✅ 支持字段和方法
- ✅ 支持运算符重载
- ✅ 使用 `using &self` 声明
- ❌ 不支持构造函数参数
- ❌ 字段必须有默认值
- 🔧 创建后需手动初始化字段
