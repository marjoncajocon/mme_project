#!/usr/bin/env python3
"""try.py - scratch driver used while writing scenarios.
   python try.py [-p] <target> <steps...>
   -p uses the instrumented build and prints the tick log afterwards."""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
S = os.path.dirname(HERE)
HARNESS = os.path.join(S, "harness.exe")

args = sys.argv[1:]
prof = False
if args and args[0] == "-p":
    prof = True
    args = args[1:]
src = os.path.join(HERE, "pbin" if prof else "bin", "mme.exe")
d = os.path.join(HERE, "work", "_x")
if os.path.isdir(d):
    shutil.rmtree(d)
os.makedirs(os.path.join(d, "mme-data"))
shutil.copyfile(src, os.path.join(d, "mme.exe"))
shutil.copyfile(os.path.join(HERE, "settings.seed.json"),
                os.path.join(d, "mme-data", "settings.json"))
env = dict(os.environ)
log = os.path.join(d, "tick.log")
env["MME_TICKLOG"] = log
target = args[0]
if not os.path.isabs(target):
    target = os.path.join(HERE, "fix", target)
r = subprocess.run([HARNESS, os.path.join(d, "mme.exe"), target] + args[1:],
                   env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
sys.stdout.buffer.write(r.stdout)
if prof and os.path.isfile(log):
    sys.stdout.write("---- tick log\n")
    sys.stdout.write(open(log, encoding="utf8", errors="replace").read())
