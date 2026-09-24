#!/usr/bin/env python3
"""mkfix.py - builds the deterministic fixture tree for the mme regression suite.

Everything the suite opens lives under reg/fix, generated from here, so a
checkout of the suite is self contained and byte identical on any machine.
Run:  python mkfix.py          (safe to re-run; it rebuilds from scratch)
"""
import io
import os
import shutil
import stat
import subprocess
import tempfile
import sys


def _chmod_retry(fn, path, exc):
    """git keeps its loose objects read only; Windows refuses to unlink those."""
    os.chmod(path, stat.S_IWRITE)
    fn(path)


def rmtree(path):
    shutil.rmtree(path, onerror=_chmod_retry)

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get("MME_TEST_WORK") or os.path.join(tempfile.gettempdir(), "mme-test")
FIX = os.path.join(BUILD, "fix")
# Nothing outside this folder: every fixture is generated here, so a fresh
# checkout builds byte-identical ones and no third-party file is committed.

# a fixed identity and a fixed timestamp, so git log / blame never move
GIT_ENV = dict(os.environ)
GIT_ENV.update({
    "GIT_AUTHOR_NAME": "Reg Suite",
    "GIT_AUTHOR_EMAIL": "reg@example.com",
    "GIT_COMMITTER_NAME": "Reg Suite",
    "GIT_COMMITTER_EMAIL": "reg@example.com",
    "GIT_AUTHOR_DATE": "2024-01-02T03:04:05+0000",
    "GIT_COMMITTER_DATE": "2024-01-02T03:04:05+0000",
    "GIT_CONFIG_GLOBAL": os.path.join(FIX, "gitconfig-none"),
    "GIT_CONFIG_SYSTEM": os.path.join(FIX, "gitconfig-none"),
})


def w(path, text):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


def long_js(n):
    """one line of minified-looking JavaScript, about n bytes"""
    out, i = [], 0
    while sum(len(p) for p in out) < n:
        i += 1
        out.append("function f%d(a%d,b%d){return a%d*%d+b%d.length;}" % (i, i, i, i, i, i))
    s = "".join(out)
    return "/* generated: one long line */\n" + s[:n] + "\n"


def long_css(n):
    """one line of minified-looking CSS, about n bytes"""
    out, i = [], 0
    while sum(len(p) for p in out) < n:
        i += 1
        out.append(".c%d{margin:%dpx;padding:%dpx;color:#%06x}" % (i, i % 40, i % 20, i * 2654435761 % 0xFFFFFF))
    s = "".join(out)
    return s[:n] + "\n"


def big_c(lines):
    """an ordinary C file of many short lines"""
    out = ["/* generated: an ordinary file of %d lines */\n" % lines,
           "#include <stdio.h>\n", "\n"]
    for i in range(1, lines - 6):
        if i % 20 == 0:
            out.append("\n")
        elif i % 7 == 0:
            out.append("/* step %d of the work */\n" % i)
        else:
            out.append("static int step%04d (int a) { return a + %d; }\n" % (i, i))
    out.append("\nint main (void) {\n  printf(\"%d\\n\", step0001(1));\n  return 0;\n}\n")
    return "".join(out)


