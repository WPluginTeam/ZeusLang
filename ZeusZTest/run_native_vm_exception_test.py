"""Check native value errors, caller recovery, and uncaught VM diagnostics."""

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DECLARATION = "import zeus.ctypes [&ctypes];\n@native decl NativeByte = ctypes.INT8;\n"
UNCAUGHT_CASES = {
    "global_initialization": "var source: int = 128; var value: NativeByte = source;",
    "global_assignment": (
        "var source: int = 128; var value: NativeByte = 127; value = source;"
    ),
    "local_initialization": (
        "func fail() { let source: int = -129; let value: NativeByte = source; } fail();"
    ),
    "parameter": (
        "func accept(value: NativeByte) -> int { return value; }"
        "var source: int = 128; accept(source);"
    ),
    "return": (
        "func fail() -> NativeByte { let source: int = 128; return source; } fail();"
    ),
}


def run(executable, source):
    result = subprocess.run(
        [str(executable), str(source)], cwd=ROOT,
        capture_output=True, timeout=30,
    )
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    details = f"{source}\nexit={result.returncode}\n{output[-8000:]}"
    assert "DOUBLE-RELEASE" not in output, details
    return result, output, details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    executable = parser.parse_args().executable.absolute()
    if not executable.is_file():
        parser.error(f"Compiler executable not found: {executable}")

    result, output, details = run(executable, Path(__file__).with_name("native_vm_exception_test.zeu"))
    assert result.returncode == 0, details
    assert "NATIVE_VM_EXCEPTION_PASS" in output, details
    assert "[Runtime Error" not in output, details

    with tempfile.TemporaryDirectory(prefix="native-vm-errors-", dir=ROOT / "ZeusZTest") as directory:
        for name, body in UNCAUGHT_CASES.items():
            source = Path(directory) / f"{name}.zeu"
            source.write_text(
                DECLARATION + body + '\nprint("UNEXPECTED_CONTINUATION");\n',
                encoding="utf-8",
            )
            result, output, details = run(executable, source)
            assert result.returncode == 1, details
            assert "[Runtime Error" in output and "out of range" in output, details
            assert "UNEXPECTED_CONTINUATION" not in [line.strip() for line in output.splitlines()], details
    print("Native VM exceptions: 8 caught failures and 5 uncaught diagnostics passed")


if __name__ == "__main__":
    main()
