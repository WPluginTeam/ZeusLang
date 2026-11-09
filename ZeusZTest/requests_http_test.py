"""Compile and exercise the synchronous Zeus HTTP client against loopback."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


ROOT = Path(__file__).resolve().parents[1]
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))

ZEUS_SOURCE = r'''import zeus.requests [&requests];

func assertRequest(condition, message: string) {
    if (!condition) throw new Exception("requests test failed: " + message);
}

let base = "http://127.0.0.1:__PORT__";

let queried = requests.get(
    base + "/echo", {"q": "hello world", "page": 7}, {"X-Request": "query"}, nil, 2);
assertRequest(queried.status_code == 200 and queried.ok(), "GET status");
let queryData = queried.json();
assertRequest(queryData["method"] == "GET", "GET method");
assertRequest(queryData["query"]["q"][0] == "hello world", "query params");
assertRequest(queryData["query"]["page"][0] == "7", "numeric query param");
assertRequest(queryData["header"] == "query", "request header");
assertRequest(queried.getHeader("CONTENT-TYPE", "") == "application/json", "response header");

let headResult = requests.head(base + "/head", nil, nil, 2);
assertRequest(headResult.status_code == 200, "HEAD status");
assertRequest(headResult.getHeader("content-length", "") == "17", "HEAD headers");
assertRequest(headResult.content.length() == 0, "HEAD has no response body");

let getWithJson = requests.get(
    base + "/echo", nil, {"X-Request": "get-json"}, {"kind": "get"}, 2).json();
assertRequest(getWithJson["method"] == "GET", "GET JSON method");
assertRequest(getWithJson["body"]["kind"] == "get", "GET JSON argument position");
assertRequest(getWithJson["header"] == "get-json", "GET JSON request header");

let posted = requests.post(
    base + "/echo", nil, {"X-Request": "json"}, {"name": "Zeus", "count": 3}, 2);
let postData = posted.json();
assertRequest(postData["method"] == "POST", "POST method");
assertRequest(postData["body"]["name"] == "Zeus", "JSON request body");
assertRequest(postData["body"]["count"] == 3, "JSON number");
assertRequest(postData["header"] == "json", "POST request header");

let putResult = requests.put(base + "/echo", nil, nil, {"operation": "put"}, 2).json();
assertRequest(putResult["method"] == "PUT", "PUT method");
let patchResult = requests.patch(base + "/echo", nil, nil, {"operation": "patch"}, 2).json();
assertRequest(patchResult["method"] == "PATCH", "PATCH method");
let deleteResult = requests.deleteRequest(base + "/echo", nil, nil, nil, 2).json();
assertRequest(deleteResult["method"] == "DELETE", "DELETE method");

let chunked = requests.get(base + "/chunked", nil, nil, nil, 2);
assertRequest(chunked.text() == "chunked-response", "chunked response body");
let binary = requests.get(base + "/binary", nil, nil, nil, 2);
assertRequest(binary.content.length() == 3 and binary.content[2] == 255, "binary response body");
let closeDelimited = requests.get(base + "/close", nil, nil, nil, 2);
assertRequest(closeDelimited.text() == "close-body", "connection-close response body");

let timedOut = false;
try {
    requests.get(base + "/delay", nil, nil, nil, 0.05);
} catch (&error) {
    timedOut = true;
}
assertRequest(timedOut, "read timeout");

print("REQUESTS_HTTP_TEST_PASS");
'''


class RequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def _send_json(self, value):
        body = json.dumps(value, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _echo(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        body = json.loads(raw.decode("utf-8")) if raw else None
        parsed = urlsplit(self.path)
        self._send_json({
            "method": self.command,
            "path": parsed.path,
            "query": parse_qs(parsed.query),
            "header": self.headers.get("X-Request"),
            "body": body,
        })

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/delay":
            time.sleep(0.25)
            try:
                self._send_json({"late": True})
            except (BrokenPipeError, ConnectionAbortedError):
                pass
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"chunked-", b"response"):
                self.wfile.write(f"{len(part):X}\r\n".encode("ascii") + part + b"\r\n")
            self.wfile.write(b"0\r\nX-Test-Trailer: done\r\n\r\n")
        elif path == "/binary":
            body = bytes((0, 128, 255))
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/close":
            body = b"close-body"
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            self.wfile.write(body)
        else:
            self._echo()

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "17")
        self.end_headers()

    def do_POST(self):
        self._echo()

    def do_PUT(self):
        self._echo()

    def do_PATCH(self):
        self._echo()

    def do_DELETE(self):
        self._echo()


def run():
    if os.name != "nt":
        raise SystemExit("This loopback test requires the current Windows Winsock backend.")

    server = ThreadingHTTPServer(("127.0.0.1", 0), RequestHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="zeus-requests-") as temp_dir:
            temp_path = Path(temp_dir)
            source = temp_path / "requests_http_test.zeu"
            executable = temp_path / "requests_http_test.exe"
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
                [str(executable)], cwd=ROOT, capture_output=True, timeout=30
            )
            stdout = result.stdout.decode("mbcs", errors="replace")
            stderr = result.stderr.decode("mbcs", errors="replace")
            if result.returncode != 0 or "REQUESTS_HTTP_TEST_PASS" not in stdout:
                raise AssertionError(
                    f"HTTP client test failed ({result.returncode})\n{stdout}\n{stderr}"
                )
            print(stdout.strip())
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)


if __name__ == "__main__":
    run()
