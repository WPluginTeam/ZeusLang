"""Build and run the AOT NativeString layout and ownership regression."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "native_string_aot_layout_test.zeu"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "NATIVE_STRING_LAYOUT_PASS"


def decode(data):
    return data.decode("mbcs", errors="replace")


def run(command, label, timeout, marker=None):
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, timeout=timeout
        )
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"") + (error.stderr or b"")
        raise AssertionError(
            f"{label} timed out after {timeout}s\n{decode(output)}"
        )

    output = decode(result.stdout) + decode(result.stderr)
    if result.returncode or (marker is not None and marker not in output):
        raise AssertionError(
            f"{label} failed ({result.returncode})\n{output[-8000:]}"
        )
    print(f"{label} passed")


def main():
    with tempfile.TemporaryDirectory(prefix="zeus-native-string-aot-") as temp_dir:
        executable = Path(temp_dir) / "native_string_aot.exe"
        command = f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        run(command, "AOT build", 600)
        run([str(executable)], "AOT run", 120, MARKER)

    print("AOT NativeString layout and ownership test passed")


if __name__ == "__main__":
    main()
