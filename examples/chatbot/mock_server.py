#!/usr/bin/env python3
"""Minimal OpenAI-compatible mock for testing the Vayu chatbot without a network.

Serves POST /v1/chat, echoes the last user message back as a canned reply.
Usage:  python3 mock_server.py [port]
Then:    AI_API_KEY=test AI_API_URL=http://127.0.0.1:8642/v1/chat \\
        AI_MODEL=mock-model vyc run chatbot.vy
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # keep stdout clean

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages", [])
        user = ""
        for m in messages:
            if m.get("role") == "user":
                user = m.get("content", "")
        reply = f"you said: {user!r}"
        payload = {
            "model": body.get("model", "mock-model"),
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": reply},
                    "finish_reason": "stop",
                }
            ],
        }
        out = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8642
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
