"""Check module export allowlists through VM and LLVM imports."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ZEUS = ROOT / "Zeus.exe"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))

MODULE_SOURCE = """\
let exportedValue = 41;
let hiddenValue = 99;

func privateHelper() {
    return 1;
}

func exportedFunction() {
    return privateHelper() + exportedValue;
}

class ExportedBox {
    func value() {
        return 7;
    }
}

class HiddenBox {}

using {exportedValue, exportedFunction, ExportedBox};
"""

LEGACY_MODULE_SOURCE = """\
let legacyValue = 23;
let hiddenValue = 23;

func legacyFunction() {
    return legacyValue + 1;
}
"""

GLOBAL_IMPORT_SOURCE = """\
import module_fixture;
let item = new ExportedBox();
if (exportedFunction() == 42) {
    if (exportedValue == 41) {
        print("EXPORT_ALLOWLIST_PASS");
    }
}
"""

NAMESPACE_IMPORT_SOURCE = """\
import module_fixture [&module];
let item = new module.ExportedBox();
if (module.exportedFunction() == 42) {
    if (module.exportedValue == 41) {
        print("EXPORT_ALLOWLIST_PASS");
    }
}
if (module.hiddenValue == 0) {
    print("PRIVATE_MEMBER_BLOCKED");
}
if (module.HiddenBox == 0) {
    print("PRIVATE_CLASS_BLOCKED");
}
"""

NAMESPACE_AOT_SOURCE = """\
import module_fixture [&module];
let item = new module.ExportedBox();
if (module.exportedFunction() == 42) {
    print("EXPORT_ALLOWLIST_PASS");
}
"""

LEGACY_IMPORT_SOURCE = """\
import module_fixture;
import legacy_fixture;
if (legacyFunction() == 24) {
    if (legacyValue == 23) {
        if (hiddenValue == 23) {
            print("LEGACY_GLOBAL_IMPORT_PASS");
        }
    }
}
"""

LEGACY_NAMESPACE_SOURCE = """\
import legacy_fixture [&legacy];
if (legacy.legacyFunction() == 24) {
    if (legacy.legacyValue == 23) {
        print("LEGACY_NAMESPACE_IMPORT_PASS");
    }
}
"""

PRIVATE_GLOBAL_SOURCE = """\
import module_fixture;
print(hiddenValue);
"""

PRIVATE_CLASS_SOURCE = """\
import module_fixture;
let item = new HiddenBox();
"""

INVALID_EXPORT_MODULE_SOURCE = """\
let presentValue = 1;
using {missingValue};
"""

INVALID_EXPORT_IMPORT_SOURCE = """\
import invalid_export_fixture;
"""


def decode_output(result):
    return (result.stdout + result.stderr).decode("utf-8", errors="replace")


def run_vm(source, expected_marker):
    result = subprocess.run(
        [str(ZEUS), str(source)],
        cwd=ROOT,
        capture_output=True,
        timeout=30,
    )
    output = decode_output(result)
    if result.returncode != 0 or expected_marker not in output:
        raise AssertionError(f"VM failed ({result.returncode})\n{output}")
    return output


def run_vm_rejecting_private_reference(source, name):
    result = subprocess.run(
        [str(ZEUS), str(source)],
        cwd=ROOT,
        capture_output=True,
        timeout=30,
    )
    output = decode_output(result)
    if result.returncode == 0 or "P0043" not in output or name not in output:
        raise AssertionError(f"private reference check failed unexpectedly\n{output}")


def run_aot(source, output_path):
    compile_command = f'"{ZEUSC_BATCH}" -o "{output_path}" "{source}"'
    compiled = subprocess.run(
        compile_command,
        cwd=ROOT,
        shell=True,
        capture_output=True,
        timeout=300,
    )
    compile_output = decode_output(compiled)
    if compiled.returncode != 0:
        raise AssertionError(
            f"zeusc failed ({compiled.returncode})\n{compile_output}"
        )

    result = subprocess.run(
        [str(output_path)],
        cwd=ROOT,
        capture_output=True,
        timeout=30,
    )
    output = decode_output(result)
    if result.returncode != 0 or "EXPORT_ALLOWLIST_PASS" not in output:
        raise AssertionError(f"AOT failed ({result.returncode})\n{output}")


def main():
    if os.name != "nt":
        raise SystemExit("This regression test requires the Windows toolchain.")

    with tempfile.TemporaryDirectory(
        prefix="module-export-", dir=ROOT / "ZeusZTest"
    ) as temporary:
        temp_dir = Path(temporary)
        module = temp_dir / "module_fixture.zeu"
        global_import = temp_dir / "global_import.zeu"
        namespace_import = temp_dir / "namespace_import.zeu"
        namespace_aot = temp_dir / "namespace_aot.zeu"
        legacy_module = temp_dir / "legacy_fixture.zeu"
        legacy_global_import = temp_dir / "legacy_global_import.zeu"
        legacy_namespace_import = temp_dir / "legacy_namespace_import.zeu"
        private_global = temp_dir / "private_global.zeu"
        private_class = temp_dir / "private_class.zeu"
        invalid_module = temp_dir / "invalid_export_fixture.zeu"
        invalid_import = temp_dir / "invalid_export_import.zeu"
        module.write_text(MODULE_SOURCE, encoding="utf-8")
        global_import.write_text(GLOBAL_IMPORT_SOURCE, encoding="utf-8")
        namespace_import.write_text(NAMESPACE_IMPORT_SOURCE, encoding="utf-8")
        namespace_aot.write_text(NAMESPACE_AOT_SOURCE, encoding="utf-8")
        legacy_module.write_text(LEGACY_MODULE_SOURCE, encoding="utf-8")
        legacy_global_import.write_text(LEGACY_IMPORT_SOURCE, encoding="utf-8")
        legacy_namespace_import.write_text(LEGACY_NAMESPACE_SOURCE, encoding="utf-8")
        private_global.write_text(PRIVATE_GLOBAL_SOURCE, encoding="utf-8")
        private_class.write_text(PRIVATE_CLASS_SOURCE, encoding="utf-8")
        invalid_module.write_text(INVALID_EXPORT_MODULE_SOURCE, encoding="utf-8")
        invalid_import.write_text(INVALID_EXPORT_IMPORT_SOURCE, encoding="utf-8")

        run_vm(global_import, "EXPORT_ALLOWLIST_PASS")
        namespace_output = run_vm(namespace_import, "EXPORT_ALLOWLIST_PASS")
        if "PRIVATE_MEMBER_BLOCKED" not in namespace_output or \
                "PRIVATE_CLASS_BLOCKED" not in namespace_output:
            raise AssertionError(
                f"VM namespace import exposed a private member\n{namespace_output}"
            )
        run_vm(legacy_global_import, "LEGACY_GLOBAL_IMPORT_PASS")
        run_vm(legacy_namespace_import, "LEGACY_NAMESPACE_IMPORT_PASS")
        run_vm_rejecting_private_reference(private_global, "hiddenValue")
        run_vm_rejecting_private_reference(private_class, "HiddenBox")
        invalid_result = subprocess.run(
            [str(ZEUS), str(invalid_import)],
            cwd=ROOT,
            capture_output=True,
            timeout=30,
        )
        invalid_output = decode_output(invalid_result)
        if invalid_result.returncode == 0 or "P0042" not in invalid_output:
            raise AssertionError(
                f"unknown export name was not rejected\n{invalid_output}"
            )
        run_aot(global_import, temp_dir / "global_import.exe")
        run_aot(namespace_aot, temp_dir / "namespace_import.exe")

    print("PASS: VM and LLVM imports honor module export allowlists")


if __name__ == "__main__":
    main()
