"""Check a module-level variable that shares a library function's name.

The declaration used to return the existing slot, so `let str = ...` wrote into
`zeus.string.str`'s global. The VM then called a string, and the AOT emitter saw
a function-typed global where the module expected its own value. A declaration
now retires the inherited name and takes a slot of its own.
"""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ZEUS = ROOT / "Zeus.exe"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
MARKER = "GLOBAL_SHADOW_PASS"

SHADOW_MODULE_SOURCE = """\
import zeus.json;

let str = "shadowed";
let count = 7;

func readShadowed() -> string {
    return str;
}

func readCount() -> int {
    return count;
}
"""

ENTRY_SOURCE = """\
import shadow_module;

if (readShadowed() == "shadowed") {
    if (readCount() == 7) {
        print("GLOBAL_SHADOW_PASS");
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
        prefix="global-shadow-", dir=ROOT / "ZeusZTest"
    ) as temporary:
        temp_dir = Path(temporary)
        module = temp_dir / "shadow_module.zeu"
        entry = temp_dir / "shadow_main.zeu"
        module.write_text(SHADOW_MODULE_SOURCE, encoding="utf-8")
        entry.write_text(ENTRY_SOURCE, encoding="utf-8")

        run_vm(entry)
        run_aot(entry, temp_dir / "shadow_main.exe", temp_dir)

    print("PASS: a module-level shadowing declaration keeps its own slot")


if __name__ == "__main__":
    main()
