# Zeus Lang

Zeus is a public and free, scripting language wrote in C++.

### Struct

Zeus Code --- [Zeus IR Compiler] ---> Zeus IR --- [Zeus Virtual Machine] ---> Run


Zeus will compile code into ZeusIR, then using Zeus VM to execute it. It sounds like kind of Java or Python.

### Zeus Syntax

```
print("Hello world!");
```

###### var decl

```
var a = 123; // wherever you use keyword "var", it will decl a global var

func t() {
    let b = 3; // local var, only in "t" scope
    var c = 4; // also global var
}
```

###### func decl

```
func say (str) {
    print("Hey,", str, "!");
}
```

###### closure func (with capture/copy syntax)

```
func outer () {
    let a = 0;
    let b = 1;
    let c = 2;
  
    func inner () [&a, b] { // capture a, copy b into "inner" scope
        print(a, b);

        a = 1; // var a will change to 1
        b = 1; // var b also will change to b, but only in "inner" scope, it doesnt affect to outer's b

	let c = 123; // var c will not affect to the outer c, because it use "let" to decl.
        c = 345;     // the same reason above, inner c cover the outer c
    }

    func inner2 () [] {}  // copy all outer var, but zeus vm will not copy the thing you dont need
    func inner3 () [&] {} // capture all outer, but the same above

    return inner;
}
```

###### lambda func

```
(func () {
    print("hello");
})();

//seems like javascript style
```

lambda func also can capture/copy outer var.

###### func backward call

In Zeus, there's two way to call a backward-declaration function:

1. just call it
   ```
   func doA () {
       return doB();
   }

   func doB () {
       return 123;
   }

   print(doA());
   ```
2. use "--strict-decl" mode to avoid backward-declaration call, but you still can use "decl" syntax to use backward-declaration call:
   ```
   decl func doB ();

   func doA () {
       return doB();
   }

   func doB () {
       return 123;
   }

   print(doA());
   ```

that will provide a safer way to use backward-declaration function, avoiding undef or confusing questions about backward-declaration functions.

###### heap object create/del

```
var num = new {123};
var str = new {"hello world"};

print(num, str);

str = new {"what?!"};

del num;
del str;
```

it just like a common variable, but it provide you the method to manage your own variable lifetime.

in this example, we assign "str" to a new heap object "what?!", but at the same time, zeus vm will del "hello world" object automatically from the heap, because nothing is binded to it.

###### Loop syntax

```
for (range(0, 10)) [&a] {
    print(a);
}

var varList = [1, 2, 3];
for (varList) [&item] {
    print(item);

    item = 444;  // By using Capture Syntax, you can change var with using var assign stmt.
}

print(varList); // [444, 444, 444]

for (varList) [const &item] {
    // item = 555;      // You cant change it because it is declar
}
```
