#!/usr/bin/env python3
"""A gateway whose model always asks for a web search.

The third of the fixture gateways, and the one that makes the agent's search
tool observable end to end: the very first reply is a well-formed
`{"search": "..."}` tool call, and the second is an answer built from the
result. That exercises the whole path -- the tool dispatch in run_tool, the
websearch module's provider chain, the cache write, and the citation of the
url back to the model -- without depending on any real endpoint.

The query and the assertion target are chosen to be stable: the agent is
run with WEBSEARCH_CACHE pointed at a scratch file and a pre-seeded store,
so what comes back is the same on every run and on every machine.

    python3 searchgateway.py <port> <query>
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1])
QUERY = sys.argv[2] if len(sys.argv) > 2 else "zig programming language"

# The first turn is the OpenAI envelope too: choices[0].message.content is
# where the shipped ai module reads the assistant's reply from, so the tool
# call has to be delivered the same way a real endpoint delivers it.
FIRST = json.dumps({
    "choices": [{"message": {"role": "assistant",
                             "content": json.dumps({"search": QUERY})}}]
}).encode()


def second_body():
    # The answer quotes the url the tool returned, which is what the
    # regression asserts on: the model really saw the result.
    #
    # The reply is wrapped in the OpenAI envelope because the agent reads
    # it through choices[0].message.content -- a bare tool call would be a
    # body the shipped ai module cannot parse.
    return json.dumps({
        "choices": [{
            "message": {
                "role": "assistant",
                "content": '{"say": "See https://en.wikipedia.org/wiki/Zig_(programming_language)"}',
            }
        }]
    }).encode()


class Handler(BaseHTTPRequestHandler):
    turns = 0

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        Handler.turns += 1
        body = FIRST if Handler.turns == 1 else second_body()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
