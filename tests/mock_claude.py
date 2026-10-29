"""Stand-in for the Claude Messages API, used by tests/e2e.c.

    python mock_claude.py <port> <requests.jsonl>

Answers POST /v1/messages with the text between <text> tags in upper case, the way the
real API shapes a reply (an empty thinking block, then a text block). Text containing
ERROR401 gets an authentication error. Every request is appended to requests.jsonl
(without the API key) so the test can check what LangFix sent.
"""
import http.server
import json
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        body = json.loads(raw)
        key_ok = self.headers.get("x-api-key") == "test-key"
        with open(sys.argv[2], "a", encoding="utf-8") as f:
            headers = {k.lower(): v for k, v in self.headers.items() if k.lower() != "x-api-key"}
            f.write(json.dumps({"path": self.path, "key_ok": key_ok, "headers": headers, "body": body},
                               ensure_ascii=False) + "\n")
        content = body["messages"][0]["content"]
        text = content.split("<text>\n", 1)[1].rsplit("\n</text>", 1)[0]
        if self.path != "/v1/messages" or not key_ok or "ERROR401" in text:
            return self.reply(401, {"type": "error", "error": {"type": "authentication_error",
                                                              "message": "invalid x-api-key"}})
        self.reply(200, {
            "id": "msg_test", "type": "message", "role": "assistant", "model": body["model"],
            "content": [{"type": "thinking", "thinking": "", "signature": "sig"},
                        {"type": "text", "text": text.upper()}],
            "stop_reason": "end_turn", "usage": {"input_tokens": 10, "output_tokens": 5},
        })

    def reply(self, code, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
