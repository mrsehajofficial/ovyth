#!/usr/bin/env python3
"""Ovyth :: examples/automation/records_api.py

A deterministic "records" API for the automation example: the ordinary fetch,
validate, filter, transform, write, retry, report flow, with nothing on the
network and nothing random.

    python3 examples/automation/records_api.py [port]

Three behaviours, selected by path, all deterministic:

    GET /records    the normal page of records, including two malformed ones
                    so the pipeline's validation has real work
    GET /flaky      fails the first two requests with a 500, then answers --
                    the transient failure the retry has to survive
    GET /down       answers 503 with a body, for the defined-error path
    GET /html       answers 200 with an HTML page -- the gateway login
                    interstitial, so a 200 that is not JSON has a case too

The "first two requests" counter is in-memory, so a fresh server is the fresh
state. That is what makes the test reproducible.
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RECORDS = [
    {"id": 1, "name": "api-gateway", "status": "degraded", "cpu": 71.2, "region": "eu-west"},
    {"id": 2, "name": "auth",        "status": "healthy",  "cpu": 12.4, "region": "eu-west"},
    {"id": 3, "name": "billing",     "status": "down",     "cpu": 0.0,  "region": "us-east"},
    {"id": 4, "name": "search",      "status": "degraded", "cpu": 88.9, "region": "us-east"},
    {"id": 5, "name": "cdn",         "status": "healthy",  "cpu": 31.0, "region": "ap-south"},
    # Malformed on purpose: the pipeline must reject these, not crash on them.
    {"id": 6, "name": "cache",       "status": "healthy",  "region": "ap-south"},
    {"id": 7, "name": 404,           "status": "healthy",  "cpu": 5.0,  "region": "ap-south"},
]

_flaky_seen = 0


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # the suite's output is the program's, not the server's

    def _send(self, code, body):
        raw = json.dumps(body).encode() if not isinstance(body, bytes) else body
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        global _flaky_seen
        path = self.path.split("?")[0]
        if path == "/records":
            self._send(200, {"records": RECORDS, "page": 1, "total": len(RECORDS)})
        elif path == "/flaky":
            _flaky_seen += 1
            if _flaky_seen <= 2:
                self._send(500, {"error": "upstream is having a moment"})
            else:
                self._send(200, {"records": RECORDS, "page": 1, "total": len(RECORDS)})
        elif path == "/html":
            body = b"<html><body>Please sign in to continue</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/down":
            self._send(503, {"error": "the records service is unavailable"})
        else:
            self._send(404, {"error": "no such path"})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
