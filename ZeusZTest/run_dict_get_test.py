"""Check that dictionary literals support the `get` method.

`impl dict` in `ZeusBuiltins/zeus/dict.zeu` routes `d.get(key, fallback)`
to the `dict_get` free function, so a missing key returns the fallback
instead of throwing like `Dict.get` does. The regression pins literal,
variable, empty-literal, and untyped/typed-parameter receivers in both the
VM and the AOT build (static and dynamic member dispatch).
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "dict_get_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "DICT_GET_PASS"


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

    with tempfile.TemporaryDirectory(prefix="zeus-dict-get-") as temp_dir:
        executable = Path(temp_dir) / "dict_get.exe"
        command = f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        run(command, "AOT build", 300)
        run([str(executable)], "AOT run", 120, MARKER)

    print("dict get test passed")


if __name__ == "__main__":
    main()
