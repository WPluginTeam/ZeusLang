"""Build and run the three socket worker scenarios plus their combined AOT case."""
import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import tempfile
import shutil

from run_socket_winsock_test import ROOT, TESTS, check


@contextmanager
def artifact_directory(work_dir):
    directory = work_dir.resolve() if work_dir else Path(tempfile.mkdtemp(prefix="zeus-socket-aot-"))
    directory.mkdir(parents=True, exist_ok=True)
    try:
        yield directory
    except BaseException:
        print(f"AOT artifacts preserved: {directory}")
        raise
    else:
        if not work_dir:
            if (directory.parent.resolve() != Path(tempfile.gettempdir()).resolve()
                    or not directory.name.startswith("zeus-socket-aot-")):
                raise RuntimeError(f"unexpected temporary artifact directory: {directory}")
            shutil.rmtree(directory)


def run_aot_workers(compiler, timeout=30, build_timeout=180, repeat=1,
                    work_dir=None, cases_filter=None):
    if os.name != "nt":
        raise RuntimeError("the Winsock AOT compiler script requires Windows")
    compiler = compiler.absolute()
    source = (TESTS / "socket_cancel_test.zeu").read_text(encoding="utf-8-sig")
    entrypoint = "runAcceptCase(); // SOCKET_TEST_ENTRYPOINT"
    if source.count(entrypoint) != 1:
        raise RuntimeError("socket cancel fixture must have one test entrypoint")
    cases = [
        ("accept", "runAcceptCase();", ["SOCKET_CANCEL_ACCEPT_PASS"]),
        ("recv", "runRecvCase();", ["SOCKET_CANCEL_RECV_PASS"]),
        ("chunks", "runDelayedCase();", ["SOCKET_RECV_EXACT_DELAYED_PASS"]),
        ("combined", "runAcceptCase();\nrunRecvCase();\nrunDelayedCase();",
         ["SOCKET_CANCEL_ACCEPT_PASS", "SOCKET_CANCEL_RECV_PASS",
          "SOCKET_RECV_EXACT_DELAYED_PASS"]),
    ]
    if cases_filter:
        cases = [case for case in cases if case[0] in cases_filter]
    with artifact_directory(work_dir) as directory:
        built = []
        for name, invocation, markers in cases:
            path = Path(directory) / f"socket_workers_{name}.zeu"
            executable = path.with_suffix(".exe")
            path.write_text(source.replace(entrypoint, invocation), encoding="utf-8")
            # Every quoted argument is a local filesystem path. Windows paths
            # cannot contain a quote; explicit quoting also protects spaces.
            command = f'"{compiler}" "{path}" -o "{executable}"'
            try:
                result = subprocess.run(command, shell=True, cwd=ROOT,
                                        capture_output=True, timeout=build_timeout)
            except subprocess.TimeoutExpired as error:
                output = (error.stdout or b"") + (error.stderr or b"")
                path.with_suffix(".build.log").write_text(
                    output.decode("utf-8", errors="replace"), encoding="utf-8")
                raise RuntimeError(f"AOT build timed out: {name}\n"
                                   + output.decode("utf-8", errors="replace")) from error
            output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            path.with_suffix(".build.log").write_text(output, encoding="utf-8")
            if result.returncode != 0 or not executable.is_file():
                raise RuntimeError(f"AOT build failed: {name}, exit={result.returncode}\n"
                                   + output[-12000:])
            print(f"BUILT: socket AOT {name}")
            built.append((executable, markers))
        for iteration in range(repeat):
            for executable, markers in built:
                try:
                    check([str(executable)], markers, timeout)
                except RuntimeError as error:
                    executable.with_suffix(".run.log").write_text(str(error), encoding="utf-8")
                    raise
            if repeat > 1:
                print(f"PASS: socket AOT worker suite {iteration + 1}/{repeat}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "zeusc.bat")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--build-timeout", type=float, default=180)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--work-dir", type=Path, help="preserve sources, executables and build logs here")
    parser.add_argument("--cases", nargs="+", choices=["accept", "recv", "chunks", "combined"],
                        help="build and run selected scenarios (default: all)")
    args = parser.parse_args()
    if args.timeout <= 0 or args.build_timeout <= 0 or args.repeat <= 0:
        parser.error("timeouts and --repeat must be positive")
    run_aot_workers(args.compiler, args.timeout, args.build_timeout, args.repeat,
                    args.work_dir, args.cases)


if __name__ == "__main__":
    main()
