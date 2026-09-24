#!/usr/bin/env python3
"""mkprof.py - makes ptree/ from tree/ by adding the event-loop tick profiler.

tree/ is a plain copy of the editor sources. ptree/ is the same sources with a
few lines around the `while (!E.quit)` loop in main(): any iteration that costs
more than 20 ms appends a line to the file named by $MME_TICKLOG, split into
the polls, the draw and the rest. Nothing else changes, so the two builds draw
identically and a perf scenario's golden screen is the same either way.

Run it after copying a new version of the editor into tree/:

    python mkprof.py && python run.py --build
"""
import io
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.join(HERE, "tree")
PTREE = os.path.join(HERE, "ptree")

HELPER = '''
/* --- the regression suite's tick profiler; not a part of mme --- */
static const char *tick_log_path (void) {
  const char *e = getenv("MME_TICKLOG");
  return (e && *e) ? e : NULL;
}
static void tick_log_reset (void) {
  const char *e = tick_log_path();
  if (e) { FILE *pf = fopen(e, "w"); if (pf) fclose(pf); }
}
static void tick_log (const char *kind, long long polls, long long draw, long long rest, long long total) {
  const char *e = tick_log_path();
  FILE *pf;
  if (!e) return;
  pf = fopen(e, "a");
  if (!pf) return;
  fprintf(pf, "%s tick: total %lld  polls %lld  draw %lld  rest %lld us%c", kind, total, polls, draw, rest, 10);
  fclose(pf);
}
/* --- end of the tick profiler --- */
'''

EDITS = [
    ("  check_size();\n  layout();\n  if (line > 0 && HAS_DOC && !T->page) {",
     "  check_size();\n  layout();\n  tick_log_reset();\n  if (line > 0 && HAS_DOC && !T->page) {"),
    ("  while (!E.quit) {\n    int k;\n    lsp_poll();",
     "  while (!E.quit) {\n    int k;\n    long long q0, q1, q2, q3, q4, q5;\n"
     "    q0 = os_now_us();\n    lsp_poll();"),
    ("    draw();\n    k = term_key(",
     "    q1 = os_now_us();\n    draw();\n    q2 = os_now_us();\n    k = term_key("),
    ("    quickfix_idle();\n    if (k == K_NONE) {",
     "    q3 = os_now_us();\n    quickfix_idle();\n    if (k == K_NONE) {"),
    ("      work_idle();\n      continue;\n    }",
     "      work_idle();\n      q4 = os_now_us();\n"
     "      if (q4 - q0 - (q3 - q2) > 20000)\n"
     "        tick_log(\"idle \", q1 - q0, q2 - q1, q4 - q3, q4 - q0 - (q3 - q2));\n"
     "      continue;\n    }"),
    ("    autosave_focus();\n  }\n  search_stop();",
     "    autosave_focus();\n    q5 = os_now_us();\n"
     "    if (q5 - q0 - (q3 - q2) > 20000)\n"
     "      tick_log(k == K_MOUSE ? \"mouse\" : \"key  \", q1 - q0, q2 - q1, q5 - q3, q5 - q0 - (q3 - q2));\n"
     "  }\n  search_stop();"),
]


def main():
    if not os.path.isdir(TREE):
        raise SystemExit("tree/ is missing")
    if os.path.isdir(PTREE):
        shutil.rmtree(PTREE)
    os.makedirs(PTREE)
    for name in os.listdir(TREE):
        src = os.path.join(TREE, name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(PTREE, name))

    p = os.path.join(PTREE, "mme.c")
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    for old, new in EDITS:
        n = s.count(old)
        if n != 1:
            sys.stderr.write("the main loop moved: %d matches for\n%s\n" % (n, old))
            raise SystemExit(1)
        s = s.replace(old, new)
    at = s.rfind("\nint main ", 0, s.find("  while (!E.quit) {"))
    if at < 0:
        raise SystemExit("main() not found")
    s = s[:at] + "\n" + HELPER + s[at:]
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    print("ptree/ made from tree/, with the tick profiler")


if __name__ == "__main__":
    main()
