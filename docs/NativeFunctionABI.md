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

## VM native injection

The VM uses the same `@native` declarations for functions explicitly supported
by its injection table. Each entry maps the declaration name and link symbol to
a VM adapter in `ZeusRunner/ZeusBuiltins/NativeInjectionRegistry.hpp`; these
injected functions do not use `registerBuiltin`. Other external symbols remain
available to LLVM/AOT when linked, but cannot be called by the VM until a VM
adapter exists.

## ABI mapping

- `int` maps to LLVM `i64`; `bool` maps to `i1`.
- `string` maps to `ZeusNativeString` (`const char*`, LLVM `ptr`), pointing at the first UTF-8 byte in `[u64 byte length][UTF-8 bytes][NUL]`; the byte length is stored at `data - 8`. Static AOT string slots store this data pointer.
  Dynamic `ZeusValue` slots own the same length-prefixed buffer; decoding into a VM `UniversalVariant` copies it into `std::string`.
- `bytes` uses an owned pointer to the first byte of `[u64 byte length][bytes]`. The length is read from `data - 8`. Dynamic `ZeusValue` slots own the same layout; decoding into a VM `Bytes` value copies it into a vector. Each native allocation is released with `zeus_bytes_release`.
- Dynamic values use a 16-byte `{tag, flags, payload}` slot. `nil`, `bool`, `int64`, and `char` are inline; strings and bytes point to their owned buffers; BigInteger and BigDecimal point directly to typed native allocations. Arrays, objects, closures, class handles, and other graph-managed values still use a rooted `ZeusBox`.
- `&bytes` uses a by-value `{ptr, i64}` raw range. Its pointer addresses the first byte and its length is explicit; it does not read a header or retain the source allocation.
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

## Native type ID declarations

`@native decl Name = ...;` declares a compile-time native ABI type alias. Import
the compiler's fixed `ctypes` constants to use names, or supply a fixed numeric
ID:

```zeus
import zeus.ctypes [&ctypes];

@native decl Double = ctypes.DOUBLE;
@native decl Count = ctypes.LONGLONG;
@native decl Int32 = ctypes.INT32;
@native decl Int8 = ctypes.INT8;
@native decl UInt64 = ctypes.UINT64;
@native decl Float = ctypes.FLOAT;
@native decl RawPointer = ctypes.PTR;
@native decl AnotherDouble = 1;

func add(left: Double, right: AnotherDouble) -> Double {
    return left + right;
}

var value: Double = add(1.25, 2.5);
print(value);
```

The compiler owns these IDs; reordering or extending a source module does not
change them. An ordinary `import zeus.ctypes;` also allows these constants
without a namespace prefix.

| Constant | ID | Source type | C++ / runtime ABI | LLVM type |
| --- | ---: | --- | --- | --- |
| `DOUBLE` | 1 | `@native.double` | `double` | `double` |
| `LONGLONG` | 2 | `@native.longlong` | `int64_t` | `i64` |
| `INT32` | 3 | `@native.int32` | `int32_t` | `i32` |
| `INT8` | 4 | `@native.int8` | `int8_t` | `i8` |
| `INTEGER` | 5 | `int` | `int64_t` | `i64` |
| `BOOL` | 6 | `bool` | `bool` | `i1` |
| `PTR` | 7 | `ptr` | `void*` | `i64` |
| `STRING` | 8 | `string` | `ZeusNativeString` | `ptr` |
| `BYTES` | 9 | `bytes` | `ZeusNativeBytes` | `ptr` |
| `RAW_BYTES_RANGE` | 10 | `&bytes` | `ZeusRawBytesRange` | `%ZeusRawBytesRange` |
| `FLOAT` | 11 | `@native.float32` | `float` | `float` |
| `INT16` | 12 | `@native.int16` | `int16_t` | `i16` |
| `UINT8` | 13 | `@native.uint8` | `uint8_t` | `i8` |
| `UINT16` | 14 | `@native.uint16` | `uint16_t` | `i16` |
| `UINT32` | 15 | `@native.uint32` | `uint32_t` | `i32` |
| `UINT64` | 16 | `@native.uint64` | `uint64_t` | `i64` |

