#!/usr/bin/env python3
"""A gateway whose model only ever replies about the protocol.

The regression fixture for a bad UX the agent had: when a weak model fails to
produce a tool call, the agent nudges it — and the model answers the nudge,
replying "Understood. I will reply with exactly one JSON object per turn" to
the user's actual question. This serves that reply shape, so the agent's
meta-commentary guard can be tested without a real endpoint.

    python3 metagateway.py <port>
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0

BODY = (
    b'{"choices":[{"message":{"role":"assistant","content":"Understood. '
    b'I will reply with exactly one JSON object per turn."}}]}'
)


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
