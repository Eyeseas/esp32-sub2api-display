import hashlib
import hmac
import json
import ssl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


def make_server(cfg, collector):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self):
            super().setup()
            self.connection.settimeout(15)

        def log_message(self, *args):
            pass  # No tokens, query strings, upstream errors or account names in access logs.

        def send(self, status, body=b"", content_type="application/json; charset=utf-8", etag=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "private, no-cache")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Vary", "Authorization")
            if etag:
                self.send_header("ETag", etag)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def error(self, status, message):
            self.send(status, json.dumps({"code": status, "message": message, "data": None}).encode())

        def do_GET(self):
            parsed = urlsplit(self.path)
            if parsed.path == "/":
                self.send(200, (Path(__file__).parent / "preview.html").read_bytes(), "text/html; charset=utf-8")
                return
            if parsed.path == "/health":
                self.send(200, b'{"status":"ok","service":"independent_bridge"}')
                return
            if not parsed.path.startswith("/display/v1/"):
                self.error(404, "not_found")
                return
            authorization = self.headers.get("Authorization", "")
            token = authorization[7:] if authorization.startswith("Bearer ") else ""
            digest = hashlib.sha256(token.encode()).hexdigest()
            device = None
            for item in cfg["devices"]:
                if token and hmac.compare_digest(digest, item["token_sha256"]):
                    device = item
            if device is None:
                self.error(401, "device_unauthorized")
                return
            query = parse_qs(parsed.query)
            try:
                if any(len(values) != 1 for values in query.values()):
                    raise ValueError()
                page = int(query.get("page", ["1"])[0])
                size = int(query.get("page_size", ["5"])[0])
                window_page = int(query.get("window_page", ["1"])[0])
                if not (1 <= page <= 10000 and 1 <= size <= 10 and 1 <= window_page <= 10000):
                    raise ValueError()
            except ValueError:
                self.error(400, "invalid_pagination")
                return
            detail = None
            if parsed.path.startswith("/display/v1/accounts/"):
                detail = parsed.path.removeprefix("/display/v1/accounts/")
                if not detail.isascii() or not detail.isdigit() or str(int(detail)) != detail:
                    self.error(400, "invalid_account_id")
                    return
            elif parsed.path != "/display/v1/snapshot":
                self.error(404, "not_found")
                return
            try:
                payload = collector.snapshot(device, page, size, detail, window_page)
            except KeyError:
                self.error(404, "account_not_in_scope")
                return
            if payload["data"]["generated_at"] is None:
                self.error(503, "snapshot_unavailable")
                return
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode()
            if len(body) > 32768:
                self.error(413, "snapshot_too_large_reduce_page_size")
                return
            etag = '"' + hashlib.sha256(body).hexdigest() + '"'
            if etag in [value.strip() for value in self.headers.get("If-None-Match", "").split(",")]:
                self.send(304, etag=etag)
            else:
                self.send(200, body, etag=etag)

    server = ThreadingHTTPServer((cfg.get("listen_host", "127.0.0.1"), cfg.get("listen_port", 8787)), Handler)
    server.daemon_threads = True
    if cfg.get("tls_cert_file") and cfg.get("tls_key_file"):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(Path(cfg["config_dir"]) / cfg["tls_cert_file"]),
                                str(Path(cfg["config_dir"]) / cfg["tls_key_file"]))
        server.socket = context.wrap_socket(server.socket, server_side=True)
    return server