The table contains native ABI types with a concrete C++ representation.
`STRING` and `BYTES` use the length-prefixed Zeus-owned bridge formats described
above. `RAW_BYTES_RANGE` expands to the existing `&bytes` syntax. The pointer ID
names a raw address and follows the same pointer handling as `ptr`/`void*`.
Zeus runtime types such as `bigint`, decimal `float`, `nil`, `Array`, `dict`,
`object`, and `func` remain compiler/runtime ABI types and do not have native
type IDs.

Aliases use the same ABI as their table entry; the emitted call does not perform
a runtime `ctypes` lookup. Integer aliases accept `BigInt` values and exact
integer `BigDecimal` values, check the declared signed or unsigned width, and
normalize storage to an integer. `FLOAT` and `DOUBLE` accept `BigInt` and
`BigDecimal` values and round to binary32 and binary64 respectively. VM dynamic
DLL imports use the Windows x64 calling convention for mixed integer/float
signatures; LLVM/AOT emits typed calls using the platform ABI.

The Windows x64 convention hands out argument slots by position: the n-th
argument takes the n-th slot, and the argument's class decides whether that
slot is one of `RCX`/`RDX`/`R8`/`R9` or one of `XMM0`-`XMM3`. The first four
slots are registers and the rest are spilled onto the stack in argument order.
The VM trampoline therefore receives the finished placement instead of the
argument kinds, so a mixed signature such as `f(double, int64_t, double)`
lands in `XMM0`, `RDX`, and `XMM2` rather than in one register bank per class.

The table does not define universal Zeus class IDs or arbitrary C++ layout.
User-defined Zeus classes and `&T` references retain their compiler-generated
identity and pointer rules. Function values retain their internal dynamic ABI,
but `func` cannot currently be written as an explicit type annotation and has
no ID.

Declarations belong at module scope and can be exported with `using {Name};`.
Importing that module exposes its exported aliases, including through a
namespace import such as `import numbers [&types];` and `types.Double`.
Unknown IDs, unknown constants, runtime expressions, and duplicate alias names
are compile errors. `long double`, `char`/`wchar_t`, C/C++ enums, and arbitrary
struct/union or class layouts do not have native type IDs or general ABI
support. Zeus `int` uses the C++ `int64_t` ABI; C++ `int` remains a separate
32-bit type and should use the explicit `INT32` ID. `std::string` and ordinary
NUL-terminated C strings also do not match the `ZeusNativeString` layout.

For example, this Zeus declaration links directly to a C++ `double` function:

```zeus
@native decl Double = 1;
@native("native_add_double")
decl func nativeAdd(left: Double, right: Double) -> Double;
```

```cpp
extern "C" double native_add_double(double left, double right) {
    return left + right;
}
```

`ZeusZTest/run_native_decl_test.py` checks VM behavior, emitted ABI signatures,
module imports, and invalid declarations. Its optional `--aot-compiler` and
`--native-object` arguments also run the scalar tests and real C++ ABI calls;
compile `ZeusZTest/native_decl_abi_test.cpp` to supply that object.

`ZeusZTest/run_native_decl_range_test.py` checks legal boundaries, out-of-range
initialization and assignment, exact integer decimals, and fractional rejection
for signed and unsigned native integer widths. It covers global, branch, and
function scopes. Pass `--aot-compiler zeusc.bat` to check the same cases in AOT.

`ZeusZTest/run_native_import_double_test.py` builds a temporary Windows DLL and
checks mixed integer/`FLOAT`/`DOUBLE` register and stack arguments, native
integer and floating-point returns, `UINT64_MAX`, and BigInt/BigDecimal
conversion. Pass `--aot-compiler zeusc.bat` to run the same DLL-import fixture
through AOT.
