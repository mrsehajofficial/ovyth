#!/usr/bin/env python3
"""A provider that misbehaves on purpose.

The AI suite's whole subject is what the client does when the endpoint is
wrong. Each request's "model" field selects the failure, so one server
covers every case and the client configuration stays the only variable:

    ok             200, a well-formed chat completion
    err500         500 with a JSON error body
    badjson        200 whose body is not JSON at all
    html200        200 with an HTML page (a proxy login interstitial)
    emptychoices   200 with {"choices": []}
    nullcontent    200 with choices[0].message.content == null
    emptycontent   200 with content ""
    hang           sleeps past any sane client timeout

Every request's Authorization header is appended to the file named by
ARGV[2], which is how the credential test proves the client *did* send the
key and then never printed it.
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT_FILE, SEEN_FILE = sys.argv[1], sys.argv[2]


class Provider(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # keep the suite's output clean
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        with open(SEEN_FILE, "a") as f:
            f.write((self.headers.get("Authorization") or "<none>") + "\n")

        try:
            model = json.loads(raw).get("model", "")
        except ValueError:
            model = ""

        if model == "hang":
            time.sleep(5)
            model = "ok"

        if model == "ok":
            body = json.dumps({
                "choices": [{"message": {"role": "assistant",
                                         "content": "pong from the bad provider"}}]
            }).encode()
            self.send_response(200)
        elif model == "err500":
            body = json.dumps({"error": {"message": "upstream is on fire"}}).encode()
            self.send_response(500)
        elif model == "badjson":
            body = b"this is not json, not even close {"
            self.send_response(200)
        elif model == "html200":
            body = b"<html><body>Please sign in to continue</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
        elif model == "emptychoices":
            body = b'{"choices": []}'
            self.send_response(200)
        elif model == "nullcontent":
            body = json.dumps({"choices": [{"message": {"content": None}}]}).encode()
            self.send_response(200)
        elif model == "emptycontent":
            body = json.dumps({"choices": [{"message": {"content": ""}}]}).encode()
            self.send_response(200)
        else:
            body = json.dumps({"error": {"message": "unknown model %r" % model}}).encode()
            self.send_response(400)

        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
with open(PORT_FILE, "w") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
