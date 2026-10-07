#!/usr/bin/env python3
"""Checks how read-aloud classifies known paragraphs (tests/reading/cases.tsv).

Usage: scripts/check-reading.py [--rules-only] [path/to/glance]

By default the page-layout model is used when it is installed; --rules-only checks
the heuristics alone, which is what runs on pages the model has not looked at.
Exits non-zero when any case fails, and prints each failure with what was found.
"""
import os
import re
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
rules_only = "--rules-only" in sys.argv
binary = args[0] if args else os.path.join(root, "build", "glance")
pdfs = os.environ.get("GLANCE_TEST_PDFS", os.path.expanduser("~/Downloads"))

cases = []
with open(os.path.join(root, "tests", "reading", "cases.tsv")) as f:
    for line in f:
        line = line.rstrip("\n")
        if line and not line.startswith("#"):
            cases.append(line.split("\t"))

line_re = re.compile(r"^p(\d+) (\S+)\s+(?:\[skip\] )?\s*(.*)$")
cache = {}


def paragraphs(name):
    if name not in cache:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        if not rules_only:
            env["GLANCE_READ_LAYOUT"] = "1"
        out = subprocess.run([binary, "--read-selftest", os.path.join(pdfs, name)],
                             capture_output=True, text=True, env=env).stdout
        items = []
        for raw in out.splitlines():
            m = line_re.match(raw)
            if m:
                items.append((m.group(2), m.group(3).strip()))
        cache[name] = items
    return cache[name]


passed = failed = skipped = 0
for name, kind, text in cases:
    if not os.path.exists(os.path.join(pdfs, name)):
        skipped += 1
        continue
    exact = text.startswith("=")
    needle = text[1:] if exact else text
    found = [k for k, t in paragraphs(name) if (t == needle if exact else t.startswith(needle))]
    if any(k in found for k in kind.split("|")):
        passed += 1
    else:
        failed += 1
        print(f"FAIL  {name}: {needle!r} expected {kind}, found {found or 'nothing'}")
print(f"{passed} passed, {failed} failed, {skipped} skipped")
sys.exit(1 if failed else 0)
