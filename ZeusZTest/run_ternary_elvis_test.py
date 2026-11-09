"""Run ternary and nil-coalescing coverage in the VM and AOT backends."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "ternary_elvis_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
PASS_MARKER = "TERNARY_ELVIS_PASS"


def decode(data):
    return data.decode("mbcs", errors="replace")


def verify(label, result, output):
    if result != 0 or PASS_MARKER not in output:
        raise AssertionError(f"{label} failed ({result})\n{output}")
    print(f"{label} passed")


def run_vm():
    result = subprocess.run(
        [str(ZEUS), str(SOURCE)],
        cwd=ROOT,
        capture_output=True,
        timeout=120,
    )
    return result.returncode, decode(result.stdout) + decode(result.stderr)


def run_aot():
    if os.name != "nt":
        raise SystemExit("This AOT regression test requires Windows.")
    with tempfile.TemporaryDirectory(prefix="zeus-ternary-elvis-") as temp_dir:
        executable = Path(temp_dir) / "ternary_elvis_test.exe"
        command = f'"{ZEUSC_BATCH}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        compiled = subprocess.run(
            command,
            cwd=ROOT,
            shell=True,
            capture_output=True,
            timeout=900,
        )
        compile_output = decode(compiled.stdout) + decode(compiled.stderr)
        if compiled.returncode != 0:
            raise AssertionError(
                f"zeusc failed ({compiled.returncode})\n{compile_output}"
            )
        result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        return result.returncode, output


def main():
    if not ZEUS.is_file():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")
    verify("VM ternary/Elvis", *run_vm())
    verify("AOT ternary/Elvis", *run_aot())
    print("ternary and Elvis regression test passed")


if __name__ == "__main__":
    main()
