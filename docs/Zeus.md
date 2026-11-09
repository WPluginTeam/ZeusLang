# Zeus Lang

Zeus is a public and free, scripting language wrote in C++.

### Struct

Zeus Code --- [Zeus IR Compiler] ---> Zeus IR --- [Zeus Virtual Machine] ---> Run


Zeus will compile code into ZeusIR, then using Zeus VM to execute it. It sounds like kind of Java or Python.

### Zeus Syntax

```
print("Hello world!");
```

###### conditional expressions

```zeus
let label = ready ? "ready" : "waiting";
let count = maybeCount ?: 0;
```

`condition ? whenTrue : whenFalse` evaluates only the selected branch. Elvis,
`value ?: fallback`, uses `fallback` only when `value` is `nil`; it evaluates
`value` once and keeps values such as `false`, `0`, and `""`. Safe navigation
remains `object?.member`.

###### when pattern matching

```zeus
// Target matching mode (colon separator)
let grade = when (score) {
    90..100: "A",
    80..89:  "B",
    60, 70:  "Pass",
    else:    "Fail"
};

// Type pattern matching
when (item) {
    is string: print("string:", item);
    is int:    print("integer:", item);
    else:      print("unknown type");
}

// Condition mode (argument-free)
when {
    x > 0: print("Positive");
    x < 0: print("Negative");
    else:  print("Zero");
}
```

`when` supports statement mode, assignment mode (`let x = when ...`), and return mode (`return when ...`).
Key features:
- Branch separator: uses intuitive `:` syntax (`pattern: body`), consistent with C/JS switch-case and Zeus dictionaries
- Multi-value patterns: comma-separated values (`1, 2, 3: ...`)
- Type matching: `is TypeName: ...`
- Range patterns: `start..end: ...` or `in start..end: ...`
- Condition guards: `when { cond: ... }`
- Flexible branch bodies: single expressions, dictionary literals, or block bodies `{ ... }`
- Fallback: `else: ...` branch
- Non-local control flow: `break`, `continue`, and `return` are preserved naturally
- Zero VM overhead: desugared directly into sequential `let` and `if / else if / else` statements at parse time

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

###### importing a native DLL/SO

Use `@native` with a `from` path to load an exported C function from a shared
library. Relative paths are resolved from the `.zeu` file that declares the
function.

```zeus
@native("library_add", from="../native/math.dll")
decl func add(left: int, right: int) -> int;

print(add(20, 22));
```

The first implementation supports up to four by-value parameters of type
`int`, `bool`, or `string`, and return types `int`, `bool`, `string`, or no
return annotation for a C `void` function. `int` uses signed 64-bit values;
strings cross the C boundary as NUL-terminated UTF-8 `const char*` values, and
string results are copied into Zeus-managed storage. The library must export the
named symbol with a C ABI (`extern "C"` in C++). Reference, byte-buffer, float,
and class parameters are not supported by this first version.

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
