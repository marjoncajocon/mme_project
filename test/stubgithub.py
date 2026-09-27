#!/usr/bin/env python3
"""stubgithub.py - a fake GitHub REST API (its gists) for the sync-* scenarios.

    python stubgithub.py --mode=empty     the account has no gist yet
    python stubgithub.py --mode=synced    another machine's Settings Sync gist is there:
                                          its settings.json differs, and it has a snippets file
    python stubgithub.py --mode=noscope   the token cannot make gists (no "gist" scope)

It listens on 127.0.0.1 on a free port and prints "PORT <n>" on its first
line; run.py sets MME_GITHUB_API to it and GH_TOKEN to the dummy token it
wants. Nothing here talks to GitHub, and the token is never a real one.

Like GitHub it can keep a file other than it was sent: here the last line
end is dropped, so a sync that took that for a change would show it.
"""
import json
import socket
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

TOKEN = "gh-test-token"
MODE = "empty"
DESC = "mme Settings Sync"
GISTS = {}
NEXT = [1]

REMOTE_SETTINGS = '{\n  "editor.fontSize": 20,\n  "workbench.colorTheme": "Default Light Modern"\n}\n'
REMOTE_SNIPPET = '{\n  "main": {"prefix": "main", "body": ["int main (void) {", "\\t$0", "}"]}\n}\n'


def stored(content):
    return content[:-1] if content.endswith("\n") else content


def gist_json(gid, full):
    g = GISTS[gid]
    files = {}
    for name, content in g["files"].items():
        f = {"filename": name, "size": len(content.encode()), "truncated": False,
             "raw_url": "http://127.0.0.1/raw/" + name}
        if full:
            f["content"] = content
        files[name] = f
    return {"id": gid, "description": g["description"], "public": False, "files": files,
            "html_url": "https://gist.github.com/tester/" + gid, "updated_at": "2026-09-26T10:00:00Z"}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj=None):
        b = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def authed(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.reply(401, {"message": "Bad credentials"})
            return False
        return True

    def body(self):
        n = int(self.headers.get("content-length") or 0)
        return json.loads(self.rfile.read(n).decode("utf-8")) if n else {}

    def do_GET(self):
        if not self.authed():
            return
        p = self.path.split("?")[0]
        if p == "/user":
            return self.reply(200, {"login": "tester"})
        if p == "/gists":
            return self.reply(200, [gist_json(g, False) for g in GISTS])
        if p.startswith("/gists/") and p[7:] in GISTS:
            return self.reply(200, gist_json(p[7:], True))
        self.reply(404, {"message": "Not Found"})

    def do_POST(self):
        if not self.authed():
            return
        b = self.body()
        if self.path != "/gists" or MODE == "noscope":
            return self.reply(404, {"message": "Not Found"})
        if b.get("public") is not False or not b.get("files"):
            return self.reply(422, {"message": "a secret gist with files, please"})
        gid = "g%d" % NEXT[0]
        NEXT[0] += 1
        GISTS[gid] = {"description": b.get("description", ""),
                      "files": {k: stored(v["content"]) for k, v in b["files"].items() if v}}
        self.reply(201, gist_json(gid, True))

    def do_PATCH(self):
        if not self.authed():
            return
        gid = self.path[7:]
        if not self.path.startswith("/gists/") or gid not in GISTS or MODE == "noscope":
            return self.reply(404, {"message": "Not Found"})
        for k, v in self.body().get("files", {}).items():
            if v is None:
                GISTS[gid]["files"].pop(k, None)
            else:
                GISTS[gid]["files"][k] = stored(v["content"])
        self.reply(200, gist_json(gid, True))

    def do_DELETE(self):
        if not self.authed():
            return
        gid = self.path[7:]
        if gid in GISTS:
            del GISTS[gid]
            return self.reply(204)
        self.reply(404, {"message": "Not Found"})


def main():
    global MODE
    for a in sys.argv[1:]:
        if a.startswith("--mode="):
            MODE = a[7:]
    if MODE == "synced":
        GISTS["g0"] = {"description": DESC, "files": {"settings.json": stored(REMOTE_SETTINGS),
                                                      "snippets.c.json": stored(REMOTE_SNIPPET)}}
    GISTS["other"] = {"description": "some other gist", "files": {"notes.txt": "hello"}}
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    srv = HTTPServer(("127.0.0.1", port), H)
    print("PORT %d" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
