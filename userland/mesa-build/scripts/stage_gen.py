#!/usr/bin/env python3
"""stdin: file paths (absolute or build-relative). Copies those that live under BUILD into DST.
usage: stage_gen.py BUILD DST   (objects, archives and depfiles are skipped)"""
import os
import shutil
import sys

bld, dst = os.path.realpath(sys.argv[1]), sys.argv[2]
seen = set()
for line in sys.stdin:
    p = line.strip()
    if not p or p.endswith((".o", ".a", ".d")):
        continue
    full = os.path.realpath(p if os.path.isabs(p) else os.path.join(bld, p))
    if not full.startswith(bld + os.sep) or full in seen or not os.path.isfile(full):
        continue
    seen.add(full)
    out = os.path.join(dst, os.path.relpath(full, bld))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copy2(full, out)
print(f"staged {len(seen)} generated files into {dst}")
