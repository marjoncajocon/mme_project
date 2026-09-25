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
    --auth=stuck    nobody signed in, and the device flow is never finished:
                    the code expires in a second and the command signIn names
                    is never answered, the way a flow ends when the browser
                    tab is closed
    --auth=slow     nobody signed in, and signIn itself takes three seconds to
                    answer, saying out loud how many times it was asked

It also takes --name=x, which puts "x: " in front of every diagnostic, so two
of these can be told apart when a scenario has one per language.

And four flags for the newer surfaces, each off by default so every other
scenario sees exactly what it always saw:

    --pull      diagnosticProvider instead of publishDiagnostics: the problems
                are only there when asked for (textDocument/diagnostic). The
                first pull of a file is "full", with a related document; the
                second, against the resultId of the first, is "unchanged";
                2.5 s later the server sends workspace/diagnostic/refresh and
                the pull after that is "full" again, with other problems
    --colors    colorProvider: the colors of fix/lsp/colors.c by position, and
                colorPresentation's two ways to write one (hex and rgb())
    --links     documentLinkProvider with resolveProvider: the links of
                fix/lsp/links.c by position; the one to renamer/main.c has no target
                until documentLink/resolve gives it one
    --rename    workspace.fileOperations willRename/didRename for **/*.h: the
                rename of a header edits the #include on line 2 of main.c
                beside it, and didRenameFiles is said back as a message


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
NAME = ""                        # --name=x: every diagnostic says "x: ..."
for _a in sys.argv[1:]:
    if _a.startswith("--auth="):
        AUTH = _a.split("=", 1)[1]
    elif _a.startswith("--name="):
        NAME = _a.split("=", 1)[1]
PULL = "--pull" in sys.argv[1:]
COLORS = "--colors" in sys.argv[1:]
LINKS = "--links" in sys.argv[1:]
RENAME = "--rename" in sys.argv[1:]

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

# --pull: what textDocument/diagnostic reports, by how many times a file was
# asked (the second is "unchanged", against the first's resultId)
PULLED_FIRST = [(12, 11, 12, 16, "pulled: total is never read", 1),
                (30, 1, 30, 8, "pulled: prefer a const pointer here", 2)]
PULLED_RELATED = [(2, 0, 2, 8, "pulled: a related document's problem", 2)]
PULLED_AFTER_REFRESH = [(16, 1, 16, 6, "pulled again after workspace/diagnostic/refresh", 1)]
PULLS = {}                       # uri -> how many times it was asked
REFRESH_WAIT = 2.5               # seconds after the unchanged pull the refresh comes

# --colors: fix/lsp/colors.c's colors, (line, start, end, r, g, b, a)
COLOR_AT = [(1, 19, 26, 1, 0, 0, 1), (2, 21, 28, 0, 1, 0, 1),
            (3, 20, 34, 0, 0, 1, 1), (4, 21, 30, 1, 0, 0, 0.5)]

# --links: fix/lsp/links.c's links, (line, start, end, target or None, fragment)
LINK_AT = [(1, 9, 19, "colors.c", "#L4,14"),
           (2, 9, 32, "https://example.com/mme", ""),
           (3, 9, 25, None, "#L4")]     # its target comes from documentLink/resolve


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


SIGNIN_SEEN = [0]                # --auth=slow counts the signIn requests
SLOW_WAIT = 3.0                  # seconds --auth=slow takes to answer signIn


def slow_signin(mid):
    """--auth=slow: signIn itself is what takes its time, so a second Sign In
    goes out while the first has no answer yet"""
    time.sleep(SLOW_WAIT)
    send({"jsonrpc": "2.0", "id": mid, "result": {
        "status": "PromptUserDeviceFlow", "userCode": DEVICE_CODE,
        "verificationUri": DEVICE_URI, "expiresIn": 899, "interval": 5,
        "command": {"command": "github.copilot.finishDeviceFlow",
                    "arguments": []}}})


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
               "source": src, "code": code,
               "message": (NAME + ": " + msg) if NAME else msg}
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


def diag(a, b, c, d, msg, sev):
    return {"range": rng(a, b, c, d), "severity": sev, "source": "stub",
            "message": msg}


def sibling(uri, name):
    """the uri of name in the folder of uri (or, with ../, beside it)"""
    return uri.rsplit("/", 1)[0] + "/" + name


def pulled(mid, params):
    """--pull: textDocument/diagnostic, full, then unchanged, then (after the
    refresh this sends) full again"""
    uri = params["textDocument"]["uri"]
    n = PULLS.get(uri, 0) + 1
    PULLS[uri] = n
    if n == 1:
        send({"jsonrpc": "2.0", "id": mid, "result": {
            "kind": "full", "resultId": "r1",
            "items": [diag(*d) for d in PULLED_FIRST],
            "relatedDocuments": {sibling(uri, "fold.c"): {
                "kind": "full", "resultId": "rel1",
                "items": [diag(*d) for d in PULLED_RELATED]}}}})
    elif n == 2 and params.get("previousResultId") == "r1":
        send({"jsonrpc": "2.0", "id": mid,
              "result": {"kind": "unchanged", "resultId": "r1"}})
        threading.Thread(target=refresh_later, daemon=True).start()
    elif n == 2:
        # the client forgot the resultId: say so where the screen shows it
        send({"jsonrpc": "2.0", "id": mid, "result": {"kind": "full", "items": [
            diag(0, 0, 0, 1, "pulled: previousResultId was not sent", 1)]}})
    else:
        send({"jsonrpc": "2.0", "id": mid, "result": {
            "kind": "full", "resultId": "r%d" % n,
            "items": [diag(*d) for d in PULLED_AFTER_REFRESH]}})


def refresh_later():
    time.sleep(REFRESH_WAIT)
    send({"jsonrpc": "2.0", "id": "refresh-1",
          "method": "workspace/diagnostic/refresh", "params": None})


def color_pres(mid, params):
    """--colors: the color as hex and as rgb(), each replacing the range"""
    c = params["color"]
    r, g, b = (int(round(c[k] * 255)) for k in ("red", "green", "blue"))
    a = c.get("alpha", 1)
    hexa = "#%02x%02x%02x" % (r, g, b) + ("" if a >= 1 else "%02x" % int(round(a * 255)))
    rgb = "rgb(%d, %d, %d)" % (r, g, b) if a >= 1 else "rgba(%d, %d, %d, %g)" % (r, g, b, a)
    send({"jsonrpc": "2.0", "id": mid, "result": [
        {"label": lab, "textEdit": {"range": params["range"], "newText": lab}}
        for lab in (hexa, rgb)]})


def main():
    while True:
        msg = read()
        if msg is None:
            return 0
        method = msg.get("method")
        mid = msg.get("id")
        if method is None:
            pass                 # the answer to a request of ours (the refresh)
        elif method == "initialize":
            caps = dict(CAPS)
            if PULL:
                caps["diagnosticProvider"] = {"identifier": "stub", "interFileDependencies": True,
                                              "workspaceDiagnostics": False}
            if COLORS:
                caps["colorProvider"] = True
            if LINKS:
                caps["documentLinkProvider"] = {"resolveProvider": True}
            if RENAME:
                ops = {"filters": [{"scheme": "file",
                                    "pattern": {"glob": "**/*.{h,hpp}", "matches": "file"}}]}
                caps["workspace"] = {"fileOperations": {"willRename": ops, "didRename": ops}}
            send({"jsonrpc": "2.0", "id": mid,
                  "result": {"capabilities": caps,
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
            SIGNIN_SEEN[0] += 1
            if AUTH == "slow":
                # answered from a thread, three seconds late, and it says out
                # loud how many times it was asked: a second Sign In while the
                # first is still out must never reach here
                say("signIn asked %d time(s)" % SIGNIN_SEEN[0])
                threading.Thread(target=slow_signin, args=(mid,), daemon=True).start()
            elif SIGNED_IN:
                send({"jsonrpc": "2.0", "id": mid,
                      "result": {"status": "AlreadySignedIn", "user": USER}})
            else:
                send({"jsonrpc": "2.0", "id": mid, "result": {
                    "status": "PromptUserDeviceFlow",
                    "userCode": DEVICE_CODE,
                    "verificationUri": DEVICE_URI,
                    # stuck: the code expires in a second and the command it
                    # names is never answered, the way a device flow ends when
                    # the user closes the browser tab
                    "expiresIn": 1 if AUTH == "stuck" else 899, "interval": 5,
                    "command": {"command": "github.copilot.neverFinishes"
                                if AUTH == "stuck" else
                                "github.copilot.finishDeviceFlow",
                                "arguments": []}}})
        elif method == "signOut":
            globals()["SIGNED_IN"] = False
            send({"jsonrpc": "2.0", "id": mid, "result": {"status": "NotSignedIn"}})
            status("Inactive", "Sign in to use Copilot")
        elif method == "workspace/executeCommand" and \
                msg["params"].get("command") == "github.copilot.neverFinishes":
            pass                 # --auth=stuck: no answer, ever
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
            if not PULL:         # --pull: only when asked
                publish(uri)
        elif method == "textDocument/diagnostic":
            pulled(mid, msg["params"])
        elif method == "textDocument/documentColor":
            send({"jsonrpc": "2.0", "id": mid, "result": [
                {"range": rng(l, a, l, b),
                 "color": {"red": r, "green": g, "blue": bl, "alpha": al}}
                for l, a, b, r, g, bl, al in COLOR_AT]})
        elif method == "textDocument/colorPresentation":
            color_pres(mid, msg["params"])
        elif method == "textDocument/documentLink":
            uri = msg["params"]["textDocument"]["uri"]
            links = []
            for l, a, b, target, frag in LINK_AT:
                lk = {"range": rng(l, a, l, b)}
                if target and "://" in target:
                    lk["target"] = target
                elif target:
                    lk["target"] = sibling(uri, target) + frag
                else:
                    lk["data"] = {"frag": frag, "uri": uri}
                links.append(lk)
            send({"jsonrpc": "2.0", "id": mid, "result": links})
        elif method == "documentLink/resolve":
            lk = dict(msg["params"])
            lk["target"] = sibling(lk["data"]["uri"], "renamer/main.c") + lk["data"]["frag"]
            send({"jsonrpc": "2.0", "id": mid, "result": lk})
        elif method == "workspace/willRenameFiles":
            f = msg["params"]["files"][0]
            new = f["newUri"].rsplit("/", 1)[1]
            main_c = sibling(f["oldUri"], "main.c")
            send({"jsonrpc": "2.0", "id": mid, "result": {"changes": {
                main_c: [{"range": rng(1, 10, 1, 16), "newText": new}]}}})
        elif method == "workspace/didRenameFiles":
            f = msg["params"]["files"][0]
            say("didRenameFiles %s -> %s" % (f["oldUri"].rsplit("/", 1)[1],
                                             f["newUri"].rsplit("/", 1)[1]))
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
