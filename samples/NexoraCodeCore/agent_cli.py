"""Compile and run the NexoraCodeCore streaming agent CLI smoke test.

Runs the real agent loop against a real provider entry from NexoraCode's
data/providers.json: streaming chat completions, one real tool (read_file,
sandboxed to a temporary work directory), and the round loop that feeds the
tool result back to the model. The API key stays in providers.json; only the
endpoint URL travels in the request.
"""

import argparse
import os
import sys
from pathlib import Path
import subprocess
import tempfile


if os.name == "nt":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


ROOT = Path(__file__).resolve().parents[2]
SAMPLE = Path(__file__).resolve().parent
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
DEFAULT_PROVIDERS = r"F:\Code\AI\ChatDB\NexoraCode\data\providers.json"
NOTE_TEXT = "NexoraCode 的关键词是：蓝鲸七号。"


def zeu_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def run(provider: str, question: str, system: str, providers_path: str,
        max_rounds: int, expect_tool: bool):
    if os.name != "nt":
        raise SystemExit("This sample currently requires the Windows Zeus toolchain.")
    if not Path(providers_path).exists():
        raise SystemExit(f"providers file not found: {providers_path}")

    with tempfile.TemporaryDirectory(prefix=".nexora-agent-cli-", dir=SAMPLE) as temp_dir:
        temp_path = Path(temp_dir)
        work = temp_path / "work"
        work.mkdir()
        (work / "note.txt").write_text(NOTE_TEXT, encoding="utf-8")

        source = temp_path / "agent_cli.zeu"
        executable = temp_path / "agent_cli.exe"
        template = (SAMPLE / "agent_cli.zeu").read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PROVIDERS__", zeu_escape(providers_path))
            .replace("__PROVIDER__", zeu_escape(provider))
            .replace("__SYSTEM__", zeu_escape(system))
            .replace("__QUESTION__", zeu_escape(question))
            .replace("__WORKDIR__", zeu_escape(work.as_posix()))
            .replace("__MAXROUNDS__", str(max_rounds)),
            encoding="utf-8",
        )

        process_env = os.environ.copy()
        process_env["TEMP"] = str(temp_path)
        process_env["TMP"] = str(temp_path)
        compiled = subprocess.run(
            f'"{ZEUSC_BATCH}" --opt-level 2 -o "{executable}" "{source}"',
            cwd=ROOT,
            env=process_env,
            shell=True,
            capture_output=True,
            timeout=300,
        )
        compile_stdout = compiled.stdout.decode("mbcs", errors="replace")
        compile_stderr = compiled.stderr.decode("mbcs", errors="replace")
        if compiled.returncode != 0:
            raise AssertionError(
                f"zeusc agent CLI failed ({compiled.returncode})\n"
                f"{compile_stdout}\n{compile_stderr}"
            )

        result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            env=process_env,
            capture_output=True,
            timeout=300,
        )
        stdout = result.stdout.decode("utf-8", errors="replace")
        stderr = result.stderr.decode("utf-8", errors="replace")
        print(stdout, end="")
        if stderr.strip():
            print(stderr, end="", file=sys.stderr)

        problems = []
        if result.returncode != 0:
            problems.append(f"exit code {result.returncode}")
        if "NEXORA_AGENT_CLI_PASS" not in stdout:
            problems.append("missing NEXORA_AGENT_CLI_PASS")
        if "status=done" not in stdout:
            problems.append("loop did not finish with status=done")
        if expect_tool and "NEXORA_AGENT_TOOL read_file" not in stdout:
            problems.append("the model never called read_file")
        if expect_tool and NOTE_TEXT not in stdout:
            problems.append("the tool output never reached the loop")
        rounds_seen = stdout.count("NEXORA_AGENT_ROUND_END")
        if rounds_seen < (2 if expect_tool else 1):
            problems.append(f"the loop reported {rounds_seen} round boundaries")
        if problems:
            raise AssertionError(
                "NexoraCodeCore agent CLI failed: "
                + "; ".join(problems)
                + f"\n{stdout}\n{stderr}"
            )
        print("NEXORA_AGENT_CLI_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", default="USST", help="provider name in providers.json")
    parser.add_argument("--question",
                        default="请调用 read_file 读取 note.txt，然后用一句话告诉我里面写的是什么。",
                        help="question to ask the model")
    parser.add_argument("--system", default="你是 NexoraCode 本地助手。需要读文件时必须调用工具，不要猜测。",
                        help="system prompt")
    parser.add_argument("--providers", default=DEFAULT_PROVIDERS,
                        help="absolute path to NexoraCode data/providers.json")
    parser.add_argument("--max-rounds", type=int, default=6,
                        help="round cap for the agent loop")
    parser.add_argument("--no-tool", dest="expect_tool", action="store_false",
                        help="do not require a tool call (plain chat roundtrip)")
    arguments = parser.parse_args()
    run(arguments.provider, arguments.question, arguments.system, arguments.providers,
        arguments.max_rounds, arguments.expect_tool)
