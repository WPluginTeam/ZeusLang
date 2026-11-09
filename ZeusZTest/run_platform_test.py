"""Verify zeus.platform reads the same platform data Python's platform module shows."""
from pathlib import Path
import platform
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(__file__).with_name("platform_test.zeu")
MARKER = "PLATFORM_TEST_PASS"

CHECKS = {
    "SYSTEM": platform.system(),
    "NODE": platform.node(),
    "RELEASE": platform.release(),
    "VERSION": platform.version(),
    "MACHINE": platform.machine(),
    "PROCESSOR": platform.processor(),
    "PLATFORM": platform.platform(),
}


def run_exe(exe, arguments, timeout):
    result = subprocess.run([str(exe.absolute())] + arguments, cwd=ROOT, capture_output=True, timeout=timeout)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    return result, output


def check(exe, timeout):
    result, output = run_exe(exe, [str(SOURCE)], timeout)
    if result.returncode != 0 or MARKER not in output:
        raise RuntimeError(f"platform test failed: {exe}\nexit={result.returncode}\n{output}")
    for name, expected in CHECKS.items():
        found = None
        for line in output.splitlines():
            if line.startswith(name + ": "):
                found = line[len(name) + 2:]
                break
        if found != expected:
            raise RuntimeError(
                f"platform mismatch for {name}:\n  zeus:   {found!r}\n  python: {expected!r}\n{output}"
            )
        print(f"PASS: {name} = {expected}")


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Zeus.exe")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    check(args.exe, args.timeout)


if __name__ == "__main__":
    main()
