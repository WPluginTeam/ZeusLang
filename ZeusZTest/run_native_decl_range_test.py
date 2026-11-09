"""Check native signed and unsigned integer bounds in VM and optional AOT."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from run_native_decl_test import ROOT, SOURCES, compile_aot, run


PASS_MARKER = "NATIVE_DECL_RANGE_PASS"
CONTINUATION_MARKER = "UNEXPECTED_CONTINUATION"
SCALARS = {
    "i8": (4, -128, 127),
    "i16": (12, -32768, 32767),
    "i32": (3, -2147483648, 2147483647),
    "i64": (2, -9223372036854775808, 9223372036854775807),
    "u8": (13, 0, 255),
    "u16": (14, 0, 65535),
    "u32": (15, 0, 4294967295),
    "u64": (16, 0, 18446744073709551615),
}
BOX_FUNCTION = "func boxed(value) { return value; }\n"


def wrap_scope(body, scope):
    if scope == "global":
        return body
    if scope == "control_flow":
        return "if (true) {\n" + body + "}\n"
    if scope == "local":
        return "func target() {\n" + body + "}\ntarget();\n"
    raise ValueError(f"unknown test scope: {scope}")


def negative_cases():
    for scalar, (code, lower, upper) in SCALARS.items():
        for scope in ("global", "control_flow", "local"):
            for operation in ("init", "reassign"):
                for bound, invalid in (("lower", lower - 1), ("upper", upper + 1)):
                    for conversion in ("literal", "boxed"):
                        expression = str(invalid) if conversion == "literal" else f"boxed({invalid})"
                        if operation == "init":
                            body = f"var value: Narrow = {expression};\n"
                        else:
                            body = f"var value: Narrow = 0;\nvalue = {expression};\n"
                        body += f'print("{CONTINUATION_MARKER}");\n'
                        source = f"@native decl Narrow = {code};\n" + BOX_FUNCTION
                        source += wrap_scope(body, scope)
                        name = f"{scalar}_{scope}_{operation}_{bound}_{conversion}"
                        yield name, source


def positive_source():
    declarations = {
        "i8": "Int8",
        "i16": "Int16",
        "i32": "Int32",
        "i64": "Int64",
        "u8": "UInt8",
        "u16": "UInt16",
        "u32": "UInt32",
        "u64": "UInt64",
    }
    source = "".join(
        f"@native decl {alias} = {SCALARS[scalar][0]};\n"
        for scalar, alias in declarations.items()
    ) + BOX_FUNCTION
    for scope in ("global", "control_flow", "local"):
        body = ""
        for scalar, (_, lower, upper) in SCALARS.items():
            alias = declarations[scalar]
            prefix = scope + scalar if scope != "local" else scalar
            # Bounds pass through declarations, exact decimals, boxed values,
            # and reassignment to exercise the stored-cell normalization path.
            body += (
                f"var {prefix}Lower: {alias} = {lower};\n"
                f"var {prefix}Upper: {alias} = boxed({upper});\n"
                f"var {prefix}DecimalUpper: {alias} = boxed({upper}.0);\n"
                f"var {prefix}DirectDecimalUpper: {alias} = {upper}.0;\n"
                f'if ({prefix}Lower != {lower} or {prefix}Upper != {upper}) throw "legal {scalar} initialization";\n'
                f'if ({prefix}DecimalUpper != {upper} or {prefix}DirectDecimalUpper != {upper}) throw "integral BigDecimal conversion for {scalar}";\n'
                f"{prefix}Lower = boxed({upper});\n"
                f"{prefix}Upper = boxed({lower}.0);\n"
                f"{prefix}Upper = {lower};\n"
                f'if ({prefix}Lower != {upper} or {prefix}Upper != {lower}) throw "legal {scalar} assignment";\n'
            )
        source += wrap_scope(body, scope)
    return source + f'print("{PASS_MARKER}");\n'


def fractional_decimal_cases():
    for scalar, (code, _, _) in SCALARS.items():
        for expression in ("1.5", "boxed(1.5)"):
            yield scalar, expression, (
                f"@native decl Narrow = {code};\n" + BOX_FUNCTION +
                f"var value: Narrow = {expression};\n" +
                f'print("{CONTINUATION_MARKER}");\n'
            )


def check_rejection_result(result, output, details, backend):
    assert result.returncode == 1, f"{backend} did not report a normal range failure\n{details}"
    assert "out of range" in output.lower(), f"{backend} missed the range diagnostic\n{details}"
    assert CONTINUATION_MARKER not in output, f"{backend} continued after a rejected value\n{details}"


def check_rejection(command, backend):
    check_rejection_result(*run(command), backend)


def check_aot_rejection(compiler, source, binary):
    command = [str(compiler), str(source), "-o", str(binary)]
    if os.name == "nt":
        result, output, details = run(subprocess.list2cmdline(command), shell=True, timeout=300)
    else:
        result, output, details = run(command, timeout=300)
    if result.returncode != 0:
        check_rejection_result(result, output, details, "AOT compiler")
        assert re.search(r"error\[(?:type|llvm)\][^\n]*out of range", output, re.IGNORECASE), (
            f"AOT compiler did not issue a located range diagnostic\n{details}"
        )
        assert not binary.exists(), f"AOT compiler published a binary after rejection\n{details}"
        return
    assert binary.is_file(), f"AOT compiler did not create the executable\n{details}"
    check_rejection([str(binary)], "AOT")


def check_fractional_rejection(command, backend):
    result, output, details = run(command)
    assert result.returncode == 1, f"{backend} did not reject a fractional decimal\n{details}"
    assert "exact integer" in output.lower(), f"{backend} missed the exact-integer diagnostic\n{details}"
    assert CONTINUATION_MARKER not in output, f"{backend} continued after a fractional value\n{details}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    parser.add_argument("--aot-compiler", type=Path, help="also compile and run every case with AOT")
    parser.add_argument("--case", help="run one rejection case by its exact name, plus valid boundary checks")
    parser.add_argument("--scalar", choices=SCALARS, help="check one native integer width")
    args = parser.parse_args()
    executable = args.executable.absolute()
    aot_compiler = args.aot_compiler.absolute() if args.aot_compiler else None
    for label, path in (("executable", executable), ("AOT compiler", aot_compiler)):
        if path is not None and not path.is_file():
            parser.error(f"{label} is not a file: {path}")
    cases = list(negative_cases())
    if args.scalar:
        cases = [(name, source) for name, source in cases if name.startswith(args.scalar + "_")]
    if args.case:
        cases = [(name, source) for name, source in cases if name == args.case]
        if not cases:
            parser.error(f"unknown rejection case: {args.case}")

    with tempfile.TemporaryDirectory(prefix="native-decl-range-", dir=SOURCES) as directory:
        temporary = Path(directory)
        positive = temporary / "legal_bounds.zeu"
        positive.write_text(positive_source(), encoding="utf-8")
        result, output, details = run([str(executable), str(positive)])
        assert result.returncode == 0 and PASS_MARKER in output, details
        if aot_compiler:
            binary = temporary / "legal_bounds.exe"
            compile_aot(aot_compiler, positive, binary)
            result, output, details = run([str(binary)])
            assert result.returncode == 0 and PASS_MARKER in output, details

        decimal_cases = list(fractional_decimal_cases())
        for index, (scalar, expression, source) in enumerate(decimal_cases):
            path = temporary / f"fractional_{scalar}_{index}.zeu"
            path.write_text(source, encoding="utf-8")
            check_fractional_rejection([str(executable), str(path)], "VM")
            if aot_compiler:
                binary = temporary / f"fractional_{index}.exe"
                compile_aot(aot_compiler, path, binary)
                check_fractional_rejection([str(binary)], "AOT")

        for name, source in cases:
            path = temporary / (name + ".zeu")
            path.write_text(source, encoding="utf-8")
            check_rejection([str(executable), str(path)], "VM")
            if aot_compiler:
                binary = temporary / (name + ".exe")
                check_aot_rejection(aot_compiler, path, binary)
    backends = "VM and AOT" if aot_compiler else "VM"
    print(
        f"PASS: {backends} legal bounds, {len(cases)} native integer range rejections, "
        f"and {len(decimal_cases)} fractional decimal rejections"
    )


if __name__ == "__main__":
    main()
