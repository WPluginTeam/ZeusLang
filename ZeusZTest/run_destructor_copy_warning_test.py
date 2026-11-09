"""Check the P0045 warning for a class that releases a resource in operator[~]
without declaring operator[@=].

Only the class that owns a destructor and no copy hook may be reported; a class
that binds (-> &T) or copies (-> T) explicitly silences it, and a class with no
destructor is never considered.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "destructor_copy_warning_test.zeu"
ZEUS = ROOT / "Zeus.exe"
WARNING_CODE = "P0045"
EXPECTED = "'OwnsResource'"
UNEXPECTED = ("'DeclaresBind'", "'DeclaresCopy'", "'PlainValue'")


def decode(data):
    return data.decode("mbcs", errors="replace")


def run(args, timeout):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=timeout)
    return result.returncode, decode(result.stdout) + decode(result.stderr)


def check(label, code, output):
    if code != 0:
        raise AssertionError(f"{label}: compiler exited with {code}\n{output}")
    lines = [line for line in output.splitlines() if WARNING_CODE in line]
    if len(lines) != 1:
        raise AssertionError(
            f"{label}: expected exactly one {WARNING_CODE} warning, saw {len(lines)}\n{output}"
        )
    line = lines[0]
    if EXPECTED not in line:
        raise AssertionError(f"{label}: warning does not name {EXPECTED}\n{line}")
    for name in UNEXPECTED:
        if name in line:
            raise AssertionError(f"{label}: warning must not name {name}\n{line}")
    if "operator[@=]" not in line:
        raise AssertionError(f"{label}: warning should point at operator[@=]\n{line}")
    print(f"PASS: {label} / {WARNING_CODE} {EXPECTED}")


def main():
    code, output = run([str(ZEUS), str(SOURCE)], 180)
    check("Zeus.exe", code, output)
    if "DESTRUCTOR_COPY_WARNING_TEST_DONE" not in output:
        raise AssertionError(f"sample did not run to completion\n{output}")

    with tempfile.TemporaryDirectory(prefix="zeus-dtor-warning-") as temp_dir:
        llvm_file = Path(temp_dir) / "program.ll"
        code, output = run([str(ZEUS), "--emit-llvm", str(SOURCE), str(llvm_file)], 300)
        check("--emit-llvm", code, output)

    print("DESTRUCTOR_COPY_WARNING_TEST_PASS")


if __name__ == "__main__":
    main()
