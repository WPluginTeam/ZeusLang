"""Run local Winsock regressions with a hard deadline for blocked native calls."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TESTS = Path(__file__).resolve().parent


def check(command, markers, timeout):
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        output = ((error.stdout or b"") + (error.stderr or b""))
        print(output.decode("utf-8", errors="replace"), file=sys.stderr)
        raise RuntimeError(f"socket test timed out after {timeout}s: {command}") from error
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    missing = [marker for marker in markers if marker not in output]
    if result.returncode or missing:
        raise RuntimeError(
            f"socket test failed: {command}\nexit={result.returncode}, "
            f"missing={missing}\n{output[-12000:]}"
        )
    print(f"PASS: {Path(command[0]).name} / {', '.join(markers)}")


def run_suite(executable, aot_exe, timeout):
    exchange_markers = ["SOCKET_EXCHANGE_PASS 127.0.0.1",
                        "SOCKET_EXCHANGE_PASS localhost", "SOCKET_RECV_ERROR_PASS",
                        "SOCKET_WINSOCK_PASS"]
    check([executable, str(TESTS / "socket_winsock_test.zeu")],
          exchange_markers, timeout)
    source = (TESTS / "socket_cancel_test.zeu").read_text(encoding="utf-8-sig")
    entrypoint = "runAcceptCase(); // SOCKET_TEST_ENTRYPOINT"
    if source.count(entrypoint) != 1:
        raise RuntimeError("socket cancel fixture must have one test entrypoint")
    with tempfile.TemporaryDirectory(prefix="zeus-socket-cases-") as directory:
        for case, marker in (
            ("runAcceptCase", "SOCKET_CANCEL_ACCEPT_PASS"),
            ("runRecvCase", "SOCKET_CANCEL_RECV_PASS"),
            ("runDelayedCase", "SOCKET_RECV_EXACT_DELAYED_PASS"),
        ):
            path = Path(directory) / f"{case}.zeu"
            path.write_text(source.replace(entrypoint, f"{case}();"), encoding="utf-8")
            check([executable, str(path)], [marker], timeout)
        combined = Path(directory) / "combined_socket_workers.zeu"
        combined.write_text(source.replace(entrypoint,
            "runAcceptCase();\nrunRecvCase();\nrunDelayedCase();"), encoding="utf-8")
        check([executable, str(combined)],
              ["SOCKET_CANCEL_ACCEPT_PASS", "SOCKET_CANCEL_RECV_PASS",
               "SOCKET_RECV_EXACT_DELAYED_PASS"], timeout)
    if aot_exe:
        check([str(aot_exe.absolute())], exchange_markers, timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Zeus.exe")
    parser.add_argument("--aot-exe", type=Path,
                        help="already built socket_winsock_test executable")
    parser.add_argument("--aot-compiler", type=Path,
                        help="optional zeusc script to build and run all AOT worker cases")
    parser.add_argument("--build-timeout", type=float, default=180,
                        help="hard timeout in seconds for each optional AOT build")
    parser.add_argument("--timeout", type=float, default=30,
                        help="hard timeout in seconds for each test process")
    parser.add_argument("--repeat", type=int, default=1, help="repeat the entire suite")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.build_timeout <= 0:
        parser.error("--build-timeout must be positive")
    if args.repeat <= 0:
        parser.error("--repeat must be positive")
    executable = str(args.exe.absolute())
    for iteration in range(args.repeat):
        run_suite(executable, args.aot_exe, args.timeout)
        if args.repeat > 1:
            print(f"PASS: socket suite {iteration + 1}/{args.repeat}")
    if args.aot_compiler:
        from run_socket_aot_workers_test import run_aot_workers
        run_aot_workers(args.aot_compiler, args.timeout, args.build_timeout, args.repeat)


if __name__ == "__main__":
    main()
