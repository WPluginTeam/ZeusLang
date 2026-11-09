"""Check a module can call a function it only sees through its import's imports.

The LLVM side registered just the module's direct imports in the symbol tables,
so a name that arrived through an import's own imports resolved through the
compile-time global slot. After the merge that slot named a different global,
and the emitter rejected the call with a classref callee. The VM never noticed
because it looks the name up in the runtime global table.
"""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ZEUS = ROOT / "Zeus.exe"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
MARKER = "TRANSITIVE_IMPORT_CALL_PASS"

HELPER_SOURCE = """\
let helperValue = 11;

func describeTag(value: string) -> string {
    return "helper:" + value;
}
"""

BRIDGE_SOURCE = """\
import transitive_helper;

func bridgeTag(value: string) -> string {
    return describeTag(value);
}
"""

MAIN_SOURCE = """\
import transitive_bridge;

if (bridgeTag("a") == "helper:a") {
    if (describeTag("b") == "helper:b") {
        if (helperValue == 11) {
            print("TRANSITIVE_IMPORT_CALL_PASS");
        }
    }
}
"""


def decode_output(result):
    return (result.stdout + result.stderr).decode("utf-8", errors="replace")


def run_vm(source):
    result = subprocess.run(
        [str(ZEUS), str(source)], cwd=ROOT, capture_output=True, timeout=60
    )
    output = decode_output(result)
    if result.returncode != 0 or MARKER not in output:
        raise AssertionError(f"VM run failed ({result.returncode})\n{output}")
    print("VM run passed")


def run_aot(source, output_path, work_dir):
    environment = os.environ.copy()
    environment["TEMP"] = str(work_dir)
    environment["TMP"] = str(work_dir)
    compiled = subprocess.run(
        [ZEUSC_BATCH, "--opt-level", "2", "-o", str(output_path), str(source)],
        cwd=ROOT,
        env=environment,
        shell=True,
        capture_output=True,
        timeout=900,
    )
    compile_output = decode_output(compiled)
    if compiled.returncode != 0:
        raise AssertionError(f"zeusc failed ({compiled.returncode})\n{compile_output}")

    result = subprocess.run(
        [str(output_path)], cwd=ROOT, env=environment, capture_output=True, timeout=60
    )
    output = decode_output(result)
    if result.returncode != 0 or MARKER not in output:
        raise AssertionError(f"AOT run failed ({result.returncode})\n{output}")
    print("AOT build and run passed")


def main():
    if os.name != "nt":
        raise SystemExit("This regression test requires the Windows toolchain.")
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    with tempfile.TemporaryDirectory(
        prefix="transitive-import-", dir=ROOT / "ZeusZTest"
    ) as temporary:
        temp_dir = Path(temporary)
        helper = temp_dir / "transitive_helper.zeu"
        bridge = temp_dir / "transitive_bridge.zeu"
        entry = temp_dir / "transitive_main.zeu"
        helper.write_text(HELPER_SOURCE, encoding="utf-8")
        bridge.write_text(BRIDGE_SOURCE, encoding="utf-8")
        entry.write_text(MAIN_SOURCE, encoding="utf-8")

        run_vm(entry)
        run_aot(entry, temp_dir / "transitive_main.exe", temp_dir)

    print("PASS: a call through an import's imports reaches the function")


if __name__ == "__main__":
    main()
