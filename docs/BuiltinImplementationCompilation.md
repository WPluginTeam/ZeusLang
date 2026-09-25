# Built-in Type Implementation Compilation

## Goal

Built-in type behavior is written once in Zeus source under `ZeusBuiltins/`.
The normal Zeus front end compiles that source to shared Zeus IR; the VM and
LLVM consume that IR through their own backends. Backend-specific storage and
ABI rules belong to lowering adapters, not duplicate implementations of
high-level methods in C++.

## Compilation flow

```text
BuiltinCompilation registry
        |
        +--> VM package plan --> package dependency loader --+
        |                                                   |
        +--> LLVM module plan --> module dependency loader --+--> Zeus source
                                                              --> shared Zeus IR
                                                              --> VM or LLVM backend
```

`ZeusBuiltins/BuiltinRegistry.hpp` owns the source-module catalog and built-in
type-to-module mappings. `ZeusBuiltins/BuiltinCompilation.hpp` turns that
registry into a target-specific implicit-import plan. The module loader decides
which sources are present; `ZeusPackage::compilePackage` and
`ZeusCompile::compile` remain the Zeus-to-IR compilation paths.

## Adding a Zeus-implemented built-in type

1. Add its source module to `implementationModules()` and specify which
   backends may implicitly compile it and whether its exports are unqualified.
2. Add the public type name, aliases, implementation package, value category,
   immutability, and inheritance policy to `typeImplementations()`.
3. Implement the type's behavior in its `.zeu` module. Keep unavoidable byte
   storage, allocation, and platform interop primitives in the runtime ABI.
4. Add backend lowering only for representation-specific work. Do not move
   high-level type methods into a backend emitter or a second C++ library.

## Current backend policy

The VM keeps its current default implementation-module set. LLVM also includes
the Zeus `String` source module in its built-in module plan, so its class and
methods go through the shared Zeus source compiler and LLVM lowering path.
Untyped parameters and dynamic operations continue to use `ZeusValue`; explicitly
typed `string` parameters and returns use the native `ptr` ABI and are bridged
to dynamic values at the boundary.

`NativeTypeABI.hpp` is the common LLVM representation table. It currently maps
`int`/`int32`/`int64` to `i64`, `bool` to `i1`, and `string`/`str` to a
`ZeusStringView*`. The view carries a UTF-8 data pointer and length, so embedded
NUL bytes remain valid. A C++ `decl func f(s: string) -> string` bridge receives
and returns `const ZeusStringView*`; `zeus_string_view_intern` or
`zeus_string_view_from_data` can make temporary data stable across the call
boundary. Dynamic types such as
`bigint`, `float`, containers, and `bytes` remain `ZeusValue` until they have a
native lowering. Bytes literals can still cross into that dynamic ABI without a
Byte object; the Byte source module remains VM-only until its container
operations have LLVM lowering. Native string views created from `ZeusValue` are
interned for the process lifetime, keeping class fields and call arguments
valid. High-level string methods remain in Zeus source.

`BuiltinRegistry.hpp` separately distinguishes the inheritable Zeus `String`
class from the immutable primitive `string` ABI type, and the `Byte` class from
the dynamic `bytes` value type. Literal wrapper policy is registered there and
shared by array, byte, and string literals. `using original` disables those
wrappers; it does not change numeric literal types or convert existing values.
