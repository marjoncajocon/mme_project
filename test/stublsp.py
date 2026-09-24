#!/usr/bin/env python3
"""stublsp.py - a fixed, fake language server for the regression suite.

A real clangd or gopls answers differently on every machine and every version,
so the suite cannot pin what mme draws from it. This stub always answers the
same thing, which makes the language-server surfaces -- PROBLEMS, the squiggles
in the gutter, the code lens rows, inlay hints and the document symbols --
testable. It is not a language server: it never looks at the text.

mme starts it from settings.json:  "c": "python <this file>"

It answers textDocument/copilotInlineEdit too -- GitHub Copilot's next edit
suggestion, the edit somewhere else in the file that what was just typed calls
for -- with the same fixed content by cursor position, in exactly the shape
copilot-language-server 1.551 answers with: {"edits":[{"text","range",
"textDocument","cacheTelemetryContext","command"}]}. The entry for line 18
also echoes every notification and command that edit draws out of mme
(didShowInlineEdit, reportCachedInlineEdit, the accept and reject commands)
back as a window/showMessage, so a scenario can read the wire traffic off the
screen.

It also stands in for GitHub Copilot's account surface -- signIn with its
device flow, signOut, checkStatus and the didChangeStatus notification -- so
the sign-in can be driven end to end without a GitHub account. Which account
state it pretends to be in is the one argument it takes:

    --auth=out      nobody is signed in (the default): Inactive at startup
    --auth=in       signed in as stubuser: Normal at startup
    --auth=error    signed in, but the server is unhappy: Error at startup
    --auth=busy     signed in and fetching: Normal with busy true
    --auth=v2       signed in, said only in didChangeStatus/v2, the newer shape
                    Copilot's server 1.551 sends beside the old one

signIn always answers with the device flow, and the command it names
(github.copilot.finishDeviceFlow) answers DEVICE_WAIT seconds later from a
thread of its own -- the server keeps answering everything else meanwhile,
exactly as Copilot's does, which is what makes a frozen editor visible.
"""
import json
import os
import sys
import threading
import time

IN = sys.stdin.buffer
OUT = sys.stdout.buffer
LOCK = threading.Lock()          # the device-flow thread writes too

AUTH = "out"
for _a in sys.argv[1:]:
    if _a.startswith("--auth="):
        AUTH = _a.split("=", 1)[1]

USER = "stubuser"
DEVICE_CODE = "ABCD-1234"
DEVICE_URI = "https://github.com/login/device"
DEVICE_WAIT = 1.2                # seconds the "browser" takes; long enough to see
SIGNED_IN = AUTH in ("in", "error", "busy", "v2")

# everything below is by line number, counted from 0, in whatever file is open
DIAGS = [
    (12, 11, 16, 2, "total is never read", "stub", "unused-variable"),
    (30, 1, 30, 8, "prefer a const pointer here", "stub", "style"),
]
LENSES = [
    (15, "2 references"),
    (28, "run test | debug test"),
]
HINTS = [
    (16, 14, ": int"),
]
# textDocument/inlineCompletion, by exactly where the cursor is. Nothing is
# offered anywhere else, so every scenario that is not about ghost text sees
# what it always saw, and accepting one does not make another appear. An entry
# is a list of items; an item is (insertText, command-or-None).
INLINE = {
    # one line, on the blank line 12
    (11, 0): [("int cached_total = total;", "stub.didAccept")],
    # two candidates on the blank line 14, for Alt+] and Alt+[
    (13, 0): [("int first = 1;", None), ("int second = 2;", None)],
    # three lines on the blank line 20: the lines under it move down
    (19, 0): [("static int sub (int a, int b) {\n\treturn a - b;\n}", None)],
}

