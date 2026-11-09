"""Exercise the Zeus HTTPS client as an AOT program against a loopback TLS server.

The server presents a self-signed certificate. The client must accept it when
verification is disabled and report the untrusted root when verification is on.
"""

import datetime
import ipaddress
import os
from pathlib import Path
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

# Ports this suite reserves for loopback services; the cert names localhost.
ZEUS_SOURCE = r'''import zeus.requests [&requests];

func assertTls(condition, message: string) {
    if (!condition) throw new Exception("tls test failed: " + message);
}

let base = "https://127.0.0.1:__PORT__";

// An explicitly unverified context accepts whatever the wire presents: the
// handshake and the record walk still have to be flawless.
let answer = requests.get(base + "/zen", nil, nil, nil, 8, false, true);
assertTls(answer.status_code == 200, "insecure status");
assertTls(answer.ok(), "insecure ok flag");
assertTls(answer.text() == "zeus-tls-ok\n", "insecure body");

// The default context verifies the chain. A self-signed server has no root,
// so the call has to refuse it with a certificate error.
let rejected = false;
let reported = "";
try {
    requests.get(base + "/zen", nil, nil, nil, 8, nil, nil);
} catch (&error) {
    rejected = true;
    reported = error.getMessage();
}
assertTls(rejected, "verified call must reject a self-signed certificate");
assertTls(reported.index("untrusted root") >= 0, "certificate error identifies the untrusted root");

print("RUN_TLS_TEST_PASS");
'''


class ZenHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        if self.path != "/zen":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = b"zeus-tls-ok\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def build_certificate(directory: Path):
    """Self-signed localhost certificate. The SAN carries both spellings the
    client could resolve, so a hostname mismatch never preempts the chain
    question this suite is actually testing."""
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
                [
                    x509.DNSName("localhost"),
                    x509.IPAddress(ipaddress.ip_address("127.0.0.1")),
                ]
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


def run():
    if os.name != "nt":
        raise SystemExit("This loopback test requires the current Windows Winsock backend.")

    server = ThreadingHTTPServer(("127.0.0.1", 0), ZenHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="zeus-tls-") as temp_dir:
            temp_path = Path(temp_dir)
            cert_path, key_path = build_certificate(temp_path)

            # The server speaks TLS 1.2: the client pins its protocol mask to
            # one version, and 1.2 is the one the schannel record walk fully
            # supports today.
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(cert_path), str(key_path))
            server.socket = context.wrap_socket(server.socket, server_side=True)

            source = temp_path / "run_tls_test.zeu"
            executable = temp_path / "run_tls_test.exe"
            source.write_text(
                ZEUS_SOURCE.replace("__PORT__", str(server.server_port)),
                encoding="utf-8",
            )

            compile_command = (
                f'"{ZEUSC_BATCH}" --keep-temps '
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
            if result.returncode != 0 or "RUN_TLS_TEST_PASS" not in stdout:
                raise AssertionError(
                    f"TLS client test failed ({result.returncode})\n{stdout}\n{stderr}"
                )
            print(stdout.strip())
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)


if __name__ == "__main__":
    run()
