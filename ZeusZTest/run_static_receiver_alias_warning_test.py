"""Check the P0044 warning for class/impl blocks without a receiver alias."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "static_receiver_alias_warning_test.zeu"
ZEUS = ROOT / "Zeus.exe"
WARNING_CODE = "P0044"
EXPECTED_METHODS = {"'noReceiverMethod'", "'implNoReceiver'"}


def decode(data):
    return data.decode("mbcs", errors="replace")


def compile_vm():
    result = subprocess.run(
        [str(ZEUS), str(SOURCE)],
        cwd=ROOT,
        capture_output=True,
        timeout=120,
    )
    return result.returncode, decode(result.stdout) + decode(result.stderr)


def compile_aot():
    with tempfile.TemporaryDirectory(prefix="zeus-static-alias-") as temp_dir:
        llvm_file = Path(temp_dir) / "program.ll"
        result = subprocess.run(
            [str(ZEUS), "--emit-llvm", str(SOURCE), str(llvm_file)],
            cwd=ROOT,
            capture_output=True,
            timeout=300,
        )
        return result.returncode, decode(result.stdout) + decode(result.stderr)


def check(label, returncode, output):
    if returncode != 0:
        raise AssertionError(f"{label} failed ({returncode})\n{output}")
    reported = [line for line in output.splitlines() if WARNING_CODE in line]
    if len(reported) != len(EXPECTED_METHODS):
        raise AssertionError(
            f"{label} reported {len(reported)} warnings, "
            f"expected {len(EXPECTED_METHODS)}\n{output}"
        )
    for line in reported:
        if not any(method in line for method in EXPECTED_METHODS):
            raise AssertionError(f"{label} warned about an unexpected method\n{output}")
        if "aliasedMethod" in line:
            raise AssertionError(
                f"{label} warned about a method that has a receiver alias\n{output}"
            )
    print(f"{label} reported the expected receiver alias warnings")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    check("VM compile", *compile_vm())
    check("AOT compile", *compile_aot())
    print("static receiver alias warning test passed")


if __name__ == "__main__":
    main()