# textDocument/copilotInlineEdit, by exactly where the cursor is, the same way.
# The key is the cursor; the value is the one edit the server offers somewhere
# else in the file: (line0, char0, line1, char1, replacement, echo). With echo
# the stub answers every notification and command that edit draws with a
# window/showMessage, so a scenario can see on the screen that the wire
# traffic really went. edit.c has 39 lines and 26 of them fit on the screen,
# so line 34 is only reachable by scrolling: that is the off-screen one.
NEDIT = {
    # seven lines below the cursor, on the screen: "total += a + b;"
    (5, 0): (16, 0, 16, 16, "	total += a + b + 1;", False),
    # nine lines above the cursor: "static int total = 0;"
    (21, 0): (12, 0, 12, 21, "static int total = 1;", False),
    # far below the last line the screen shows: "total = add(total, i);"
    (3, 0): (34, 0, 34, 24, "		total = add(total, i * 2);", False),
    # above the top of the screen, once the view has scrolled down
    (35, 0): (2, 0, 2, 19, "#include <stdlib.h>  /* moved */", False),
    # the same place the inline completion above answers for: the cursor's
    # suggestion must win and nothing may point anywhere else
    (11, 0): (16, 0, 16, 16, "	total += a + b + 2;", False),
    # one that says out loud what mme sent it
    (17, 0): (12, 0, 12, 21, "static int total = 7;", True),
}
NEDIT_ID = "stub-nes-1"     # the id its command carries, and the notifications


def send(msg):
    body = json.dumps(msg, sort_keys=True, separators=(",", ":")).encode("utf-8")
    with LOCK:
        OUT.write(b"Content-Length: %d\r\n\r\n" % len(body))
        OUT.write(body)
        OUT.flush()


def status(kind, message, busy=False):
    """Copilot's own notification: how the server is, right now"""
    send({"jsonrpc": "2.0", "method": "didChangeStatus",
          "params": {"busy": busy, "kind": kind, "message": message}})


def account():
    """what checkStatus answers, from the state we are pretending to be in"""
    if SIGNED_IN:
        return {"status": "OK", "user": USER}
    return {"status": "NotSignedIn"}


def finish_device_flow(mid):
    """the command signIn named: it answers only once the user has authorised"""
    global SIGNED_IN
    time.sleep(DEVICE_WAIT)
    SIGNED_IN = True
    send({"jsonrpc": "2.0", "id": mid, "result": {"status": "OK", "user": USER}})
    status("Normal", "Ready")


ECHO = False                     # the edit on the screen asked to be echoed


def say(text):
    """a window/showMessage: mme draws it as a toast, so the suite can see it"""
    send({"jsonrpc": "2.0", "method": "window/showMessage",
          "params": {"type": 3, "message": text}})


def read():
    n = 0
    while True:
        line = IN.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":", 1)[1])
    if n <= 0:
        return None
    return json.loads(IN.read(n).decode("utf-8"))


def rng(l0, c0, l1, c1):
    return {"start": {"line": l0, "character": c0},
            "end": {"line": l1, "character": c1}}


def publish(uri):
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
          "params": {"uri": uri, "diagnostics": [
              {"range": rng(a, b, c, d), "severity": 1 if i == 0 else 2,
               "source": src, "code": code, "message": msg}
              for i, (a, b, c, d, msg, src, code) in enumerate(DIAGS)]}})


CAPS = {
    "textDocumentSync": 1,
    "hoverProvider": True,
    "definitionProvider": True,
    "referencesProvider": True,
    "documentSymbolProvider": True,
    "codeLensProvider": {"resolveProvider": False},
    "inlayHintProvider": True,
    "documentHighlightProvider": True,
    "completionProvider": {"resolveProvider": False, "triggerCharacters": ["."]},
    "inlineCompletionProvider": True,
}


