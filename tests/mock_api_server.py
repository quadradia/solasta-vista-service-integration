"""Local HTTPS stand-in for the Identity Server token endpoint and the Solasta import endpoint.

Never talks to the real services. TLS 1.2 only (like the XP client), self-signed certificate.

Usage:
  python -I tests/mock_api_server.py --cert cert.pem --key key.pem --port 8443 \
      --client-id vista-import --secret-file secret.txt --out received.jsonl

Endpoints:
  POST /connect/token                    client_credentials grant (form body or HTTP Basic)
  POST /api/inventory/import/items       needs "Authorization: Bearer <token>"; appends the body to --out
"""
import argparse
import base64
import json
import secrets
import ssl
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote

ARGS = None
TOKENS = set()
LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        sys.stderr.write("mock: %s %s\n" % (self.address_string(), fmt % a))

    def send_json(self, status, obj, chunked=False):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        if chunked:  # exercise the client's chunked decoder
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            half = len(data) // 2
            for part in (data[:half], data[half:]):
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def read_body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n)

    def do_POST(self):
        body = self.read_body()
        if self.path == "/connect/token":
            form = {k: v[0] for k, v in parse_qs(body.decode("ascii")).items()}
            cid, sec = form.get("client_id"), form.get("client_secret")
            auth = self.headers.get("Authorization", "")
            if auth.startswith("Basic "):
                cid, sec = (unquote(x) for x in base64.b64decode(auth[6:]).decode().split(":", 1))
            if form.get("grant_type") != "client_credentials":
                return self.send_json(400, {"error": "unsupported_grant_type"})
            if cid != ARGS.client_id or sec != ARGS.secret:
                return self.send_json(400, {"error": "invalid_client"})
            if "inventory.import" not in form.get("scope", "").split():
                return self.send_json(400, {"error": "invalid_scope"})
            tok = secrets.token_urlsafe(24)
            with LOCK:
                TOKENS.add(tok)
            return self.send_json(200, {"access_token": tok, "token_type": "Bearer", "expires_in": 3600,
                                        "scope": form.get("scope")})
        if self.path == "/api/inventory/import/items":
            auth = self.headers.get("Authorization", "")
            with LOCK:
                ok = auth.startswith("Bearer ") and auth[7:] in TOKENS
            if not ok:
                return self.send_json(401, {"error": "invalid_token"})
            try:
                doc = json.loads(body.decode("utf-8"))
            except ValueError as e:
                return self.send_json(400, {"error": "bad json: %s" % e})
            with LOCK, open(ARGS.out, "a", encoding="utf-8") as f:
                f.write(json.dumps(doc) + "\n")
            results = [{"sku": i["sku"], "status": "upserted"} for i in doc.get("items", [])]
            return self.send_json(200, {"received": len(results), "results": results}, chunked=True)
        self.send_json(404, {"error": "not found"})


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--port", type=int, default=8443)
    p.add_argument("--client-id", default="vista-import")
    p.add_argument("--secret-file", required=True)
    p.add_argument("--out", required=True)
    ARGS = p.parse_args()
    ARGS.secret = open(ARGS.secret_file, encoding="utf-8").read().strip()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(ARGS.cert, ARGS.key)
    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    sys.stderr.write("mock: listening on https://localhost:%d (TLS 1.2 only)\n" % ARGS.port)
    srv.serve_forever()


if __name__ == "__main__":
    main()
