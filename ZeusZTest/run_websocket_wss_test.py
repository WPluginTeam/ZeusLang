"""Exercise the Zeus WSS client with a local TLS WebSocket echo server."""

import argparse
import base64
import datetime
import hashlib
import ipaddress
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
WEBSOCKET_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def build_certificate(directory: Path):
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


class WebSocketEchoHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        if self.path != "/echo":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

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
            raise ValueError("expected a text frame")
        self.wfile.write(make_server_frame(1, payload))
        self.wfile.flush()

        opcode, payload = read_client_frame(self.rfile)
        if opcode != 8:
            raise ValueError("expected a close frame")
        self.wfile.write(make_server_frame(8, payload))
        self.wfile.flush()


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, _request, _client_address):
        # A failed certificate check closes TLS before the HTTP handshake.
        pass


def run(opt_level="0"):
    if os.name != "nt":
        raise SystemExit("This loopback test requires the Windows Schannel backend.")

    server = QuietThreadingHTTPServer(("127.0.0.1", 0), WebSocketEchoHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)

    try:
        with tempfile.TemporaryDirectory(prefix="zeus-websocket-wss-") as temp_dir:
            temp_path = Path(temp_dir)
            cert_path, key_path = build_certificate(temp_path)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(cert_path), str(key_path))
            server.socket = context.wrap_socket(server.socket, server_side=True)
            server_thread.start()

            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as refused_socket:
                refused_socket.bind(("127.0.0.1", 0))
                refused_port = refused_socket.getsockname()[1]

            source = temp_path / "websocket_wss_test.zeu"
            executable = temp_path / "websocket_wss_test.exe"
            template = (ROOT / "ZeusZTest" / "websocket_wss_test.zeu").read_text(
                encoding="utf-8-sig"
            )
            source.write_text(
                template.replace("__PORT__", str(server.server_port)).replace(
                    "__REFUSED_PORT__", str(refused_port)
                ),
                encoding="utf-8",
            )

            compile_command = (
                f'"{ZEUSC_BATCH}" --keep-temps --opt-level {opt_level} '
                f'-o "{executable}" "{source}"'
            )
            compiled = subprocess.run(
                compile_command,
                cwd=ROOT,
                shell=True,
                capture_output=True,
                timeout=300,
            )
            stdout = compiled.stdout.decode("mbcs", errors="replace")
            stderr = compiled.stderr.decode("mbcs", errors="replace")
            if compiled.returncode != 0:
                raise AssertionError(
                    f"zeusc failed ({compiled.returncode})\n{stdout}\n{stderr}"
                )

            result = subprocess.run(
                [str(executable)], cwd=ROOT, capture_output=True, timeout=60
            )
            stdout = result.stdout.decode("mbcs", errors="replace")
            stderr = result.stderr.decode("mbcs", errors="replace")
            required_markers = (
                "WSS_INSECURE_CONNECT_FAILURE_TYPE Exception",
                "WSS_INSECURE_CONNECT_RETHROW_TYPE Exception",
                "RUN_WEBSOCKET_WSS_TEST_PASS",
            )
            missing = [marker for marker in required_markers if marker not in stdout]
            if result.returncode != 0 or missing:
                raise AssertionError(
                    f"WSS client test failed ({result.returncode}, missing={missing})\n"
                    f"{stdout}\n{stderr}"
                )

            print(stdout.strip())
    finally:
        if server_thread.is_alive():
            server.shutdown()
            server_thread.join(timeout=5)
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--opt-level", choices=("0", "1", "2", "3", "s", "z"), default="0"
    )
    run(parser.parse_args().opt_level)
