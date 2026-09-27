#!/usr/bin/env python3
"""stubclaude.py - a fake Anthropic Messages API for the chat-* scenarios.

    python stubclaude.py --mode=ok        a streamed answer (the default)
    python stubclaude.py --mode=401       the key is refused
    python stubclaude.py --mode=529       the API is overloaded
    python stubclaude.py --mode=refusal   the model declines

Agent mode (a request with "tools") is answered as an agent would: read
edit.c, then edit its first line, then say what it did (or that the edit
was denied). POST /v1/chat/completions plays an OpenAI-compatible API (the
key sk-openai-test, the model gpt-test), streamed as OpenAI streams.

It listens on 127.0.0.1 on a free port and prints "PORT <n>" on its first
line, so run.py can point mme.chat.baseUrl at it. Nothing here ever talks to
Anthropic, and no key is ever real: the scenarios use a dummy one.

What mme sends is checked the way the real API would, and a little stricter:
the headers (x-api-key, anthropic-version, the fallback beta), the body's
model, max_tokens, stream and fallbacks, no sampling parameters, and the
roles taking turns, ending on the user's. Anything wrong is answered with a
400 whose message says what, so a golden would show it on the screen.

The answer is streamed the way the API streams: message_start, a thinking
block mme must not show, ping, text deltas cut mid-word, message_delta with
its stop_reason, message_stop - with small pauses between the events. A chat
answer says which turn it is (so resending the talk is checked) and what the
attachment named; Inline Chat's answer is the selection with int made long.
"""
import json
import re
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

KEY = "sk-ant-test-dummy-key"
OPENAI_KEY = "sk-openai-test"
MODE = "ok"
FIRST_LINE = "/* edit.c - a small C file the editing scenarios type into. */"


def problems(headers, body):
    out = []
    if headers.get("x-api-key") != KEY:
        out.append("x-api-key is not the scenario's key")
    if headers.get("anthropic-version") != "2023-06-01":
        out.append("anthropic-version is not 2023-06-01")
    if "application/json" not in (headers.get("content-type") or ""):
        out.append("content-type is not JSON")
    if "server-side-fallback-2026-07-01" not in (headers.get("anthropic-beta") or ""):
        out.append("the fallback beta header is missing")
    try:
        j = json.loads(body)
    except ValueError as e:
        return out + ["the body is not JSON: %s" % e], None
    if j.get("model") != "claude-opus-5":
        out.append("model is %r" % j.get("model"))
    if j.get("max_tokens") != 64000:
        out.append("max_tokens is %r" % j.get("max_tokens"))
    if j.get("stream") is not True:
        out.append("stream is not true")
    if j.get("fallbacks") != "default":
        out.append("fallbacks is not \"default\"")
    for bad in ("temperature", "top_p", "top_k", "thinking"):
        if bad in j:
            out.append("%s must not be sent" % bad)
    msgs = j.get("messages") or []
    if not msgs or msgs[-1].get("role") != "user":
        out.append("the messages do not end on the user's")
    for a, b in zip(msgs, msgs[1:]):
        if a.get("role") == b.get("role"):
            out.append("two %s messages in a row" % a.get("role"))
    if not isinstance(j.get("system"), str) or not j["system"]:
        out.append("no system prompt")
    return out, j


def agent_step(j):
    """what the agent does next: ("tool", name, input) or ("text", words)"""
    last = j["messages"][-1]["content"]
    if isinstance(last, str):
        return ("tool", "toolu_read", "read_file", {"path": "edit.c", "start_line": 1, "end_line": 2})
    res = [b for b in last if b.get("type") == "tool_result"]
    got = res[-1] if res else {}
    if got.get("tool_use_id") == "toolu_read":
        if FIRST_LINE not in got.get("content", ""):
            return ("text", "read_file did not return the file: %r" % got.get("content", "")[:80])
        return ("tool", "toolu_edit", "edit_file", {"path": "edit.c", "old_text": FIRST_LINE,
                                                    "new_text": "/* edit.c - changed by the agent. */"})
    if "denied" in got.get("content", ""):
        return ("text", "You denied the edit, so **nothing changed**.")
    return ("text", "Done: the first line of `edit.c` now says it was changed by the agent.")


def chat_answer(j):
    msgs = j["messages"]
    turn = sum(1 for m in msgs if m["role"] == "user")
    q = msgs[-1]["content"]
    if not isinstance(q, str):
        q = ""
    m = re.search(r'<attachment path="([^"]*)" lines="([^"]*)"', q)
    seen = ("I see `%s`, lines %s." % (m.group(1), m.group(2))) if m else "No file came with it."
    return ("Answer **%d**. %s\n\n"
            "Use a helper:\n\n"
            "```c\n"
            "static int twice (int n) {\n"
            "\treturn n * 2;\n"
            "}\n"
            "```\n\n"
            "- it is *pure*\n"
            "- it is short\n" % (turn, seen))


