"""Check synchronous exception handling, including uncaught failures."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "Zeus.exe"


def run(source):
    result = subprocess.run([str(EXE), str(source)], cwd=ROOT,
                            capture_output=True, timeout=30)
    output = result.stdout.decode("utf-8", errors="replace")
    assert result.returncode == 0, (source, result.returncode, output)
    assert "DOUBLE-RELEASE" not in output, output
    return output


def main():
    output = run(ROOT / "ZeusZTest" / "sync_exception_test.zeu")
    assert "PASS = 11 FAIL = 0" in output, output
    assert "[Runtime Error" not in output, output
    with tempfile.TemporaryDirectory(prefix="zeus-sync-exception-") as directory:
        for name, body, error in [
            ("runtime", "let n = 4; n.toString();", "Cannot call method 'toString'"),
            ("explicit", 'throw "destructor failed";', "Uncaught exception:"),
        ]:
            source = Path(directory) / (name + ".zeu")
            source.write_text(
                "class Resource { using &this; func operator[~]() { " + body + " } }\n"
                'func main() { let d = new Resource(); delete d; _write("UNEXPECTED_CONTINUATION\\n"); }\n'
                "main();\n", encoding="utf-8")
            output = run(source)
            assert "[Runtime Error" in output and error in output, output
            assert "UNEXPECTED_CONTINUATION" not in [line.strip() for line in output.splitlines()], output
    print("sync exception regression: 11 assertions + 2 uncaught cases passed")


if __name__ == "__main__":
    main()
