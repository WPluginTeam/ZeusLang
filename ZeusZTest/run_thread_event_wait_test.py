"""Check that Event.wait reports a timeout instead of waiting forever.

The wait used to ignore its timeout argument and spin on 1 ms sleeps until a
setter arrived. The regression pins both halves: false at the deadline with no
setter, true (and early) once a worker sets the event.
"""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "thread_event_wait_test.zeu"
ZEUS = ROOT / "Zeus.exe"
MARKERS = ["EVENT_TIMEOUT_MS", "EVENT_SIGNALLED_MS", "THREAD_EVENT_WAIT_PASS"]


def decode(data):
    return data.decode("mbcs", errors="replace")


def check(label, command, markers, timeout):
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"") + (error.stderr or b"")
        raise AssertionError(f"{label} timed out after {timeout}s\n{decode(output)}")
    output = decode(result.stdout) + decode(result.stderr)
    missing = [marker for marker in markers if marker not in output]
    if result.returncode or missing:
        raise AssertionError(
            f"{label} failed ({result.returncode}), missing={missing}\n{output[-8000:]}"
        )
    print(f"{label} passed")
    return output


def run_vm():
    check("VM run", [str(ZEUS), str(SOURCE)], MARKERS, 120)


def run_aot_emission():
    with tempfile.TemporaryDirectory(prefix="zeus-event-wait-") as temp_dir:
        llvm_file = Path(temp_dir) / "program.ll"
        check(
            "AOT emission",
            [str(ZEUS), "--emit-llvm", str(SOURCE), str(llvm_file)],
            [],
            300,
        )
        emitted = llvm_file.read_text(encoding="utf-8", errors="replace")
    if "__time_monotonic_ns" not in emitted:
        raise AssertionError("AOT output does not call the monotonic clock")
    print("AOT emission keeps the monotonic clock call")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")
    run_vm()
    run_aot_emission()
    print("thread event wait test passed")


if __name__ == "__main__":
    main()
