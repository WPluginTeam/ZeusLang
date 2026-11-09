"""Check normal and uncaught-error AOT exit dependency ordering."""
import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def check(executable, expected_exit, timeout):
    result = subprocess.run([str(executable.resolve(strict=True))], cwd=ROOT,
                            capture_output=True, timeout=timeout)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    shared = output.find("SHARED_EXIT_DEPENDENCY_PASS")
    ordinary = output.find("ORDINARY_EXIT_DEPENDENCY_DESTROYED")
    if (result.returncode != expected_exit or shared < 0 or ordinary < 0 or shared > ordinary
            or output.count("SHARED_EXIT_DEPENDENCY_PASS") != 1
            or output.count("ORDINARY_EXIT_DEPENDENCY_DESTROYED") != 1):
        raise RuntimeError(f"exit finalization failed: {executable}, exit={result.returncode}\n{output}")
    print(f"PASS: {executable.name} / shared finalizer precedes ordinary dependency")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--normal-exe", type=Path, required=True)
    parser.add_argument("--error-exe", type=Path, required=True,
                        help="same source with final mode line replaced by an uncaught throw")
    parser.add_argument("--timeout", type=float, default=20)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    check(args.normal_exe, 0, args.timeout)
    check(args.error_exe, 1, args.timeout)


if __name__ == "__main__":
    main()