def main():
    while True:
        msg = read()
        if msg is None:
            return 0
        method = msg.get("method")
        mid = msg.get("id")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": mid,
                  "result": {"capabilities": CAPS,
                             "serverInfo": {"name": "stub-lsp", "version": "1"}}})
            # the status the editor sees before it has asked anything
            if AUTH == "error":
                status("Error", "You have exceeded your completions quota")
            elif AUTH == "busy":
                status("Normal", "Fetching a suggestion...", busy=True)
            elif AUTH == "v2":
                # exactly what copilot-language-server 1.551 sends, and nothing
                # of the older shape: the account comes with the notification
                send({"jsonrpc": "2.0", "method": "didChangeStatus/v2", "params": {
                    "statuses": [
                        {"category": "auth", "kind": "Normal",
                         "result": {"status": "OK", "user": USER}},
                        {"category": "completion", "busy": False},
                        {"category": "cls", "kind": "Normal", "inactive": False}]}})
            elif AUTH == "in":
                status("Normal", "Ready")
            else:
                status("Inactive", "Sign in to use Copilot")
        elif method == "checkStatus":
            # in the v2 state the editor must learn the account from the
            # notification alone, so this answers nothing it could use
            send({"jsonrpc": "2.0", "id": mid,
                  "result": None if AUTH == "v2" else account()})
        elif method == "signIn":
            if SIGNED_IN:
                send({"jsonrpc": "2.0", "id": mid,
                      "result": {"status": "AlreadySignedIn", "user": USER}})
            else:
                send({"jsonrpc": "2.0", "id": mid, "result": {
                    "status": "PromptUserDeviceFlow",
                    "userCode": DEVICE_CODE,
                    "verificationUri": DEVICE_URI,
                    "expiresIn": 899, "interval": 5,
                    "command": {"command": "github.copilot.finishDeviceFlow",
                                "arguments": []}}})
        elif method == "signOut":
            globals()["SIGNED_IN"] = False
            send({"jsonrpc": "2.0", "id": mid, "result": {"status": "NotSignedIn"}})
            status("Inactive", "Sign in to use Copilot")
        elif method == "workspace/executeCommand" and \
                msg["params"].get("command") == "github.copilot.finishDeviceFlow":
            # answered from a thread: the main loop goes on serving everything else
            threading.Thread(target=finish_device_flow, args=(mid,),
                             daemon=True).start()
        elif method == "shutdown":
            send({"jsonrpc": "2.0", "id": mid, "result": None})
        elif method == "exit":
            return 0
        elif method in ("textDocument/didOpen", "textDocument/didChange"):
            uri = msg["params"]["textDocument"]["uri"]
            publish(uri)
        elif method == "textDocument/codeLens":
            send({"jsonrpc": "2.0", "id": mid, "result": [
                {"range": rng(line, 0, line, 1),
                 "command": {"title": title, "command": "stub.noop"}}
                for line, title in LENSES]})
        elif method == "textDocument/inlayHint":
            send({"jsonrpc": "2.0", "id": mid, "result": [
                {"position": {"line": l, "character": c}, "label": lab, "kind": 1}
                for l, c, lab in HINTS]})
        elif method == "textDocument/inlineCompletion":
            pos = msg["params"]["position"]
            at = (pos["line"], pos["character"])
            items = []
            for text, cmd in INLINE.get(at, []):
                item = {"insertText": text}
                if cmd:
                    item["command"] = {"title": "stub", "command": cmd}
                items.append(item)
            # both shapes of the answer are in use: a bare array, and {"items": [...]}
            result = {"items": items} if at[0] == 19 else items
            send({"jsonrpc": "2.0", "id": mid, "result": result})
        elif method == "textDocument/copilotInlineEdit":
            pos = msg["params"]["position"]
            e = NEDIT.get((pos["line"], pos["character"]))
            edits = []
            if e:
                l0, c0, l1, c1, text, echo = e
                globals()["ECHO"] = echo
                edits = [{"text": text, "range": rng(l0, c0, l1, c1),
                          "textDocument": msg["params"]["textDocument"],
                          "cacheTelemetryContext": "{\"stub\":1}",
                          "command": {"title": "Accept inline edit",
                                      "command": "stub.didAcceptInlineEdit",
                                      "arguments": [NEDIT_ID]}}]
            send({"jsonrpc": "2.0", "id": mid, "result": {"edits": edits}})
        elif method == "textDocument/didShowInlineEdit":
            if ECHO:
                say("didShowInlineEdit " +
                    msg["params"]["item"]["command"]["arguments"][0])
        elif method == "textDocument/reportCachedInlineEdit":
            if ECHO:
                pm = msg["params"]
                say("reportCachedInlineEdit %s %s shown=%s" %
                    (pm["opportunityId"], pm["acceptance"], pm["isShown"]))
        elif method == "workspace/executeCommand" and \
                str(msg["params"].get("command", "")).startswith(
                    ("stub.didAccept", "github.copilot.did")):
            if ECHO:
                say(msg["params"]["command"].split(".")[-1] + " " +
                    str(msg["params"].get("arguments", [""])[0]))
            send({"jsonrpc": "2.0", "id": mid, "result": True})
        elif method == "textDocument/hover":
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "contents": {"kind": "markdown",
                             "value": "**stub** hover, always the same"}}})
        elif method == "textDocument/documentSymbol":
            send({"jsonrpc": "2.0", "id": mid, "result": [
                {"name": "stubSymbol", "kind": 12, "range": rng(15, 0, 18, 1),
                 "selectionRange": rng(15, 0, 15, 10)}]})
        elif mid is not None:
            send({"jsonrpc": "2.0", "id": mid, "result": None})
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(1)
