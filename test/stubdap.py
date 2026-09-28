#!/usr/bin/env python3
"""stubdap.py - a fixed, fake debug adapter (DAP) for the regression suite.

mme starts it from settings.json ("mme.debugAdapters": {"stub": "python <this
file>"}) for a launch configuration of type "stub". It debugs nothing: it
answers the Debug Adapter Protocol over stdin/stdout with the same things every
time, so what mme draws from a session can be pinned.

    initialize      the capabilities the newer surfaces need: completions,
                    disassemble, readMemory, writeMemory, steppingGranularity
                    (--no-write-memory: without supportsWriteMemoryRequest)
    launch          "program" is the file shown as the stopped frame's source;
                    "runInTerminal": true asks mme (the reverse request) to run
                    "python -c ..." in its terminal - a program that reads a
                    line and says it back - and says in the console what mme
                    answered
    configurationDone   a stop on entry, thread 1, line 2 of the program,
                    instruction pointer 0x1000
    variables       count = 42 (memoryReference 0x2000), counter = the byte
                    at 0x2015 (7 until writeMemory changes it)
    evaluate        "= <the expression>"
    completions     count, counter, compute: those that start with the word
    disassemble     instructions 4 bytes apart around the pointer, of "main"
    readMemory      a fake 4 GB address space: "Hello, memory!" and the bytes
                    0..49 at 0x2000, the stops counted at 0x2040 (a step
                    changes it), elsewhere (a ^ a >> 8) & 255; unreadable
                    (unreadableBytes): below 0x1000, the hole 0x2800..0x3100,
                    and from 4 GB up
    writeMemory     the bytes are kept (a readMemory after shows them); an
                    error for bytes in the holes (not mapped) and in
                    0x1000..0x2000 (the code, read-only)
    next / stepIn   a stop again: the pointer 4 bytes on when the granularity
                    is "instruction", else the next line
    continue        exited 0, terminated
"""
from __future__ import print_function

import base64
import json
import sys

out = sys.stdout.buffer if hasattr(sys.stdout, "buffer") else sys.stdout
inp = sys.stdin.buffer if hasattr(sys.stdin, "buffer") else sys.stdin
seq = [0]
state = {"program": "", "line": 2, "ip": 0x1000, "name": "stub", "stops": 0}

MEM_TOP = 1 << 32
HOLES = [(0, 0x1000), (0x2800, 0x3100)]
HELLO = b"Hello, memory!" + bytes(range(50))
CODE = (0x1000, 0x2000)
written = {}  # address: the byte writeMemory put there


def readable(a):
    return a < MEM_TOP and not any(lo <= a < hi for lo, hi in HOLES)


def mem_byte(a):
    if a in written:
        return written[a]
    if 0x2000 <= a < 0x2000 + len(HELLO):
        return HELLO[a - 0x2000]
    if a == 0x2040:
        return state["stops"] & 255
    return (a ^ (a >> 8)) & 255


def read_memory(start, count):
    """the bytes readable from start (up to count), and how many unreadable follow them"""
    data = bytearray()
    a = start
    while a < start + count and readable(a):
        data.append(mem_byte(a))
        a += 1
    skip = 0
    while a < start + count and not readable(a):
        skip += 1
        a += 1
    return bytes(data), skip


def send(msg):
    seq[0] += 1
    msg["seq"] = seq[0]
    body = json.dumps(msg).encode("utf-8")
    out.write(b"Content-Length: %d\r\n\r\n" % len(body))
    out.write(body)
    out.flush()


def event(name, body=None):
    send({"type": "event", "event": name, "body": body or {}})


def respond(req, body=None, success=True, message=None):
    m = {"type": "response", "request_seq": req["seq"], "command": req["command"], "success": success,
         "body": body or {}}
    if message:
        m["message"] = message
    send(m)


def read():
    n = None
    while True:
        line = inp.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            if n is not None:
                break
            continue
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1])
    return json.loads(inp.read(n).decode("utf-8"))


def stopped(reason):
    event("stopped", {"reason": reason, "threadId": 1, "allThreadsStopped": True})


