#!/usr/bin/env python3
"""
Ovyth :: examples/chatbot/mock_server.py
A mock OpenAI-compatible API server for testing the chatbot without an API key.

Run this in one terminal:
    python3 examples/chatbot/mock_server.py

Then run the chatbot:
    AI_API_URL=http://localhost:8765 AI_API_KEY=dummy ovc run examples/chatbot/chatbot.ov
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
import json

responses = [
    {"role": "assistant", "content": "Hello! I'm a mock AI assistant. How can I help you?"},
    {"role": "assistant", "content": "That's interesting! Tell me more."},
    {"role": "assistant", "content": "I understand. Let me process that for you."},
    {"role": "assistant", "content": "Here's what I found:\n\n1. First point\n2. Second point\n3. Third point"},
]

class ChatHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)
        data = json.loads(body)
        
        # Get last user message
        messages = data.get("messages", [])
        last_msg = messages[-1]["content"] if messages else "Hello"
        
        # Generate response
        response_idx = len(messages) % len(responses)
        reply = responses[response_idx]["content"]
        
        output = {
            "id": "mock-123",
            "object": "chat.completion",
            "created": 1700000000,
            "model": data.get("model", "mock-model"),
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": reply
                },
                "finish_reason": "stop"
            }],
            "usage": {
                "prompt_tokens": len(last_msg.split()),
                "completion_tokens": len(reply.split()),
                "total_tokens": len(last_msg.split()) + len(reply.split())
            }
        }
        
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(output).encode())
    
    def log_message(self, format, *args):
        pass  # Suppress logging

if __name__ == "__main__":
    server = HTTPServer(("localhost", 8765), ChatHandler)
    print("Mock server running at http://localhost:8765")
    print("Press Ctrl+C to stop")
    server.serve_forever()
