#!/usr/bin/env bash
# Pull xnu's persistent console log ("huskylog", bsd/dev/husky_ufs.c) from the
# reserved tail of husky's userdata partition, over adb from rooted Android,
# and print it in order. Read-only on the device.
#
#   tools/husky-log-pull.sh [OUT.txt]        (default: stdout)
#
# Layout: the last LOG_SIZE bytes of userdata (ADT /husky-ufs log-size,
# 64 MiB); block 0 = header "HUSKYLOG" | u32 version | u32 ring size |
# u64 total | u64 seq, ring in the blocks after it.
set -euo pipefail
SERIAL=${SERIAL:-3A110DLJG0019U}
LOG_SIZE=$((64 << 20))
RING_MAX=$((4 << 20))
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

size=$(adb -s "$SERIAL" shell "su -c 'blockdev --getsize64 /dev/block/by-name/userdata'" | tr -d '\r')
[[ "$size" =~ ^[0-9]+$ ]] || { echo "cannot read userdata size: $size" >&2; exit 1; }
skip=$(( (size - LOG_SIZE) / 4096 ))
count=$(( 1 + RING_MAX / 4096 ))
adb -s "$SERIAL" exec-out "su -c 'dd if=/dev/block/by-name/userdata bs=4096 skip=$skip count=$count 2>/dev/null'" > "$tmp"

python3 -I - "$tmp" "${1:-}" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', data, 0)
if magic != b'HUSKYLOG' or version != 1 or not 0 < size <= len(data) - 4096:
    sys.exit(f'no huskylog header (magic {magic!r}); was the kernel booted with ufsrw=1?')
ring = data[4096:4096 + size]
if total <= size:
    out = ring[:total]
else:
    at = total % size
    out = ring[at:] + ring[:at]
sys.stderr.write(f'huskylog: {total} bytes written, flush #{seq}, showing {len(out)}\n')
if sys.argv[2]:
    open(sys.argv[2], 'wb').write(out)
else:
    sys.stdout.buffer.write(out)
PY