def inline_answer(j):
    q = j["messages"][-1]["content"]
    m = re.findall(r"<selection>(.*?)</selection>", q, re.S)   # the file's, not the preamble's
    if m:
        return m[-1].replace("int", "long").rstrip("\n")
    return "/* written at the cursor */"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, obj, extra=()):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def event(self, name, obj, pause=0.03):
        self.wfile.write(("event: %s\ndata: %s\n\n" % (name, json.dumps(obj))).encode())
        self.wfile.flush()
        time.sleep(pause)

    def openai(self, body):
        """an OpenAI-compatible chat/completions: checked, then streamed as OpenAI streams"""
        bad = []
        if self.headers.get("authorization") != "Bearer " + OPENAI_KEY:
            bad.append("the bearer token is not the scenario's")
        try:
            j = json.loads(body)
        except ValueError:
            j = {}
            bad.append("the body is not JSON")
        msgs = j.get("messages") or []
        if j.get("model") != "gpt-test":
            bad.append("model is %r" % j.get("model"))
        if j.get("stream") is not True:
            bad.append("stream is not true")
        if not msgs or msgs[0].get("role") != "system":
            bad.append("the first message is not the system prompt")
        if "Claude" in (msgs[0].get("content", "") if msgs else ""):
            bad.append("the system prompt names Claude to another model")
        if bad:
            return self.reply(400, {"error": {"message": "; ".join(bad), "type": "invalid_request_error"}})
        turn = sum(1 for m in msgs if m["role"] == "user")
        text = "OpenAI answer **%d**: the model is gpt-test.\n\n- streamed as `chat.completion.chunk`\n" % turn
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("connection", "close")
        self.end_headers()
        self.close_connection = True
        def chunk(delta, fin=None):
            o = {"id": "chatcmpl-stub", "object": "chat.completion.chunk", "model": "gpt-test",
                 "choices": [{"index": 0, "delta": delta, "finish_reason": fin}]}
            self.wfile.write(("data: %s\n\n" % json.dumps(o)).encode())
            self.wfile.flush()
            time.sleep(0.005)
        chunk({"role": "assistant", "content": ""})
        for i in range(0, len(text), 6):
            chunk({"content": text[i:i + 6]})
        chunk({}, "stop")
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    def do_GET(self):	# the models, as /v1/models lists them (Chat: Change Model)
        if self.path.startswith("/v1/models") and self.headers.get("x-api-key") == KEY:
            return self.reply(200, {"data": [{"id": "claude-opus-5", "type": "model"},
                                             {"id": "claude-sonnet-5", "type": "model"}], "has_more": False})
        return self.reply(404, {"type": "error", "error": {"type": "not_found_error", "message": "no " + self.path}})

    def do_POST(self):
        n = int(self.headers.get("content-length") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace")
        if self.path == "/v1/chat/completions":
            return self.openai(body)
        if self.path != "/v1/messages":
            return self.reply(404, {"type": "error", "error": {"type": "not_found_error",
                                                               "message": "no " + self.path}})
        if MODE == "401":
            return self.reply(401, {"type": "error", "error": {"type": "authentication_error",
                                                               "message": "invalid x-api-key"}})
        if MODE == "529":
            return self.reply(529, {"type": "error", "error": {"type": "overloaded_error",
                                                               "message": "Overloaded"}})
        bad, j = problems(self.headers, body)
        if bad:
            return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                                                               "message": "; ".join(bad)}})
        inline = "editing code" in j["system"]
        step = agent_step(j) if j.get("tools") else None
        if j.get("tools"):
            names = sorted(t.get("name") for t in j["tools"])
            if names != sorted(["read_file", "list_dir", "search_text", "edit_file", "create_file", "run_command"]):
                return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                                                                   "message": "the tools are %r" % names}})
        text = inline_answer(j) if inline else (step[1] if step and step[0] == "text" else chat_answer(j))
        if step and step[0] == "tool":
            text = "Let me look at `edit.c` first." if step[2] == "read_file" else "Now the change."
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("cache-control", "no-cache")
        self.send_header("connection", "close")
        self.end_headers()
        self.close_connection = True
        self.event("message_start", {"type": "message_start", "message": {
            "id": "msg_stub", "type": "message", "role": "assistant", "model": j["model"],
            "content": [], "stop_reason": None, "usage": {"input_tokens": 10, "output_tokens": 1}}})
        self.event("content_block_start", {"type": "content_block_start", "index": 0,
                                           "content_block": {"type": "thinking", "thinking": ""}})
        self.event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                           "delta": {"type": "thinking_delta", "thinking": "SECRET THOUGHT"}})
        self.event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                           "delta": {"type": "signature_delta", "signature": "c2ln"}})
        self.event("content_block_stop", {"type": "content_block_stop", "index": 0})
        self.event("ping", {"type": "ping"})
        stop = "end_turn"
        if MODE == "refusal":
            stop = "refusal"
            text = ""
        else:
            self.event("content_block_start", {"type": "content_block_start", "index": 1,
                                               "content_block": {"type": "text", "text": ""}})
            for i in range(0, len(text), 7):   # cut anywhere, mid-word and mid-line
                self.event("content_block_delta", {"type": "content_block_delta", "index": 1,
                                                   "delta": {"type": "text_delta", "text": text[i:i + 7]}},
                           pause=0.005)
            self.event("content_block_stop", {"type": "content_block_stop", "index": 1})
        if step and step[0] == "tool" and MODE != "refusal":	# the tool call: its input streams as JSON pieces
            self.event("content_block_start", {"type": "content_block_start", "index": 2, "content_block": {
                "type": "tool_use", "id": step[1], "name": step[2], "input": {}}})
            raw = json.dumps(step[3])
            for i in range(0, len(raw), 9):
                self.event("content_block_delta", {"type": "content_block_delta", "index": 2,
                                                   "delta": {"type": "input_json_delta", "partial_json": raw[i:i + 9]}},
                           pause=0.003)
            self.event("content_block_stop", {"type": "content_block_stop", "index": 2})
            stop = "tool_use"
        self.event("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop,
                                     "stop_sequence": None}, "usage": {"output_tokens": 42}})
        self.event("message_stop", {"type": "message_stop"}, pause=0)


def main():
    global MODE
    for a in sys.argv[1:]:
        if a.startswith("--mode="):
            MODE = a.split("=", 1)[1]
    srv = HTTPServer(("127.0.0.1", 0), Handler)
    sys.stdout.write("PORT %d\n" % srv.server_address[1])
    sys.stdout.flush()
    srv.serve_forever()


if __name__ == "__main__":
    main()