def main():
    while True:
        m = read()
        if m is None:
            return
        if m.get("type") == "response":  # runInTerminal's answer
            event("output", {"category": "console", "output": "runInTerminal: %s\n" %
                             ("ok" if m.get("success") else "failed: " + str(m.get("message")))})
            continue
        c, a = m.get("command"), m.get("arguments") or {}
        if c == "initialize":
            respond(m, {"supportsConfigurationDoneRequest": True, "supportsCompletionsRequest": True,
                        "completionTriggerCharacters": ["."], "supportsDisassembleRequest": True,
                        "supportsReadMemoryRequest": True, "supportsSteppingGranularity": True,
                        "supportsSetVariable": True,
                        "supportsWriteMemoryRequest": "--no-write-memory" not in sys.argv})
            event("initialized")
        elif c == "launch":
            state["program"] = a.get("program", "")
            state["name"] = a.get("name", "stub")
            respond(m)
            event("output", {"category": "console", "output": "stub adapter: launched %s\n" % state["name"]})
            if a.get("runInTerminal"):
                send({"type": "request", "command": "runInTerminal",
                      "arguments": {"kind": "integrated", "title": "stub program", "cwd": a.get("cwd", "."),
                                    "args": [sys.executable, "-c",
                                             "print('name? ', end='', flush=True); print('hello ' + input())"]}})
        elif c == "configurationDone":
            respond(m)
            stopped("entry")
        elif c == "threads":
            respond(m, {"threads": [{"id": 1, "name": "main"}]})
        elif c == "stackTrace":
            respond(m, {"stackFrames": [{"id": 1, "name": "main", "line": state["line"], "column": 1,
                                         "source": {"path": state["program"], "name": "prog.py"},
                                         "instructionPointerReference": "0x%x" % state["ip"]}],
                        "totalFrames": 1})
        elif c == "scopes":
            respond(m, {"scopes": [{"name": "Locals", "variablesReference": 10, "expensive": False}]})
        elif c == "variables":
            respond(m, {"variables": [{"name": "count", "value": "42", "variablesReference": 0,
                                       "memoryReference": "0x2000"},
                                      {"name": "counter", "value": str(mem_byte(0x2015)), "variablesReference": 0}]})
        elif c == "evaluate":
            respond(m, {"result": "= " + a.get("expression", ""), "variablesReference": 0})
        elif c == "completions":
            text = a.get("text", "")
            col = a.get("column", len(text) + 1) - 1
            w = col
            while w > 0 and (text[w - 1].isalnum() or text[w - 1] == "_"):
                w -= 1
            word = text[w:col]
            names = [x for x in ("count", "counter", "compute") if x.startswith(word)]
            respond(m, {"targets": [{"label": x, "type": "variable"} for x in names]})
        elif c == "disassemble":
            base = int(a.get("memoryReference", "0"), 0) + 4 * int(a.get("instructionOffset", 0))
            ins = []
            for k in range(int(a.get("instructionCount", 10))):
                addr = base + 4 * k
                d = {"address": "0x%08x" % addr, "instructionBytes": "48 89 %02x %02x" % (k & 255, addr & 255),
                     "instruction": "mov r%d, %d" % (k % 8, k), "symbol": "main"}
                if addr >= 0x1000 and addr < 0x1000 + 16:
                    d["location"] = {"path": state["program"], "name": "prog.py"}
                    d["line"] = 2 + (addr - 0x1000) // 8
                ins.append(d)
            respond(m, {"instructions": ins})
        elif c == "readMemory":
            start = (int(a.get("memoryReference", "0"), 0) + int(a.get("offset", 0))) & ((1 << 64) - 1)
            data, skip = read_memory(start, int(a.get("count", 0)))
            body = {"address": "0x%x" % start, "data": base64.b64encode(data).decode("ascii")}
            if skip:
                body["unreadableBytes"] = skip
            respond(m, body)
        elif c == "writeMemory":
            start = (int(a.get("memoryReference", "0"), 0) + int(a.get("offset", 0))) & ((1 << 64) - 1)
            data = base64.b64decode(a.get("data", ""))
            bad = [start + k for k in range(len(data)) if not readable(start + k)]
            code = [start + k for k in range(len(data)) if CODE[0] <= start + k < CODE[1]]
            if bad:
                respond(m, success=False, message="Unable to write memory at 0x%x: the address is not mapped" % bad[0])
            elif code:
                respond(m, success=False, message="Unable to write memory at 0x%x: the page is read-only" % code[0])
            else:
                for k, x in enumerate(bytearray(data)):
                    written[start + k] = x
                respond(m, {"bytesWritten": len(data)})
        elif c in ("next", "stepIn", "stepOut"):
            respond(m)
            state["stops"] += 1
            if a.get("granularity") == "instruction":
                state["ip"] += 4
            else:
                state["line"] += 1
                state["ip"] += 8
            event("continued", {"threadId": 1})
            stopped("step")
        elif c == "continue":
            respond(m, {"allThreadsContinued": True})
            event("exited", {"exitCode": 0})
            event("terminated")
        elif c == "disconnect":
            respond(m)
            return
        else:
            respond(m)


if __name__ == "__main__":
    main()
