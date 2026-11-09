"""Verify environment cleanup remains stable during interpreted destructor reentry."""
import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(__file__).with_name("vm_reentrant_environment_cleanup_test.zeu")
MARKER = "VM_REENTRANT_ENV_CLEANUP_PASS"


def check(command, timeout):
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = ((error.stdout or b"") + (error.stderr or b""))
        raise RuntimeError(
            f"reentrant cleanup test timed out: {command}\n"
            + output.decode("utf-8", errors="replace")
        ) from error
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    if result.returncode != 0 or MARKER not in output:
        raise RuntimeError(
            f"reentrant cleanup test failed: {command}\nexit={result.returncode}\n{output}"
        )
    print(f"PASS: {Path(command[0]).name} / {MARKER}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Zeus.exe")
    parser.add_argument("--aot-exe", type=Path, help="already built cleanup test executable")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    check([str(args.exe.resolve(strict=True)), str(SOURCE)], args.timeout)
    if args.aot_exe:
        check([str(args.aot_exe.resolve(strict=True))], args.timeout)


if __name__ == "__main__":
    main()
