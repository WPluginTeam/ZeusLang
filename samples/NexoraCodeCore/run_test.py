"""Compile and exercise the NexoraCodeCore sample against a local WSS peer."""

import argparse
import base64
import datetime
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer



ROOT = Path(__file__).resolve().parents[2]
SAMPLE = Path(__file__).resolve().parent
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
WEBSOCKET_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BENCHMARK_OPS = 5000
BENCHMARK_PAYLOAD = {
    "type": "rpc_result",
    "request_id": "req-42",
    "result": {"status": 200, "body": {"ok": True, "message": "Zeus JSON bench"}},
}
SERVER_FRAME = json.dumps(
    {
        "type": "rpc",
        "request_id": "req-42",
        "payload": {"path": "/api/config", "method": "GET"},
    },
    ensure_ascii=False,
    separators=(",", ":"),
).encode("utf-8")


def build_certificate(directory: Path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=2))
        .add_extension(
            x509.SubjectAlternativeName(
                [x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
            ),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )
    cert_path = directory / "server.pem"
    key_path = directory / "server.key"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    return cert_path, key_path


def read_exact(stream, length):
    output = bytearray()
    while len(output) < length:
        chunk = stream.read(length - len(output))
        if not chunk:
            raise ConnectionError("incomplete WebSocket frame")
        output.extend(chunk)
    return bytes(output)


def read_client_frame(stream):
    first, second = read_exact(stream, 2)
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(read_exact(stream, 2), "big")
    elif length == 127:
        length = int.from_bytes(read_exact(stream, 8), "big")
    if not second & 0x80:
        raise ValueError("client WebSocket frame is not masked")
    mask = read_exact(stream, 4)
    payload = read_exact(stream, length)
    decoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return first & 0x0F, decoded


def make_server_frame(opcode, payload):
    length = len(payload)
    if length < 126:
        header = bytes((0x80 | opcode, length))
    elif length <= 0xFFFF:
        header = bytes((0x80 | opcode, 126)) + length.to_bytes(2, "big")
    else:
        header = bytes((0x80 | opcode, 127)) + length.to_bytes(8, "big")
    return header + payload


def send_text(stream, text):
    stream.write(make_server_frame(1, text.encode("utf-8")))
    stream.flush()


class NexoraMockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        try:
            if self.path != "/ws/nexoracode":
                raise ValueError(f"unexpected WebSocket path: {self.path}")

            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(
                hashlib.sha1((key + WEBSOCKET_MAGIC).encode("ascii")).digest()
            ).decode("ascii")
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()

            opcode, payload = read_client_frame(self.rfile)
            if opcode != 1:
                raise ValueError("expected a text authentication frame")
            auth = json.loads(payload.decode("utf-8"))
            if auth != {
                "type": "auth",
                "device_id": "sample-device",
                "device_token": "sample-token",
            }:
                raise ValueError("authentication envelope did not match Nexora")
            send_text(self.wfile, '{"type":"auth_ok"}')

            opcode, payload = read_client_frame(self.rfile)
            if opcode != 1:
                raise ValueError(f"expected a text ping frame, got opcode {opcode}")
            ping = json.loads(payload.decode("utf-8"))
            if ping != {"type": "ping"}:
                observed_type = ping.get("type") if isinstance(ping, dict) else type(ping).__name__
                raise ValueError(f"expected a JSON ping envelope, got type {observed_type!r}")
            send_text(self.wfile, SERVER_FRAME.decode("utf-8"))

            opcode, payload = read_client_frame(self.rfile)
            if opcode != 1:
                raise ValueError("expected a text RPC result frame")
            rpc_result = json.loads(payload.decode("utf-8"))
            expected = {
                "type": "rpc_result",
                "request_id": "req-42",
                "result": {
                    "status": 200,
                    "body": {"ok": True, "message": "Zeus 通过"},
                },
            }
            if rpc_result != expected:
                raise ValueError(f"RPC result mismatch: {rpc_result!r}")

            opcode, payload = read_client_frame(self.rfile)
            if opcode != 8:
                raise ValueError("expected a WebSocket close frame")
            self.wfile.write(make_server_frame(8, payload))
            self.wfile.flush()
        except Exception as exc:
            self.server.failures.append(exc)


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.failures = []
        self.state = None

    def handle_error(self, _request, _client_address):
        pass


class ConnectionPeerState:
    """What the reconnecting peer observed across its connections."""

    def __init__(self):
        self.lock = threading.Lock()
        self.connections = 0
        self.received = []
        self.sent = []
        self.errors = []

    def next_connection(self):
        with self.lock:
            self.connections += 1
            return self.connections

    def record_received(self, index, opcode, payload):
        with self.lock:
            self.received.append((index, opcode, payload))

    def record_sent(self, index, opcode, payload):
        with self.lock:
            self.sent.append((index, opcode, payload))

    def fail(self, error):
        with self.lock:
            self.errors.append(error)


class ConnectionMockHandler(BaseHTTPRequestHandler):
    """Drops the first two connections and serves the third until the client closes."""

    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        state = self.server.state
        try:
            self._upgrade()
            index = state.next_connection()
            self.connection.settimeout(2.0)

            opcode, payload = read_client_frame(self.rfile)
            auth = json.loads(payload.decode("utf-8"))
            if auth.get("type") != "auth" or auth.get("device_id") != "sample-device":
                raise ValueError(f"unexpected auth envelope: {auth!r}")
            self._send_text(state, index, '{"type":"auth_ok"}')

            if index == 1:
                # Healthy connection: the client flushes its queued lifecycle
                # frame, then heartbeats until the test stops it.
                self._send_text(state, index, json.dumps({
                    "type": "rpc",
                    "request_id": "wire-1",
                    "payload": {"path": "/api/chat/queue", "method": "GET"},
                }, separators=(",", ":")))
                self._send_text(state, index, json.dumps({
                    "type": "task_subscribe",
                    "subscription_id": "0123456789abcdef0123456789abcdef",
                    "stream_id": "22222222222222222222222222222222",
                }, separators=(",", ":")))
                while True:
                    try:
                        opcode, payload = read_client_frame(self.rfile)
                    except Exception:
                        return
                    state.record_received(index, opcode, payload)
                    if opcode == 8:
                        self.wfile.write(make_server_frame(8, payload))
                        self.wfile.flush()
                        return
        except Exception as exc:
            state.fail(exc)

    def _upgrade(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(
            hashlib.sha1((key + WEBSOCKET_MAGIC).encode("ascii")).digest()
        ).decode("ascii")
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()

    def _send_text(self, state, index, text):
        payload = text.encode("utf-8")
        state.record_sent(index, 1, payload)
        self.wfile.write(make_server_frame(1, payload))
        self.wfile.flush()


SSE_EVENTS = [
    'data: {"n":1}\n\n',
    'data: {"n":2}\n\n',
    "data: [DONE]\n\n",
]


class SseMockHandler(BaseHTTPRequestHandler):
    """Plain-HTTP chunked SSE peer: two events, then [DONE], then close.

    The second event's chunk is split across two TCP writes with a short
    pause so the client must reassemble partial chunk framing.
    """

    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        try:
            if self.path != "/events":
                self.send_response(404, "Not Found")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            length = int(self.headers.get("Content-Length", 0) or 0)
            if length:
                self.rfile.read(length)

            self.send_response(200, "OK")
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.flush()

            for index, event in enumerate(SSE_EVENTS):
                payload = event.encode("utf-8")
                if index == 1:
                    half = len(payload) // 2
                    self.wfile.write(f"{len(payload):X}\r\n".encode("ascii"))
                    self.wfile.write(payload[:half])
                    self.wfile.flush()
                    time.sleep(0.05)
                    self.wfile.write(payload[half:] + b"\r\n")
                else:
                    self.wfile.write(f"{len(payload):X}\r\n".encode("ascii"))
                    self.wfile.write(payload + b"\r\n")
                self.wfile.flush()

            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except Exception as exc:
            self.server.failures.append(exc)


class BackendChatMockHandler(BaseHTTPRequestHandler):
    """OpenAI-compatible streaming peer for the standalone backend path."""

    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", 0) or 0)
            request_body = json.loads(self.rfile.read(length).decode("utf-8"))
            request_index = len(self.server.requests)
            self.server.requests.append({
                "path": self.path,
                "authorization": self.headers.get("Authorization", ""),
                "body": request_body,
            })
            if self.path != "/v1/chat/completions":
                raise ValueError(f"unexpected backend provider path: {self.path}")
            if request_body.get("model") != "test-model":
                raise ValueError(f"unexpected backend model: {request_body.get('model')!r}")

            request_messages = request_body.get("messages", [])
            if not request_messages or request_messages[0].get("role") != "system":
                raise ValueError("backend omitted its stable NexoraCode system prompt")
            if "NexoraCode" not in request_messages[0].get("content", ""):
                raise ValueError("backend sent the wrong default system prompt")
            project_blocks = [
                message for message in request_messages
                if message.get("role") == "system"
                and "## Project conventions" in message.get("content", "")
            ]
            if request_index == 7:
                if project_blocks:
                    raise ValueError("backend ignored the disabled AGENTS.md setting")
                custom_blocks = [
                    message for message in request_messages
                    if message.get("role") == "system"
                    and "## Custom instructions" in message.get("content", "")
                ]
                if len(custom_blocks) != 1 or "Custom settings fixture" not in custom_blocks[0].get("content", ""):
                    raise ValueError("backend omitted or duplicated the configured custom prompt")
            elif len(project_blocks) != 1 or "Project rule fixture" not in project_blocks[0].get("content", ""):
                raise ValueError("project instructions were missing or injected more than once")

            if request_index == 7:
                if not any(
                    message.get("role") == "user"
                    and "以下是此前对话的压缩摘要" in message.get("content", "")
                    for message in request_messages
                ):
                    raise ValueError("backend did not send the folded conversation summary")
                permission_messages = [
                    message for message in request_messages
                    if message.get("role") == "system"
                    and "当前本会话为完全权限模式" in message.get("content", "")
                ]
                if len(permission_messages) != 1:
                    raise ValueError("backend omitted or duplicated the active permission mode")
                events = [
                    {"choices": [{"index": 0, "delta": {"role": "assistant", "content": "Compression reply"}, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                     "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}},
                ]
            elif request_index == 0:
                tool_names = {
                    item.get("function", {}).get("name")
                    for item in request_body.get("tools", [])
                }
                required_tools = {
                    "local_file_read", "local_file_list", "local_file_search_tree",
                    "local_file_write", "local_shell_exec",
                }
                if not required_tools.issubset(tool_names):
                    raise ValueError(f"backend omitted registered tools: {tool_names!r}")
                events = [
                    {"choices": [{"index": 0, "delta": {"tool_calls": [{
                        "index": 0,
                        "id": "call_file_read",
                        "type": "function",
                        "function": {
                            "name": "local_file_read",
                            "arguments": "{\"path\":\"sample.txt\"}",
                        },
                    }]}, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                     "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}},
                ]
            elif request_index == 1:
                messages = request_body.get("messages", [])
                tool_results = [message for message in messages if message.get("role") == "tool"]
                if not any(
                    message.get("tool_call_id") == "call_file_read"
                    and "backend tool fixture" in message.get("content", "")
                    for message in tool_results
                ):
                    raise ValueError("backend did not feed the file tool result into the next round")
                events = [
                    {"choices": [{"index": 0, "delta": {"tool_calls": [{
                        "index": 0,
                        "id": "call_file_write",
                        "type": "function",
                        "function": {
                            "name": "local_file_write",
                            "arguments": "{\"path\":\"approved.txt\",\"content\":\"approved file body\"}",
                        },
                    }]}, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                     "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}},
                ]
            elif request_index == 3:
                events = [
                    {"choices": [{"index": 0, "delta": {"tool_calls": [{
                        "index": 0,
                        "id": "call_file_write_denied",
                        "type": "function",
                        "function": {
                            "name": "local_file_write",
                            "arguments": "{\"path\":\"denied.txt\",\"content\":\"must not exist\"}",
                        },
                    }]}, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                     "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}},
                ]
            elif request_index == 5:
                arguments = json.dumps({
                    "command": "echo approved-command>command-approved.txt",
                    "cwd": self.server.workspace,
                    "timeout": 5,
                }, separators=(",", ":"))
                events = [
                    {"choices": [{"index": 0, "delta": {"tool_calls": [{
                        "index": 0,
                        "id": "call_shell_exec",
                        "type": "function",
                        "function": {
                            "name": "local_shell_exec",
                            "arguments": arguments,
                        },
                    }]}, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                     "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}},
                ]
            else:
                messages = request_body.get("messages", [])
                tool_results = [message for message in messages if message.get("role") == "tool"]
                expected_call = "call_shell_exec" if request_index == 6 else (
                    "call_file_write_denied" if request_index == 4 else "call_file_write"
                )
                expected_content = "Exit code: 0" if request_index == 6 else (
                    "denied by user" if request_index == 4 else "Wrote"
                )
                if not any(
                    message.get("tool_call_id") == expected_call
                    and expected_content in message.get("content", "")
                    for message in tool_results
                ):
                    raise ValueError("resolved tool result was not sent in the continuation")
                reply = "Command reply" if request_index == 6 else (
                    "Denied reply" if request_index == 4 else "Backend reply"
                )
                events = [
                    {"choices": [{"index": 0, "delta": {"role": "assistant", "content": reply}, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                     "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7}},
                ]
            wire_events = [
                "data: " + json.dumps(event, separators=(",", ":")) + "\n\n"
                for event in events
            ] + ["data: [DONE]\n\n"]

            self.send_response(200, "OK")
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.flush()
            for event in wire_events:
                payload = event.encode("utf-8")
                self.wfile.write(f"{len(payload):X}\r\n".encode("ascii"))
                self.wfile.write(payload + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except Exception as exc:
            self.server.failures.append(exc)


def python_json_parse_benchmark():
    wire = json.dumps(BENCHMARK_PAYLOAD, ensure_ascii=False, separators=(",", ":"))
    for _ in range(100):
        json.loads(wire)
    started = time.perf_counter_ns()
    checksum = 0
    for _ in range(BENCHMARK_OPS):
        checksum += json.loads(wire)["result"]["status"]
    return time.perf_counter_ns() - started, checksum


def python_json_stringify_benchmark():
    wire = json.dumps(BENCHMARK_PAYLOAD, ensure_ascii=False, separators=(",", ":"))
    value = json.loads(wire)
    for _ in range(100):
        json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    started = time.perf_counter_ns()
    checksum = 0
    for _ in range(BENCHMARK_OPS):
        encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        checksum += len(encoded)
    return time.perf_counter_ns() - started, checksum



def check_json_benchmarks(stdout):
    parse_match = re.search(
        r"NEXORA_JSON_PARSE_BENCH_NS\s+(\d+)\s+OPS\s+(\d+)\s+CHECKSUM\s+(\d+)",
        stdout,
    )
    stringify_match = re.search(
        r"NEXORA_JSON_STRINGIFY_BENCH_NS\s+(\d+)\s+OPS\s+(\d+)\s+CHECKSUM\s+(\d+)",
        stdout,
    )
    if not parse_match or not stringify_match:
        raise AssertionError(f"JSON benchmark result is missing\n{stdout}")
    zeus_parse_ns, parse_ops, parse_checksum = map(int, parse_match.groups())
    zeus_stringify_ns, stringify_ops, stringify_checksum = map(
        int, stringify_match.groups()
    )
    python_parse_ns, python_parse_checksum = python_json_parse_benchmark()
    python_stringify_ns, python_stringify_checksum = python_json_stringify_benchmark()
    if (
        parse_ops != BENCHMARK_OPS
        or stringify_ops != BENCHMARK_OPS
        or parse_checksum != python_parse_checksum
        or stringify_checksum != python_stringify_checksum
    ):
        raise AssertionError(
            "Zeus and Python JSON benchmark checksums differ: "
            f"parse=({parse_ops},{parse_checksum},{python_parse_checksum}) "
            f"stringify=({stringify_ops},{stringify_checksum},{python_stringify_checksum})"
        )

    zeus_parse_ops_per_second = parse_ops * 1_000_000_000 / zeus_parse_ns
    python_parse_ops_per_second = BENCHMARK_OPS * 1_000_000_000 / python_parse_ns
    zeus_stringify_ops_per_second = stringify_ops * 1_000_000_000 / zeus_stringify_ns
    python_stringify_ops_per_second = (
        BENCHMARK_OPS * 1_000_000_000 / python_stringify_ns
    )
    print(
        "JSON_PARSE_BENCHMARK "
        f"ops={BENCHMARK_OPS} "
        f"zeus_ns={zeus_parse_ns} "
        f"python_ns={python_parse_ns} "
        f"zeus_ops_s={zeus_parse_ops_per_second:.0f} "
        f"python_ops_s={python_parse_ops_per_second:.0f} "
        f"python_to_zeus_ratio={python_parse_ops_per_second / zeus_parse_ops_per_second:.1f}x"
    )
    print(
        "JSON_STRINGIFY_BENCHMARK "
        f"ops={BENCHMARK_OPS} "
        f"zeus_ns={zeus_stringify_ns} "
        f"python_ns={python_stringify_ns} "
        f"zeus_ops_s={zeus_stringify_ops_per_second:.0f} "
        f"python_ops_s={python_stringify_ops_per_second:.0f} "
        f"python_to_zeus_ratio={python_stringify_ops_per_second / zeus_stringify_ops_per_second:.1f}x"
    )


def run_offline():
    if os.name != "nt":
        raise SystemExit("This sample currently requires the Windows Zeus toolchain.")

    with tempfile.TemporaryDirectory(prefix=".nexora-code-core-offline-", dir=SAMPLE) as temp_dir:
        temp_path = Path(temp_dir)
        source = temp_path / "test_offline.zeu"
        executable = temp_path / "test_offline.exe"
        ground = temp_path / "ground"
        ground.mkdir()
        invalid_utf8_root = ground / "invalid-utf8-fixture"
        invalid_utf8_root.mkdir()
        (invalid_utf8_root / "AGENTS.md").write_bytes(b"A\xffB\xe2\x82")
        source.write_text(
            (SAMPLE / "test_offline.zeu").read_text(encoding="utf-8-sig").replace(
                "__GROUND__", ground.as_posix()
            ),
            encoding="utf-8",
        )

        process_env = os.environ.copy()
        process_env["TEMP"] = str(temp_path)
        process_env["TMP"] = str(temp_path)
        compile_command = f'"{ZEUSC_BATCH}" --opt-level 2 -o "{executable}" "{source}"'
        compiled = subprocess.run(
            compile_command,
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
                f"zeusc offline sample failed ({compiled.returncode})\n"
                f"{compile_stdout}\n{compile_stderr}"
            )

        try:
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                env=process_env,
                capture_output=True,
                timeout=180,
            )
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout or b""
            stderr = error.stderr or b""
            if isinstance(stdout, bytes):
                stdout = stdout.decode("mbcs", errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode("mbcs", errors="replace")
            raise AssertionError(
                "NexoraCodeCore offline sample timed out after 180 seconds\n"
                f"{stdout}\n{stderr}"
            ) from error
        stdout = result.stdout.decode("mbcs", errors="replace")
        stderr = result.stderr.decode("mbcs", errors="replace")
        if (
            result.returncode != 0
            or "NEXORA_POLICY_JSON_TESTS_PASS" not in stdout
            or "NEXORA_TASK_EVENT_TESTS_PASS" not in stdout
            or "NEXORA_CONNECTION_STATE_TESTS_PASS" not in stdout
            or "NEXORA_RPC_DISPATCH_TESTS_PASS" not in stdout
            or "NEXORA_AGENT_EVENTS_TESTS_PASS" not in stdout
            or "NEXORA_TURN_INJECTION_TESTS_PASS" not in stdout
            or "NEXORA_PROVIDER_CONFIG_TESTS_PASS" not in stdout
            or "NEXORA_PROVIDER_STREAM_TESTS_PASS" not in stdout
            or "NEXORA_PROVIDER_MODELS_TESTS_PASS" not in stdout
            or "NEXORA_PERMISSION_SETTINGS_TESTS_PASS" not in stdout
            or "NEXORA_AGENT_LOOP_TESTS_PASS" not in stdout
            or "NEXORA_COMPRESSION_TESTS_PASS" not in stdout
            or "NEXORA_CONTEXT_TESTS_PASS" not in stdout
            or "NEXORA_TASK_STREAM_TESTS_PASS" not in stdout
            or "NEXORA_TASK_JOURNAL_TESTS_PASS" not in stdout
            or "NEXORA_STREAM_RUNTIME_TESTS_PASS" not in stdout
            or "NEXORA_BACKEND_API_TESTS_PASS" not in stdout
            or "NEXORA_AGENT_SETTINGS_TESTS_PASS" not in stdout
            or "NEXORA_PROJECT_INSTRUCTIONS_TESTS_PASS" not in stdout
            or "NEXORA_CONVERSATION_STORE_TESTS_PASS" not in stdout
            or "NEXORA_CONVERSATION_BUSINESS_TESTS_PASS" not in stdout
            or "NEXORA_TOOL_REGISTRY_TESTS_PASS" not in stdout
            or "NEXORA_CODE_CORE_OFFLINE_TEST_PASS" not in stdout
        ):
            raise AssertionError(
                f"NexoraCodeCore offline sample failed ({result.returncode})\n"
                f"{stdout}\n{stderr}"
            )

        check_json_benchmarks(stdout)
        print("NEXORA_CODE_CORE_OFFLINE_TEST_PASS")

def run():
    if os.name != "nt":
        raise SystemExit("This sample currently requires the Windows Schannel backend.")

    run_remote_slice()
    run_connection_slice()
    run_sse_slice()
    run_backend_chat_slice()


def run_remote_slice():
    with tempfile.TemporaryDirectory(prefix=".nexora-code-core-", dir=SAMPLE) as temp_dir:
        temp_path = Path(temp_dir)
        cert_path, key_path = build_certificate(temp_path)
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), NexoraMockHandler)
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.minimum_version = ssl.TLSVersion.TLSv1_2
        tls_context.load_cert_chain(str(cert_path), str(key_path))
        server.socket = tls_context.wrap_socket(server.socket, server_side=True)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)

        source = temp_path / "test_core.zeu"
        executable = temp_path / "test_core.exe"
        template = (SAMPLE / "test_core.zeu").read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PORT__", str(server.server_port)), encoding="utf-8"
        )

        process_env = os.environ.copy()
        process_env["TEMP"] = str(temp_path)
        process_env["TMP"] = str(temp_path)
        try:
            compile_command = (
                f'"{ZEUSC_BATCH}" --opt-level 2 -o "{executable}" "{source}"'
            )
            compiled = subprocess.run(
                compile_command,
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
                    f"zeusc failed ({compiled.returncode})\n"
                    f"{compile_stdout}\n{compile_stderr}"
                )

            server_thread.start()
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                env=process_env,
                capture_output=True,
                timeout=60,
            )
            stdout = result.stdout.decode("mbcs", errors="replace")
            stderr = result.stderr.decode("mbcs", errors="replace")
            if server.failures:
                raise AssertionError(f"mock Nexora peer failed: {server.failures[0]}\n{stdout}\n{stderr}")
            if result.returncode != 0 or "NEXORA_CODE_CORE_TEST_PASS" not in stdout:
                raise AssertionError(
                    f"NexoraCodeCore sample failed ({result.returncode})\n"
                    f"{stdout}\n{stderr}"
                )

            print("NEXORA_CODE_CORE_TEST_PASS")
        finally:
            if server_thread.is_alive():
                server.shutdown()
                server_thread.join(timeout=5)
            server.server_close()


def check_connection_wire(state, stdout):
    if state.errors:
        raise AssertionError(f"reconnecting peer failed: {state.errors[0]}")

    if state.connections < 1:
        raise AssertionError("the peer never accepted a connection")

    if "NEXORA_WIRE_ATTEMPTS 1" not in stdout:
        raise AssertionError(f"the client did not keep a single healthy session\n{stdout}")

    first_frames = [
        payload for index, opcode, payload in state.received
        if index == 1 and opcode == 1
    ]
    if not first_frames:
        raise AssertionError("the first connection carried no text frame")
    first = json.loads(first_frames[0].decode("utf-8"))
    if first.get("type") != "task_lifecycle" or first.get("stream_id") != "stream-wire":
        raise AssertionError(
            f"the first frame was not the queued lifecycle event: {first!r}"
        )

    pings = 0
    for _index, opcode, payload in state.received:
        if opcode != 1:
            continue
        if json.loads(payload.decode("utf-8")).get("type") == "ping":
            pings += 1
    if pings < 2:
        raise AssertionError(f"only {pings} heartbeat frames arrived\n{stdout}")

    answers = []
    for _index, opcode, payload in state.received:
        if opcode != 1:
            continue
        frame = json.loads(payload.decode("utf-8"))
        if frame.get("type") == "rpc_result":
            answers.append(frame)
    if not answers:
        raise AssertionError(f"the client never answered the rpc frame\n{stdout}")
    answer = answers[0]
    if answer.get("request_id") != "wire-1":
        raise AssertionError(f"the rpc_result lost the request id: {answer!r}\n{stdout}")
    result = answer.get("result") or {}
    if result.get("status") != 200:
        raise AssertionError(f"the rpc_result status is not 200: {answer!r}\n{stdout}")
    if (result.get("body") or {}).get("path") != "/api/chat/queue":
        raise AssertionError(f"the host did not see the whitelisted path: {answer!r}\n{stdout}")

    stream_frames = []
    for _index, opcode, payload in state.received:
        if opcode != 1:
            continue
        frame = json.loads(payload.decode("utf-8"))
        if frame.get("type") in {"task_event", "task_state", "task_stream_gap", "task_stream_error"}:
            stream_frames.append(frame)
    events = [frame for frame in stream_frames if frame.get("type") == "task_event"]
    if len(events) != 2:
        raise AssertionError(f"expected two task_event frames, saw {len(events)}\n{stdout}")
    if [event.get("event", {}).get("text") for event in events] != ["第一段", "第二段"]:
        raise AssertionError(f"the task events lost their payloads: {events!r}\n{stdout}")
    if any(event.get("subscription_id") != "0123456789abcdef0123456789abcdef" for event in events):
        raise AssertionError(f"the task events lost the subscription id: {events!r}\n{stdout}")
    states = [frame for frame in stream_frames if frame.get("type") == "task_state"]
    if not states:
        raise AssertionError(f"the relay never closed with a task_state\n{stdout}")
    if (states[-1].get("session") or {}).get("status") != "finished":
        raise AssertionError(f"the closing state lost the session meta: {states[-1]!r}\n{stdout}")
    if "NEXORA_WIRE_SUBSCRIPTIONS 1" not in stdout:
        raise AssertionError(f"the task_subscribe frame was not registered\n{stdout}")

    closes = [payload for _index, opcode, payload in state.received if opcode == 8]
    if not closes:
        raise AssertionError(f"the client never sent a close frame\n{stdout}")
    if closes[0][:2] != b"\x03\xe8":
        raise AssertionError(
            f"the close frame did not carry code 1000: {closes[0][:2]!r}\n{stdout}"
        )

    if "NEXORA_WIRE_RPC_CALLS 1" not in stdout:
        raise AssertionError(f"the rpc handler did not run\n{stdout}")


def run_connection_slice():
    with tempfile.TemporaryDirectory(
        prefix=".nexora-code-core-connection-", dir=SAMPLE
    ) as temp_dir:
        temp_path = Path(temp_dir)
        cert_path, key_path = build_certificate(temp_path)
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), ConnectionMockHandler)
        server.state = ConnectionPeerState()
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.minimum_version = ssl.TLSVersion.TLSv1_2
        tls_context.load_cert_chain(str(cert_path), str(key_path))
        server.socket = tls_context.wrap_socket(server.socket, server_side=True)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)

        source = temp_path / "test_connection.zeu"
        executable = temp_path / "test_connection.exe"
        template = (SAMPLE / "test_connection.zeu").read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PORT__", str(server.server_port)), encoding="utf-8"
        )

        process_env = os.environ.copy()
        process_env["TEMP"] = str(temp_path)
        process_env["TMP"] = str(temp_path)
        try:
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
                    f"zeusc failed ({compiled.returncode})\n"
                    f"{compile_stdout}\n{compile_stderr}"
                )

            server_thread.start()
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                env=process_env,
                capture_output=True,
                timeout=60,
            )
            stdout = result.stdout.decode("mbcs", errors="replace")
            stderr = result.stderr.decode("mbcs", errors="replace")
            if result.returncode != 0 or "NEXORA_CONNECTION_WIRE_TEST_PASS" not in stdout:
                raise AssertionError(
                    f"NexoraCodeCore connection sample failed ({result.returncode})\n"
                    f"{stdout}\n{stderr}"
                )

            check_connection_wire(server.state, stdout)
            print("NEXORA_CONNECTION_WIRE_TEST_PASS")
        finally:
            if server_thread.is_alive():
                server.shutdown()
                server_thread.join(timeout=5)
            server.server_close()


def run_sse_slice():
    """Exercise chunked SSE through ResponseStream against a local mock peer."""
    with tempfile.TemporaryDirectory(prefix=".nexora-code-core-sse-", dir=SAMPLE) as temp_dir:
        temp_path = Path(temp_dir)
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), SseMockHandler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)

        source = temp_path / "test_sse.zeu"
        executable = temp_path / "test_sse.exe"
        template = (SAMPLE / "test_sse.zeu").read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PORT__", str(server.server_port)), encoding="utf-8"
        )

        process_env = os.environ.copy()
        process_env["TEMP"] = str(temp_path)
        process_env["TMP"] = str(temp_path)
        try:
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
                    f"zeusc failed ({compiled.returncode})\n"
                    f"{compile_stdout}\n{compile_stderr}"
                )

            server_thread.start()
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                env=process_env,
                capture_output=True,
                timeout=60,
            )
            stdout = result.stdout.decode("mbcs", errors="replace")
            stderr = result.stderr.decode("mbcs", errors="replace")
            if server.failures:
                raise AssertionError(f"mock SSE peer failed: {server.failures[0]}\n{stdout}\n{stderr}")
            missing = [
                marker
                for marker in (
                    "NEXORA_SSE_TEST_PASS",
                    "NEXORA_SSE_CANCEL_TEST_PASS",
                    "NEXORA_SSE_MULTILINE_TEST_PASS",
                )
                if marker not in stdout
            ]
            if result.returncode != 0 or missing:
                raise AssertionError(
                    f"NexoraCodeCore SSE sample failed ({result.returncode}, missing={missing})\n"
                    f"{stdout}\n{stderr}"
                )

            print("NEXORA_SSE_TEST_PASS")
        finally:
            if server_thread.is_alive():
                server.shutdown()
                server_thread.join(timeout=5)
            server.server_close()


def run_backend_chat_slice():
    """Exercise the HTTP API -> task runtime -> Agent Loop -> provider path."""
    with tempfile.TemporaryDirectory(prefix=".nexora-code-core-backend-", dir=SAMPLE) as temp_dir:
        temp_path = Path(temp_dir)
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), BackendChatMockHandler)
        server.requests = []
        server.workspace = (temp_path / "data").as_posix()
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)

        source = temp_path / "backend_chat_test.zeu"
        executable = temp_path / "backend_chat_test.exe"
        data_root = (temp_path / "data").as_posix()
        template = (SAMPLE / "backend_chat_test.zeu").read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PORT__", str(server.server_port)).replace(
                "__DATA_ROOT__", data_root
            ),
            encoding="utf-8",
        )

        process_env = os.environ.copy()
        process_env["TEMP"] = str(temp_path)
        process_env["TMP"] = str(temp_path)
        try:
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
                    f"zeusc backend chat sample failed ({compiled.returncode})\n"
                    f"{compile_stdout}\n{compile_stderr}"
                )

            server_thread.start()
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                env=process_env,
                capture_output=True,
                timeout=60,
            )
            stdout = result.stdout.decode("mbcs", errors="replace")
            stderr = result.stderr.decode("mbcs", errors="replace")
            if server.failures:
                raise AssertionError(
                    f"mock backend provider failed: {server.failures[0]}\n{stdout}\n{stderr}"
                )
            if (
                result.returncode != 0
                or "NEXORA_BACKEND_CHAT_TEST_PASS" not in stdout
                or "NEXORA_BACKEND_LIFETIME_TEST_PASS" not in stdout
            ):
                raise AssertionError(
                    f"NexoraCodeCore backend chat failed ({result.returncode}, provider_requests={len(server.requests)})\n"
                    f"{stdout}\n{stderr}"
                )
            if len(server.requests) != 8:
                raise AssertionError(
                    f"expected eight provider requests across approval, denial, command, resume, and compression, got {len(server.requests)}"
                )
            for request in server.requests:
                if request["authorization"] != "Bearer backend-test-key":
                    raise AssertionError("backend did not apply provider authorization")
                if request["body"].get("stream") is not True:
                    raise AssertionError("backend provider request was not streaming")
            if server.requests[0]["body"].get("messages", [])[-1].get("content") != "hello from frontend":
                raise AssertionError("backend provider request omitted the user message")

            print("NEXORA_BACKEND_CHAT_TEST_PASS")
        finally:
            if server_thread.is_alive():
                server.shutdown()
                server_thread.join(timeout=5)
            server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--offline", action="store_true", help="run policy and JSON checks without opening sockets")
    arguments = parser.parse_args()
    if arguments.offline:
        run_offline()
    else:
        run()