def git(repo, *args):
    r = subprocess.run(["git"] + list(args), cwd=repo, env=GIT_ENV,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.stderr.write("git %s failed:\n%s\n" % (" ".join(args), r.stdout.decode("utf8", "replace")))
        raise SystemExit(1)
    return r.stdout.decode("utf8", "replace")


# ---------------------------------------------------------------- languages

C_FILE = """/* edit.c - a small C file the editing scenarios type into. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 64

typedef struct Point {
\tint x;
\tint y;
} Point;

static int total = 0;

/* adds two numbers and remembers the sum */
static int add (int a, int b) {
\ttotal += a + b;
\treturn a + b;
}

static Point make (int x, int y) {
\tPoint p;
\tp.x = x;
\tp.y = y;
\treturn p;
}

int main (int argc, char **argv) {
\tPoint p = make(1, 2);
\tconst char *name = "mme";
\tdouble ratio = 1.5;
\tint i;
\tfor (i = 0; i < MAXN; i++) {
\t\tif (i % 2 == 0) continue;
\t\ttotal = add(total, i);
\t}
\tprintf("%s %d %d %f %d\\n", name, p.x, p.y, ratio, argc);
\treturn argv == NULL ? 1 : 0;
}
"""

GO_FILE = """// Package demo is the Go colour fixture.
package demo

import (
\t"errors"
\t"fmt"
\t"strings"
)

// ErrEmpty is returned for an empty name.
var ErrEmpty = errors.New("empty name")

const Greeting = "hello"

type Person struct {
\tName string
\tAge  int
}

func (p Person) String() string {
\treturn fmt.Sprintf("%s (%d)", p.Name, p.Age)
}

func Greet(name string) (string, error) {
\tif strings.TrimSpace(name) == "" {
\t\treturn "", ErrEmpty
\t}
\tfor i := 0; i < 3; i++ {
\t\tname = strings.ToUpper(name)
\t}
\treturn Greeting + " " + name, nil
}
"""

TSX_FILE = """import React, { useState, useEffect } from "react";

export interface CardProps {
  title: string;
  count: number;
  onPick?: (id: number) => void;
}

type Mode = "list" | "grid";

const DEFAULT_MODE: Mode = "grid";

export function Card({ title, count, onPick }: CardProps) {
  const [mode, setMode] = useState<Mode>(DEFAULT_MODE);
  const [open, setOpen] = useState(false);

  useEffect(() => {
    if (count > 10) setOpen(true);
  }, [count]);

  return (
    <div className="card" data-mode={mode}>
      <h2>{title}</h2>
      <span>{count} items</span>
      <button onClick={() => setMode(mode === "grid" ? "list" : "grid")}>
        toggle
      </button>
      {open && <p>many</p>}
    </div>
  );
}

export default Card;
"""

JS_FILE = """// app.js - the JS colour and highlight-invalidation fixture.
const NAME = "mme";
let count = 0;

/* a block comment */
function greet(who) {
  const text = `hello ${who}`;
  count += 1;
  return text;
}

class Box {
  constructor(w, h) {
    this.w = w;
    this.h = h;
  }
  area() {
    return this.w * this.h;
  }
}

const list = [1, 2, 3].map((n) => n * 2);
const conf = { name: NAME, list, deep: { on: true, off: false, n: null } };

export { greet, Box, conf };
"""

CSS_FILE = """/* site.css - the CSS colour fixture. */
:root {
  --bg: #1e1e1e;
  --fg: rgb(212, 212, 212);
  --gap: 8px;
}

body {
  background: var(--bg);
  color: var(--fg);
  font-family: "Segoe UI", sans-serif;
  margin: 0;
}

.card > h2,
.card span {
  padding: var(--gap) 12px;
  border: 1px solid #333;
}

@media (max-width: 600px) {
  .card {
    display: none !important;
  }
}
"""

MD_FILE = """# mme regression fixture

A paragraph with **bold**, *italic*, `code` and a [link](https://example.com).

## A list

- one
- two
  - nested
1. first
2. second

> a quote

```c
int main (void) {
    return 0;
}
```

| col a | col b |
| ----- | ----- |
| 1     | 2     |

---

The end.
"""

JSON_FILE = """{
  "name": "fixture",
  "version": "1.0.0",
  "private": true,
  "count": 42,
  "ratio": 1.5,
  "on": true,
  "off": false,
  "nothing": null,
  "tags": ["red", "green", "blue"],
  "nested": {
    "a": 1,
    "b": [2, 3],
    "c": { "d": "deep" }
  }
}
"""

PY_FILE = '''"""util.py - a Python fixture."""
import os
import sys


CONST = 3.14


class Shape:
    """A shape."""

    def __init__(self, sides: int = 3):
        self.sides = sides

    def describe(self) -> str:
        return f"shape with {self.sides} sides"


def main(argv=None):
    argv = argv or sys.argv[1:]
    for i, a in enumerate(argv):
        print(i, a, os.path.basename(a))
    return 0
'''

FOLD_FILE = """#include <stdio.h>

/* fold.c - blocks to fold and unfold. */

static int one (void) {
\tint a = 1;
\tint b = 2;
\tif (a < b) {
\t\ta = b;
\t\tb = 0;
\t}
\treturn a + b;
}

static int two (void) {
\tint i, s = 0;
\tfor (i = 0; i < 10; i++) {
\t\ts += i;
\t\ts *= 2;
\t}
\treturn s;
}

int main (void) {
\tprintf("%d %d\\n", one(), two());
\treturn 0;
}
"""

WRAP_FILE = (
    "// wrap.js - one very long logical line and a few short ones.\n"
    "const sentence = \""
    + " ".join("word%02d" % i for i in range(40))
    + "\";\n"
    "const short = 1;\n"
    "const another = \"" + " ".join("tok%02d" % i for i in range(40)) + "\";\n"
    "const done = true;\n"
)


def sticky_file():
    """A deeply nested function long enough that scrolling leaves its header
    above the top of the editor: what editor.stickyScroll.enabled pins there."""
    out = ["#include <stdio.h>\n", "\n", "/* sticky.c - nested scopes taller than the screen. */\n", "\n"]
    out.append("int outer (int n) {\n")
    out.append("\tint total = 0;\n")
    out.append("\tif (n > 0) {\n")
    out.append("\t\tint i;\n")
    out.append("\t\tfor (i = 0; i < n; i++) {\n")
    out.append("\t\t\tint j;\n")
    out.append("\t\t\tfor (j = 0; j < n; j++) {\n")
    for k in range(40):
        out.append("\t\t\t\ttotal += %d * i + j;\n" % k)
    out.append("\t\t\t}\n")
    out.append("\t\t}\n")
    out.append("\t}\n")
    out.append("\treturn total;\n")
    out.append("}\n")
    out.append("\n")
    out.append("int main (void) {\n")
    out.append("\tprintf(\"%d\\n\", outer(3));\n")
    out.append("\treturn 0;\n")
    out.append("}\n")
    return "".join(out)


def build_tree():
    if os.path.isdir(FIX):
        rmtree(FIX)
    os.makedirs(FIX)
    w(os.path.join(FIX, "gitconfig-none"), "")

    lang = os.path.join(FIX, "lang")
    w(os.path.join(lang, "edit.c"), C_FILE)
    w(os.path.join(lang, "demo.go"), GO_FILE)
    w(os.path.join(lang, "card.tsx"), TSX_FILE)
    w(os.path.join(lang, "app.js"), JS_FILE)
    w(os.path.join(lang, "site.css"), CSS_FILE)
    w(os.path.join(lang, "doc.md"), MD_FILE)
    w(os.path.join(lang, "conf.json"), JSON_FILE)
    w(os.path.join(lang, "util.py"), PY_FILE)
    w(os.path.join(lang, "fold.c"), FOLD_FILE)
    w(os.path.join(lang, "sticky.c"), sticky_file())
    w(os.path.join(lang, "wrap.js"), WRAP_FILE)

    # a folder for the explorer, tabs, go-to-file and search scenarios
    proj = os.path.join(FIX, "proj")
    w(os.path.join(proj, "main.c"), C_FILE.replace("edit.c", "main.c"))
    w(os.path.join(proj, "app.js"), JS_FILE.replace("app.js", "app.js"))
    w(os.path.join(proj, "site.css"), CSS_FILE)
    w(os.path.join(proj, "readme.md"), MD_FILE)
    w(os.path.join(proj, "package.json"), JSON_FILE)
    w(os.path.join(proj, "src", "alpha.c"), "/* alpha */\nint alpha (void) { return 1; }\n")
    w(os.path.join(proj, "src", "beta.c"), "/* beta */\nint beta (void) { return 2; }\n")
    w(os.path.join(proj, "src", "gamma.h"), "#ifndef GAMMA_H\n#define GAMMA_H\nint gamma_v (void);\n#endif\n")
    w(os.path.join(proj, "docs", "guide.md"), "# Guide\n\nNeedleWord lives here too.\n")
    # a deep single-child chain, for explorer.compactFolders
    w(os.path.join(proj, "deep", "one", "two", "three.txt"), "three\n")
    # enough rows that the explorer tree scrolls and shows a scrollbar
    for i in range(40):
        w(os.path.join(proj, "many", "file%02d.txt" % i), "row %02d NeedleWord\n" % i)

    # The long-line fixtures. What matters is the shape, not the content: a
    # line in the 8000..20000 byte window is where the tokenizer used to
    # re-run every frame, and the two huge ones are past the point where it
    # gives up. Generated, so they are the same on any machine.
    mini = os.path.join(FIX, "mini")
    os.makedirs(mini)
    w(os.path.join(mini, "md5.js"), long_js(12756))
    w(os.path.join(mini, "boot.min.css"), long_css(190000))
    w(os.path.join(mini, "found.min.js"), long_js(531000))
    w(os.path.join(mini, "big.c"), big_c(1400))
    # a generated file whose longest line sits in the 8000..20000 byte window
    parts = []
    parts.append("// mid.js - one line of about 12000 bytes, the window of the old hang.\n")
    body = ";".join("var v%04d=function(a,b){return a*%d+b};" % (i, i) for i in range(260))
    parts.append(body + "\n")
    parts.append("var tail = 1;\n")
    w(os.path.join(mini, "mid.js"), "".join(parts))

    build_git()
    print("fixtures in", FIX)


GIT_BASE_C = """#include <stdio.h>

int keep_one (void) {
\treturn 1;
}

int keep_two (void) {
\treturn 2;
}

int changed_here (void) {
\treturn 3;
}

int deleted_below (void) {
\treturn 4;
}

int last (void) {
\treturn 5;
}

int main (void) {
\tprintf("%d\\n", keep_one() + keep_two() + changed_here() + last());
\treturn 0;
}
"""

GIT_WORK_C = """#include <stdio.h>

int keep_one (void) {
\treturn 1;
}

int keep_two (void) {
\treturn 2;
}

int changed_here (void) {
\treturn 33;
}

int added_function (void) {
\treturn 99;
}

int last (void) {
\treturn 5;
}

int main (void) {
\tprintf("%d\\n", keep_one() + keep_two() + changed_here() + last());
\treturn 0;
}
"""


def build_git():
    repo = os.path.join(FIX, "gitrepo")
    os.makedirs(repo)
    git(repo, "init", "-q")
    git(repo, "config", "user.name", "Reg Suite")
    git(repo, "config", "user.email", "reg@example.com")
    git(repo, "config", "commit.gpgsign", "false")
    git(repo, "symbolic-ref", "HEAD", "refs/heads/main")

    w(os.path.join(repo, "tracked.c"), GIT_BASE_C)
    w(os.path.join(repo, "notes.md"), "# Notes\n\nline one\nline two\nline three\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "first commit: tracked.c and notes.md")

    w(os.path.join(repo, "notes.md"), "# Notes\n\nline one\nline two changed\nline three\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "second commit: notes.md reworded")

    w(os.path.join(repo, "third.c"), "int third (void) { return 3; }\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "third commit: add third.c")

    # the working tree, left dirty: a change, an addition, a deletion, a new file
    w(os.path.join(repo, "tracked.c"), GIT_WORK_C)
    w(os.path.join(repo, "untracked.txt"), "brand new\n")
    print(git(repo, "log", "--oneline").strip())

    # A repo of its own, so the scenarios above keep their row positions. Its
    # file's first line is a comment starting "--", and in a unified diff a
    # deleted one reads "--- the first ...", which a parser can mistake for
    # the diff's own "---" header and drop.
    dash = os.path.join(FIX, "dashrepo")
    os.makedirs(dash)
    git(dash, "init", "-q")
    git(dash, "config", "user.name", "Reg Suite")
    git(dash, "config", "user.email", "reg@example.com")
    git(dash, "config", "commit.gpgsign", "false")
    git(dash, "symbolic-ref", "HEAD", "refs/heads/main")
    w(os.path.join(dash, "lead.lua"),
      "-- the first comment line\nlocal x = 1\nlocal y = 2\nprint(x + y)\nreturn x\n")
    git(dash, "add", "-A")
    git(dash, "commit", "-q", "-m", "first commit: lead.lua")
    w(os.path.join(dash, "lead.lua"),
      "-- the FIRST comment line, edited\nlocal x = 1\nlocal y = 2\nprint(x + y)\nreturn x\n")

    # A repo of its own again: a line whose one word changed, with another
    # line inserted right above it. A diff prints everything that went and
    # then everything that came, so pairing the two sides in order pairs the
    # changed line with the inserted one, finds nothing in common and paints
    # both whole; the changed word has to stay the only brighter part.
    word = os.path.join(FIX, "wordrepo")
    os.makedirs(word)
    git(word, "init", "-q")
    git(word, "config", "user.name", "Reg Suite")
    git(word, "config", "user.email", "reg@example.com")
    git(word, "config", "commit.gpgsign", "false")
    git(word, "symbolic-ref", "HEAD", "refs/heads/main")
    w(os.path.join(word, "pair.c"),
      "int total (int a, int b) {\n\treturn added(a, b);\n}\n")
    git(word, "add", "-A")
    git(word, "commit", "-q", "-m", "first commit: pair.c")
    w(os.path.join(word, "pair.c"),
      "int total (int a, int b) {\n\tcounted(a);\n\treturn added(a, c);\n}\n")

    # one conflict with an empty side: it needs no resolving, and the merge
    # editor used to take that as licence to write its result over the file
    # when the editor simply quit
    w(os.path.join(FIX, "odd", "selfmerge.c"),
      "<<<<<<< A\nx\n=======\n>>>>>>> B\n")

    # UTF-16: a NUL in the first bytes made file_is_binary() call it binary,
    # so it opened read-only in the hex editor although ebuf.c decodes it
    io.open(os.path.join(FIX, "odd", "utf16le.txt"), "wb").write(
        b"\xff\xfe" + "".join("utf-16 line %02d\n" % i
                               for i in range(1, 25)).encode("utf-16-le"))

    # 400 KB on one line, of words that repeat: the word under the cursor has
    # tens of thousands of occurrences and "fox" matches 9091 times, so every
    # per-match column walk shows up
    w(os.path.join(FIX, "mini", "one400k.txt"),
      "the quick brown fox jumps over the lazy dog " * 9091 + "needle\n")

    # turn quadratic
    w(os.path.join(FIX, "mini", "lines20k.txt"),
      "".join("line %05d of the file, a few words on it\n" % i
              for i in range(20000)))

    # one long line of a's: (a+)$ backtracks over it
    w(os.path.join(FIX, "odd", "aaa.txt"), "a" * 60000 + "b\n")

    # a file of no language at all: nothing fills the token buffer for it, so
    # the minimap is where an uninitialised read shows up as stray colours
    w(os.path.join(FIX, "lang", "plain.txt"),
      "".join("plain text line %02d with several words on it\n" % i for i in range(1, 41)))


if __name__ == "__main__":
    build_tree()
