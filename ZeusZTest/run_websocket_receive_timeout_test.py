"""Check that WebSocketConnection.receive(timeout) reports a soft timeout.

The peer completes the handshake, stays silent for 400 ms, then answers. A read
that runs out of time has to raise WebSocketTimeout, keep the connection open,
and still deliver the frame that arrives afterwards.
"""

import argparse
import base64
import datetime
import hashlib
import ipaddress
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "websocket_receive_timeout_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
WEBSOCKET_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MARKERS = [
    "RECEIVE_TIMEOUT_TYPE WebSocketTimeout",
    "RECEIVE_TIMEOUT_MS",
    "LATE_FRAME after-timeout",
    "WEBSOCKET_RECEIVE_TIMEOUT_PASS",
]


def decode(data):
    return data.decode("mbcs", errors="replace")


def build_certificate(directory):
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
        .not_valid_after(now + datetime.timedelta(days=30))
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


def read_client_frame(stream):
    header = stream.read(2)
    if len(header) != 2:
        raise ConnectionError("incomplete WebSocket frame header")

    first, second = header
    length = second & 0x7F
    if length == 126:
        extended = stream.read(2)
        if len(extended) != 2:
            raise ConnectionError("incomplete extended WebSocket frame length")
        length = int.from_bytes(extended, "big")
    elif length == 127:
        extended = stream.read(8)
        if len(extended) != 8:
            raise ConnectionError("incomplete long WebSocket frame length")
        length = int.from_bytes(extended, "big")

    if not second & 0x80:
        raise ValueError("client WebSocket frame is not masked")
    mask = stream.read(4)
    payload = stream.read(length)
    if len(mask) != 4 or len(payload) != length:
        raise ConnectionError("incomplete masked WebSocket payload")
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


class SlowHandler(BaseHTTPRequestHandler):
    """Handshake, 400 ms of silence, one frame, then one more after each read."""

    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
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

        time.sleep(0.4)
        self.wfile.write(make_server_frame(1, b"after-timeout"))
        self.wfile.flush()

        read_client_frame(self.rfile)
        self.wfile.write(make_server_frame(1, b"second"))
        self.wfile.flush()

        read_client_frame(self.rfile)
        self.wfile.write(make_server_frame(8, b""))
        self.wfile.flush()


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, _request, _client_address):
        pass


def check(label, command, timeout, markers=MARKERS, env=None):
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, timeout=timeout, env=env
        )
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--vm-only",
        action="store_true",
        help="skip the AOT build and only run the interpreter",
    )
    arguments = parser.parse_args()

    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    with tempfile.TemporaryDirectory(prefix="zeus-receive-timeout-") as temp_dir:
        temp_path = Path(temp_dir)
        cert_path, key_path = build_certificate(temp_path)
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(cert_path), str(key_path))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)

        source = temp_path / "websocket_receive_timeout_test.zeu"
        template = SOURCE.read_text(encoding="utf-8-sig")
        source.write_text(
            template.replace("__PORT__", str(server.server_port)), encoding="utf-8"
        )

        server_thread.start()
        try:
            check("VM run", [str(ZEUS), str(source)], 120)
            if not arguments.vm_only:
                executable = temp_path / "websocket_receive_timeout_test.exe"
                env = os.environ.copy()
                env["TEMP"] = str(temp_path)
                env["TMP"] = str(temp_path)
                check(
                    "AOT build",
                    [ZEUSC_BATCH, "--opt-level", "2", "-o", str(executable), str(source)],
                    900,
                    markers=[],
                    env=env,
                )
                check("AOT run", [str(executable)], 120, env=env)
        finally:
            if server_thread.is_alive():
                server.shutdown()
                server_thread.join(timeout=5)
            server.server_close()


if __name__ == "__main__":
    main()
