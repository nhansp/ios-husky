#!/usr/bin/env bash
# Make an incremental xnu build see only real source changes.
#
# DXB re-applies its sed edits and our patches every run (and the workflow
# resets the cached tree first), so every patched file gets a fresh mtime and
# make would rebuild most of the kernel. Run after patching: age every file
# whose content matches the last successful build's manifest so only files
# that genuinely changed are newer than the cached objects.
#
# usage: xnu-mtime-sync.sh <xnu-src-dir> <manifest-path>
set -euo pipefail
SRC="$1"
MAN="$2"
T="$(mktemp -d)"
cd "$SRC"

git ls-files -co --exclude-standard | grep -v '^compile_commands.json$' > "$T/paths"
git hash-object --stdin-paths < "$T/paths" | paste -d' ' - "$T/paths" | LC_ALL=C sort > "$T/cur"

if [ -f "$MAN" ]; then
    tr '\n' '\0' < "$T/paths" | xargs -0 touch -h -t 200001010000
    LC_ALL=C sort "$MAN" > "$T/old"
    comm -13 "$T/old" "$T/cur" | cut -d' ' -f2- > "$T/changed"
    echo "mtime-sync: $(wc -l < "$T/changed" | tr -d ' ') changed of $(wc -l < "$T/paths" | tr -d ' ') files"
    head -50 "$T/changed"
    if [ -s "$T/changed" ]; then
        tr '\n' '\0' < "$T/changed" | xargs -0 touch -h
    fi
else
    echo "mtime-sync: no manifest (cold cache), full build"
fi

mkdir -p "$(dirname "$MAN")"
cp "$T/cur" "$MAN"
rm -rf "$T"
