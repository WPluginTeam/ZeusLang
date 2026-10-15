# Built-in Type Implementation Compilation

## Goal

Built-in type behavior is written once in Zeus source under `ZeusBuiltins/zeus/`.
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

`NativeTypeABI.hpp` is the common LLVM representation table. It maps `int`/`int32`/`int64` to `i64`, `bool` to `i1`, and `string`/`str` to `ptr` (`NativeString`). A NativeString pointer points at the first character in a block laid out as `[u64 byte length][UTF-8 bytes][NUL]`; the byte length is stored at `data - 8` and excludes the trailing NUL. Static AOT string slots store this data pointer. A C++ `decl func f(s: string) -> string` binding receives and returns the same pointer ABI (`ZeusNativeString` in the runtime headers).

A dynamic string slot owns a malloc buffer laid out as `[u64 byte length][UTF-8 bytes][NUL]`; the `ZeusValue` payload points at the first byte, and the length is read from `data - 8`. Decoding into the VM's dynamic `UniversalVariant` representation copies the bytes into `std::string`. Native `ZeusNativeString` values use the same data pointer layout; strings created by the native string ABI use a content-deduplicating pool whose blocks remain valid for the process lifetime. VM cells use `ZeusValueSlot`; the operand stack and container contents still use `UniversalVariant`.

A dynamic bytes slot owns a malloc buffer laid out as `[u64 byte length][bytes]`; the payload points at the first byte, and the length is read from `data - 8`. Decoding into the VM's dynamic `UniversalVariant` representation copies the bytes into `std::vector<uint8_t>`. Separately allocated native `bytes` values use the same layout and are released with `zeus_bytes_release`. The AOT and VM slot ABIs no longer use a separate view descriptor. Bytes literals can cross into the dynamic ABI without a `Byte` object; the `Byte` source module remains VM-only until its container operations have LLVM lowering. High-level string methods remain in Zeus source.

`BuiltinRegistry.hpp` separately distinguishes the inheritable Zeus `String`
class from the immutable primitive `string` ABI type, and the `Byte` class from
the dynamic `bytes` value type. Literal wrapper policy is registered there and
shared by array, byte, and string literals. `using original` disables those
wrappers; it does not change numeric literal types or convert existing values.
