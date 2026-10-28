#!/usr/bin/env bash
# Pull the iOS unified log (logd's tracev3 files) from husky's iOS root and decode it on the host.
#
#   tools/ios-unified-log.sh OUT_DIR [SINCE] [PROCESS_REGEX]
#
# SINCE ("2026-10-28 08:00", device local time) limits the tracev3 files to the latest boot(s); the
# uuidtext string tables are pulled once into OUT_DIR/../uuidtext-cache and reused. Decoding uses
# mandiant/macos-UnifiedLogs' unifiedlog_iterator (UNIFIEDLOG_ITERATOR=path, built with cargo).
# Writes OUT_DIR/log.jsonl, and OUT_DIR/filtered.txt when PROCESS_REGEX is given.
set -euo pipefail
OUT=${1:?OUT_DIR}
SINCE=${2:-}
FILTER=${3:-}
SERIAL=${SERIAL:-3A110DLJG0019U}
ITER=${UNIFIEDLOG_ITERATOR:?set UNIFIEDLOG_ITERATOR to a built unifiedlog_iterator}
HERE=$(cd "$(dirname "$0")" && pwd)
ARCHIVE=$OUT/logarchive
CACHE=$(dirname "$OUT")/uuidtext-cache
mkdir -p "$ARCHIVE" "$CACHE"

bash "$HERE/ios-mount.sh" >/dev/null
trap 'bash "$HERE/ios-mount.sh" -u >/dev/null' EXIT
NEWER=""
[ -n "$SINCE" ] && NEWER="-newermt \"$SINCE\""
adb -s "$SERIAL" exec-out "su -c 'cd /data/local/tmp/ios/private/var/db/diagnostics && tar -cf - timesync \$(find Persist Special Signpost HighVolume -name \"*.tracev3\" $NEWER 2>/dev/null)'" |
	tar -xf - -C "$ARCHIVE"
if [ ! -d "$CACHE/dsc" ]; then
	adb -s "$SERIAL" exec-out "su -c 'cd /data/local/tmp/ios/private/var/db/uuidtext && tar -cf - .'" | tar -xf - -C "$CACHE"
fi
for d in "$CACHE"/*; do ln -sfn "$d" "$ARCHIVE/$(basename "$d")"; done

"$ITER" -m log-archive -i "$ARCHIVE" -o "$OUT/log.jsonl" -f jsonl >"$OUT/iterator.log" 2>&1
echo "$(wc -l <"$OUT/log.jsonl") entries in $OUT/log.jsonl"
if [ -n "$FILTER" ]; then
	python3 -I - "$OUT/log.jsonl" "$FILTER" >"$OUT/filtered.txt" <<'EOF'
import json, re, sys
pat = re.compile(sys.argv[2])
for line in open(sys.argv[1]):
    o = json.loads(line)
    p = o.get('process', '').rsplit('/', 1)[-1]
    if pat.search(p):
        print(o.get('timestamp', '')[11:23], f"{p}[{o.get('pid')}]", f"{o.get('subsystem', '')}:{o.get('category', '')}",
              o.get('log_type', ''), o.get('message', '')[:2000])
EOF
	echo "$(wc -l <"$OUT/filtered.txt") lines in $OUT/filtered.txt"
fi
