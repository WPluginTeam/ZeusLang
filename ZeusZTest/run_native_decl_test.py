"""Verify native type IDs and ABI aliases in VM, LLVM, imports, and optional AOT."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCES = Path(__file__).resolve().parent
POSITIVE = {
    "native_decl_dynamic_error_test.zeu": "NATIVE_DECL_DYNAMIC_ERROR_PASS",
    "native_decl_test.zeu": "NATIVE_DECL_PASS",
    "native_decl_ctypes_import_test.zeu": "NATIVE_DECL_CTYPES_IMPORT_PASS",
    "native_decl_ctypes_static_value_test.zeu": "NATIVE_DECL_CTYPES_STATIC_VALUE_PASS",
    "native_decl_import_test.zeu": "NATIVE_DECL_IMPORT_PASS",
    "native_decl_namespace_test.zeu": "NATIVE_DECL_NAMESPACE_PASS",
    "native_decl_static_const_test.zeu": "NATIVE_DECL_STATIC_CONST_PASS",
    "native_decl_type_ids_test.zeu": "NATIVE_TYPE_IDS_PASS",
    "native_width_merge_test.zeu": "PASS native width control flow merge",
    "fixed_width_spellings_test.zeu": "FIXED_WIDTH_SPELLINGS_PASS",
}
NEGATIVE = {
    "zero_code": ("@native decl Invalid = 0;", "native type"),
    "unknown_code": ("@native decl Invalid = 99;", "native type"),
    "first_unassigned_code": ("@native decl Invalid = 17;", "native type"),
    "unknown_enum": (
        "import zeus.ctypes [&ctypes]; @native decl Invalid = ctypes.NO_SUCH_TYPE;",
        "native type",
    ),
    "zeus_object_has_no_native_id": (
        "import zeus.ctypes [&ctypes]; @native decl ObjectType = ctypes.OBJECT;",
        "native type",
    ),
    "duplicate_alias": ("@native decl Repeated = 1; @native decl Repeated = 1;", "duplicate native type"),
    "runtime_code": ("let code = 1; @native decl Invalid = code;", "static integer constant"),
    "function_local": ("func local() { @native decl Invalid = 1; }", "native type"),
    "double_string": ('@native decl Double = 1; var value: Double = "wrong";', None),
    "private_import": (
        "import native_decl_package; var value: HiddenDouble = 1.25;",
        None,
    ),
    "private_namespace_import": (
        "import native_decl_package [&types]; var value: types.HiddenDouble = 1.25;",
        None,
    ),
}
for internal_name in ("__i8", "__i16", "__i32", "__double"):
    NEGATIVE["internal_annotation_" + internal_name] = (
        f"var value: {internal_name} = 1;",
        "internal native type names are not source syntax",
    )
    NEGATIVE["internal_declaration_" + internal_name] = (
        f"@native decl Invalid = {internal_name};",
        "internal native type names are not source syntax",
    )


def run(command, *, shell=False, timeout=60):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, shell=shell, timeout=timeout)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    details = f"{command}\nexit={result.returncode}\n{output[-8000:]}"
    return result, output, details


def compile_aot(compiler, source, destination, native_object=None):
    command = [str(compiler), str(source), "-o", str(destination)]
    if native_object:
        command.extend(["--link", str(native_object)])
    if os.name == "nt":
        result, output, details = run(subprocess.list2cmdline(command), shell=True, timeout=300)
    else:
        result, output, details = run(command, timeout=300)
    assert result.returncode == 0, details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    parser.add_argument("--aot-compiler", type=Path, help="optionally build and run every positive case")
    parser.add_argument("--native-object", type=Path, help="object compiled from native_decl_abi_test.cpp")
    args = parser.parse_args()
    executable = args.executable.absolute()
    aot_compiler = args.aot_compiler.absolute() if args.aot_compiler else None
    native_object = args.native_object.absolute() if args.native_object else None
    for label, path in (("executable", executable), ("AOT compiler", aot_compiler),
                        ("native object", native_object)):
        if path is not None and not path.is_file():
            parser.error(f"{label} is not a file: {path}")
    if native_object and not aot_compiler:
        parser.error("--native-object requires --aot-compiler")

    with tempfile.TemporaryDirectory(prefix="native-decl-", dir=SOURCES) as directory:
        temporary = Path(directory)
        for name in (*POSITIVE, "native_decl_package.zeu", "native_decl_static_const_package.zeu",
                     "native_decl_abi_test.zeu"):
            shutil.copyfile(SOURCES / name, temporary / name)
        for name, marker in POSITIVE.items():
            source = temporary / name
            result, output, details = run([str(executable), str(source)])
            assert result.returncode == 0 and marker in output, details
            llvm_path = temporary / (name + ".ll")
            result, output, details = run([str(executable), "--emit-llvm", str(source), str(llvm_path)])
            assert result.returncode == 0 and llvm_path.is_file(), details
            if name == "native_decl_test.zeu":
                llvm = llvm_path.read_text(encoding="utf-8")
                assert re.search(r"define double [^\n]*doubleSum\(double [^,]+, double ", llvm), details
                assert re.search(r"define i64 [^\n]*integerSum\(i32 [^,]+, i8 ", llvm), details
            if name == "native_decl_type_ids_test.zeu":
                llvm = llvm_path.read_text(encoding="utf-8")
                expected_signatures = (
                    ("i64", "integerIdentity", "i64"),
                    ("i1", "boolIdentity", "i1"),
                    ("ptr", "stringIdentity", "ptr"),
                    ("ptr", "bytesIdentity", "ptr"),
                    ("%ZeusRawBytesRange", "rawBytesRangeIdentity", "%ZeusRawBytesRange"),
                )
                for result_type, function, argument_type in expected_signatures:
                    signature = rf"define {re.escape(result_type)} [^\n]*{function}\({re.escape(argument_type)} "
                    assert re.search(signature, llvm), f"missing type-ID ABI {signature}\n{details}"
                assert re.search(r'declare [^\n]*@"?unused_pointer"?\([^\n]*i64', llvm), details
            if aot_compiler:
                binary = temporary / (source.stem + ".exe")
                compile_aot(aot_compiler, source, binary)
                result, output, details = run([str(binary)])
                assert result.returncode == 0 and marker in output, details

        abi_source = temporary / "native_decl_abi_test.zeu"
        abi_llvm = temporary / "native_decl_abi_test.ll"
        result, output, details = run([str(executable), "--emit-llvm", str(abi_source), str(abi_llvm)])
        assert result.returncode == 0 and abi_llvm.is_file(), details
        llvm = abi_llvm.read_text(encoding="utf-8")
        for scalar, symbol in (("double", "double"), ("i64", "i64"), ("i32", "i32"), ("i8", "i8")):
            expected = rf'declare {scalar} @"?zeus_native_decl_add_{symbol}"?\({scalar}, {scalar}\)'
            assert re.search(expected, llvm), f"missing scalar ABI {expected}\n{details}"
        if native_object:
            binary = temporary / "native_decl_abi_test.exe"
            compile_aot(aot_compiler, abi_source, binary, native_object)
            result, output, details = run([str(binary)])
            assert result.returncode == 0 and "NATIVE_DECL_ABI_PASS" in output, details

        for name, (source_text, diagnostic) in NEGATIVE.items():
            source = temporary / (name + ".zeu")
            source.write_text(source_text + "\n", encoding="utf-8")
            for emit_llvm in (False, True):
                llvm_path = source.with_suffix(".ll")
                command = [str(executable)]
                if emit_llvm:
                    command.append("--emit-llvm")
                command.append(str(source))
                if emit_llvm:
                    command.append(str(llvm_path))
                result, output, details = run(command)
                assert result.returncode != 0, details
                if diagnostic:
                    assert diagnostic.lower() in output.lower(), details
                assert not llvm_path.exists(), details
    print("PASS: native type IDs, VM, LLVM ABI signatures, imports, and rejection checks")
    if native_object:
        print("PASS: AOT scalar behavior and real extern C double/i64/i32/i8 calls")


if __name__ == "__main__":
    main()
