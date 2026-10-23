#!/usr/bin/env bash
# Pull husky's iOS region back into a host image, transferring only what changed.
#
#   tools/pull-ios-root-delta.sh BASE.img OUT.img
#
# BASE.img must be the image last written with tools/flash-ios-root.sh (so the
# device matched it then). OUT.img starts as a copy of BASE.img; every 16 MiB
# chunk whose SHA-256 differs on the device (hashed on the phone, only hashes
# cross USB) is pulled over adb and verified. Read-only on the device.
set -euo pipefail
BASE=$1
OUT=$2
SERIAL=${SERIAL:-3A110DLJG0019U}
DEV=/dev/block/by-name/userdata
CHUNK=$((16 << 20))
ROOT_OFFSET=$((40 << 30))
SKIP0=$((ROOT_OFFSET / CHUNK))
SIZE=$(stat -c %s "$BASE")
N=$(((SIZE + CHUNK - 1) / CHUNK))
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "base $BASE: $SIZE B = $N chunks of 16 MiB at device chunk $SKIP0"

# Device-side hashes in one run (the device shell has 32-bit arithmetic: keep
# the numbers small by counting in 16 MiB blocks).
cat > "$TMP/h.sh" <<EOF
i=0
while [ \$i -lt $N ]; do
	h=\$(dd if=$DEV bs=$CHUNK skip=\$(($SKIP0 + i)) count=1 2>/dev/null | sha256sum | cut -d' ' -f1)
	echo "\$i \$h"
	i=\$((i + 1))
done
EOF
adb -s "$SERIAL" push "$TMP/h.sh" /data/local/tmp/husky-hash.sh > /dev/null
echo "hashing on the device..."
adb -s "$SERIAL" shell "su -c 'sh /data/local/tmp/husky-hash.sh'" | tr -d '\r' > "$TMP/dev.txt"
adb -s "$SERIAL" shell rm -f /data/local/tmp/husky-hash.sh > /dev/null
[ "$(wc -l < "$TMP/dev.txt")" -eq "$N" ] || { echo "device hash list incomplete"; exit 1; }

echo "hashing $BASE..."
python3 -I - "$BASE" "$CHUNK" > "$TMP/host.txt" <<'PY'
import hashlib, sys
path, chunk = sys.argv[1], int(sys.argv[2])
with open(path, 'rb') as f:
    i = 0
    while True:
        b = f.read(chunk)
        if not b:
            break
        print(i, hashlib.sha256(b).hexdigest())
        i += 1
PY

diff_list=$(join "$TMP/dev.txt" "$TMP/host.txt" | awk '$2 != $3 {print $1}')
nd=$(echo "$diff_list" | grep -c . || true)
echo "$nd of $N chunks differ"

cp --reflink=auto --sparse=always "$BASE" "$OUT"
for i in $diff_list; do
	want=$(awk -v i="$i" '$1 == i {print $2}' "$TMP/dev.txt")
	adb -s "$SERIAL" exec-out "su -c 'dd if=$DEV bs=$CHUNK skip=$((SKIP0 + i)) count=1 2>/dev/null'" > "$TMP/c.bin"
	got=$(sha256sum "$TMP/c.bin" | cut -d' ' -f1)
	[ "$got" = "$want" ] || { echo "chunk $i: hash mismatch after pull"; exit 1; }
	dd if="$TMP/c.bin" of="$OUT" bs=$CHUNK seek="$i" conv=notrunc 2>/dev/null
	echo "chunk $i pulled"
done
echo "ok: $OUT = device iOS region ($nd chunks pulled)"
