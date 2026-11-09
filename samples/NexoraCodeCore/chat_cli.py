"""Compile and run the NexoraCodeCore chat CLI smoke test.

Reads a real provider entry from NexoraCode's data/providers.json (API key
stays in that file; only the endpoint URL travels in the request), asks one
question with streaming disabled, and prints the model's reply.
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


def zeu_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def run(provider: str, question: str, system: str, providers_path: str):
    if os.name != "nt":
        raise SystemExit("This sample currently requires the Windows Zeus toolchain.")
    if not Path(providers_path).exists():
        raise SystemExit(f"providers file not found: {providers_path}")

    with tempfile.TemporaryDirectory(prefix=".nexora-chat-cli-", dir=SAMPLE) as temp_dir:
        temp_path = Path(temp_dir)
        source = temp_path / "chat_cli.zeu"
        executable = temp_path / "chat_cli.exe"
        template = (SAMPLE / "chat_cli.zeu").read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PROVIDERS__", zeu_escape(providers_path))
            .replace("__PROVIDER__", zeu_escape(provider))
            .replace("__SYSTEM__", zeu_escape(system))
            .replace("__QUESTION__", zeu_escape(question)),
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
                f"zeusc chat CLI failed ({compiled.returncode})\n"
                f"{compile_stdout}\n{compile_stderr}"
            )

        result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            env=process_env,
            capture_output=True,
            timeout=180,
        )
        stdout = result.stdout.decode("mbcs", errors="replace")
        stderr = result.stderr.decode("mbcs", errors="replace")
        print(stdout, end="")
        if result.returncode != 0 or "NEXORA_CHAT_CLI_PASS" not in stdout:
            raise AssertionError(
                f"NexoraCodeCore chat CLI failed ({result.returncode})\n"
                f"{stdout}\n{stderr}"
            )
        print("NEXORA_CHAT_CLI_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", default="USST", help="provider name in providers.json")
    parser.add_argument("--question", default="你好，请用一句话介绍你自己",
                        help="question to ask the model")
    parser.add_argument("--system", default="你是 NexoraCode 本地助手，用中文简短回答。",
                        help="system prompt")
    parser.add_argument("--providers", default=DEFAULT_PROVIDERS,
                        help="absolute path to NexoraCode data/providers.json")
    arguments = parser.parse_args()
    run(arguments.provider, arguments.question, arguments.system, arguments.providers)
