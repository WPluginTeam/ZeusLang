"""Check that bytes() results can call their methods.

`zeus.byte` is auto-imported on a bytes literal trigger, so a program that only
calls `bytes(x)` never got the `impl bytes` type implementation: statically
inlined calls worked, dynamic dispatch reported "non-object value". The
regression pins both shapes plus slicing the converted value.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "bytes_method_dispatch_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "BYTES_METHOD_DISPATCH_PASS"


def decode(data):
    return data.decode("mbcs", errors="replace")


def run(command, label, timeout, marker=None):
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"") + (error.stderr or b"")
        raise AssertionError(f"{label} timed out after {timeout}s\n{decode(output)}")
    output = decode(result.stdout) + decode(result.stderr)
    missing = marker is not None and marker not in output
    if result.returncode or missing:
        raise AssertionError(f"{label} failed ({result.returncode})\n{output[-8000:]}")
    print(f"{label} passed")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    run([str(ZEUS), str(SOURCE)], "VM run", 120, MARKER)

    with tempfile.TemporaryDirectory(prefix="zeus-bytes-dispatch-") as temp_dir:
        executable = Path(temp_dir) / "bytes_method_dispatch.exe"
        command = f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        run(command, "AOT build", 300)
        run([str(executable)], "AOT run", 120, MARKER)

    print("bytes method dispatch test passed")


if __name__ == "__main__":
    main()
