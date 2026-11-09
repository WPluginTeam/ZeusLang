"""Regression: a module-scope try/catch whose body ends with `throw`.

The catch variable slot is only filled on the unwind path. When the catch block
ends with `throw` there is no CATCH_END to retire the slot, so the exit cleanup
used to release an uninitialised ZeusValue and the process died *after* printing
everything (`invalid value ownership flags`). The failure is a teardown crash, so
a single run is not enough evidence: run the AOT binary several times.
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
SOURCE = ROOT / "ZeusZTest" / "module_scope_throw_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKERS = ("SCOPE_FN 1", "SCOPE_TOP 2", "MODULE_SCOPE_THROW_PASS")
AOT_REPEATS = 5


def decode(data):
    return data.decode("mbcs", errors="replace")


def check(label, code, output):
    if code != 0:
        raise AssertionError(f"{label}: exited with {code}\n{output}")
    for marker in MARKERS:
        if marker not in output:
            raise AssertionError(f"{label}: missing '{marker}'\n{output}")
    print(f"PASS: {label}")


def run_vm():
    result = subprocess.run([str(ZEUS), str(SOURCE)], cwd=ROOT,
                            capture_output=True, timeout=180)
    check("Zeus.exe", result.returncode, decode(result.stdout) + decode(result.stderr))


def run_aot():
    with tempfile.TemporaryDirectory(prefix="zeus-module-scope-") as temp_dir:
        temp_path = Path(temp_dir)
        executable = temp_path / "module_scope_throw.exe"
        compiled = subprocess.run(
            f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"',
            cwd=ROOT, shell=True, capture_output=True, timeout=300)
        if compiled.returncode != 0:
            raise AssertionError(
                f"zeusc failed ({compiled.returncode})\n"
                f"{decode(compiled.stdout)}\n{decode(compiled.stderr)}")
        for attempt in range(1, AOT_REPEATS + 1):
            result = subprocess.run([str(executable)], cwd=ROOT,
                                    capture_output=True, timeout=120)
            check(f"AOT run {attempt}", result.returncode,
                  decode(result.stdout) + decode(result.stderr))


def main():
    run_vm()
    run_aot()
    print("MODULE_SCOPE_THROW_TEST_PASS")


if __name__ == "__main__":
    main()
