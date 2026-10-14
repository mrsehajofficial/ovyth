#!/usr/bin/env python3
"""A gateway whose model always answers, with a well-formed tool call.

The companion to metagateway.py: where that one serves a reply *about the
protocol*, this one serves a reply *in* it -- a single JSON object with a
`say` -- so a turn can complete end to end. That makes the write-back half of
the memory loop observable: a session that answers one question must file that
exchange, and a later `:memory` must then count one more chunk than it started
with.

    python3 saygateway.py <port>
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0

BODY = b'{"choices":[{"message":{"role":"assistant","content":"{\\"say\\": \\"ok, noted.\\"}"}}]}'


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
