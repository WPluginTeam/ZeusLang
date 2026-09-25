# Native function bindings

Zeus declarations own the function signature. `@native` supplies the external
link symbol; it does not introduce a second C++ signature table.

```zeus
@native("sum_i64")
decl func sum(a: int, b: int) -> int;

print(sum(20, 22));
```

The LLVM module declares and calls `@"sum_i64"` with the ABI derived from the
Zeus types. A C or C++ implementation can export that symbol with C linkage:

```cpp
extern "C" int64_t sum_i64(int64_t a, int64_t b) {
    return a + b;
}
```

Link the object or library when building an executable:

```bat
zeusc example.zeu --link native.obj -o example.exe
```

`--link` accepts `.obj`, static `.lib`, or a DLL import `.lib` file. For a DLL,
the DLL itself must also be available beside the resulting executable at run
time; Zeus does not load DLLs by path yet.

## ABI mapping

- `int` maps to LLVM `i64`; `bool` maps to `i1`.
- `string` maps to a pointer to `ZeusStringView`.
- `&T` maps to an LLVM pointer. Returning `new Class()` from a `->&Class`
  function returns the allocated instance pointer directly; a caller can pass
  it to `delete` to run `operator[~]` and free the allocation.
- An empty parameter type maps to `ZeusValue*`.
- An empty return type uses the dynamic out parameter ABI:
  `void(ZeusValue* out, ...)`.
- `nil` also uses the dynamic out parameter ABI and initializes the result to
  Zeus nil.

For a function definition without a return annotation, the presence of any
`return` statement selects the dynamic ZeusValue return ABI. If the body has no
`return` statement, the inferred return type is `nil`.

`ZeusValue` is declared in `ZeusRuntimeLib/ZeusABI.hpp`. Dynamic conversion of
pointer results currently supports `int`, `bool`, `string`, and declared Zeus
classes. In Zeus function bodies, class pointer results and already-returned
references are passed through as pointers. Returning a named global `int` or
`bool` still uses the VM's cell-reference opcode; LLVM needs a runtime API for
stable global-cell addresses before that form can be lowered there. Local
scalar addresses are not allowed to escape. Other `&T` values need a
type-specific pointer representation before they can cross a dynamic boundary.
