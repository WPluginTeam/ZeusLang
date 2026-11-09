"""Verify receiver return inference and ambiguous-return diagnostics."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DIAGNOSTIC = "declare an explicit return type"
NEGATIVE_CASES = {
    "mixed_object": """class Mixed {
    using &this;
    func choose(flag: bool) {
        if (flag) { return this; }
        else { return new Mixed(); }
    }
}
let owner = new Mixed();
owner.choose(true);
""",
    "implicit_nil": """class Partial {
    using &this;
    func choose(flag: bool) {
        if (flag) { return this; }
    }
}
let owner = new Partial();
owner.choose(true);
""",
}
SCALAR_REFERENCE_CASE = """class ScalarReference {
    using &this;
    func invalid() -> &int { return this; }
}
let value = new ScalarReference();
value.invalid();
"""
SCALAR_REFERENCE_DIAGNOSTIC = "managed class or a class type parameter"


def run(executable, source, emit_llvm=False):
    command = [str(executable)]
    if emit_llvm:
        command.append("--emit-llvm")
    command.append(str(source))
    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=60)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    details = f"{' '.join(command)}\nexit={result.returncode}\n{output[-6000:]}"
    return result, output, details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="zeus-receiver-return-") as directory:
        temporary = Path(directory)
        positive = temporary / "receiver_return_inference_test.zeu"
        shutil.copyfile(Path(__file__).with_name(positive.name), positive)
        result, output, details = run(executable, positive)
        assert result.returncode == 0, details
        assert "RECEIVER_RETURN_INFERENCE_PASS" in output, details
        result, output, details = run(executable, positive, emit_llvm=True)
        assert result.returncode == 0, details
        llvm_path = positive.with_suffix(".zeu.ll")
        assert llvm_path.is_file(), details
        llvm = llvm_path.read_text(encoding="utf-8")
        for owner in ("ClassMethodReturn", "ImplMethodReturn"):
            expected = f"define ptr @zeus_{owner}__forward("
            assert expected in llvm, f"missing inferred reference return {expected}\n{details}"
        for name, source in NEGATIVE_CASES.items():
            path = temporary / f"{name}.zeu"
            path.write_text(source, encoding="utf-8")
            for emit_llvm in (False, True):
                result, output, details = run(executable, path, emit_llvm)
                assert result.returncode != 0, details
                assert DIAGNOSTIC in output, details
                assert not path.with_suffix(".zeu.ll").exists(), details
        scalar = temporary / "scalar_reference.zeu"
        scalar.write_text(SCALAR_REFERENCE_CASE, encoding="utf-8")
        result, output, details = run(executable, scalar, emit_llvm=True)
        assert result.returncode != 0, details
        assert SCALAR_REFERENCE_DIAGNOSTIC in output, details
        assert not scalar.with_suffix(".zeu.ll").exists(), details
    print("Receiver return inference: VM, LLVM emission, and 5 rejection checks passed")


if __name__ == "__main__":
    main()
