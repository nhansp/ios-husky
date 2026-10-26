#!/usr/bin/env bash
# Write an HFS+ root image into husky's iOS region: the tail of the userdata
# partition, from byte offset ROOT_OFFSET (40 GiB, past Android's shrunken
# f2fs, see tools/f2fs_shrink.c) up to LOG_SIZE before its end. Streams over
# adb into rooted Android; never use `fastboot flash userdata` for this (it
# writes from offset 0 and destroys Android's data).
#
#   tools/flash-ios-root.sh ROOT.img
#
# Refuses unless: the image is HFS+ ('H+'/'HX' at byte 1024), it fits the
# window, and Android's f2fs ends below ROOT_OFFSET. The image size need not
# match the window; HFS+ only uses what its header declares.
set -euo pipefail
IMG=${1:?usage: flash-ios-root.sh ROOT.img}
SERIAL=${SERIAL:-3A110DLJG0019U}
ROOT_OFFSET=$((40 << 30))	# ADT /husky-ufs root-offset
LOG_SIZE=$((64 << 20))		# ADT /husky-ufs log-size
DEV=/dev/block/by-name/userdata

sig=$(dd if="$IMG" bs=1 skip=1024 count=2 2>/dev/null | tr -d '\0')
[[ "$sig" == "H+" || "$sig" == "HX" ]] || { echo "$IMG: not an HFS+ image (signature '$sig')" >&2; exit 1; }
img_size=$(stat -L -c %s "$IMG")
(( img_size % 4096 == 0 )) || { echo "$IMG: size not a multiple of 4096" >&2; exit 1; }

su() { adb -s "$SERIAL" shell "su -c '$1'" | tr -d '\r'; }
part_size=$(su "blockdev --getsize64 $DEV")
[[ "$part_size" =~ ^[0-9]+$ ]] || { echo "cannot read $DEV size: $part_size" >&2; exit 1; }
window=$(( part_size - ROOT_OFFSET - LOG_SIZE ))
(( img_size <= window )) || { echo "$IMG ($img_size B) exceeds the iOS window ($window B)" >&2; exit 1; }

# Android's f2fs: superblock at byte 1024 of the decrypted /data device;
# block_count (u64 at +36) in 4 KiB blocks. Compare in blocks on the host
# (the device shell's arithmetic is 32-bit).
data_dev=$(su "grep \" /data \" /proc/mounts | cut -d\" \" -f1")
f2fs_blocks=$(adb -s "$SERIAL" exec-out "su -c 'dd if=$data_dev bs=1024 skip=1 count=1 2>/dev/null'" |
	python3 -I -c 'import struct,sys; b=sys.stdin.buffer.read(); assert b[:4]==bytes.fromhex("1020f5f2"); print(struct.unpack_from("<Q",b,36)[0])')
(( f2fs_blocks * 4096 <= ROOT_OFFSET )) || { echo "f2fs ($f2fs_blocks blocks) reaches into the iOS region: refusing" >&2; exit 1; }

echo "userdata $part_size B; f2fs ends at $((f2fs_blocks * 4096)); writing $img_size B at $ROOT_OFFSET"
# Chunked, resumable: a single 20 GiB `adb exec-in` stream was seen to stop
# silently after ~1.6 GiB. Each 256 MiB chunk is compared by sha256 with the
# device, written only if it differs, then verified (3 tries).
CHUNK=$((256 << 20))
nchunks=$(( (img_size + CHUNK - 1) / CHUNK ))
for (( c = 0; c < nchunks; c++ )); do
	off=$(( c * CHUNK ))
	len=$(( img_size - off < CHUNK ? img_size - off : CHUNK ))
	# bs=4096 counts work for a short last chunk too.
	skip4k=$(( (ROOT_OFFSET + off) / 4096 )); n4k=$(( len / 4096 ))
	want=$(dd if="$IMG" bs=4096 skip=$(( off / 4096 )) count=$n4k 2>/dev/null | sha256sum | cut -d' ' -f1)
	for try in 1 2 3 4; do
		got=$(su "dd if=$DEV bs=4096 skip=$skip4k count=$n4k 2>/dev/null | sha256sum" | cut -d' ' -f1)
		[[ "$got" == "$want" ]] && break
		(( try < 4 )) || { echo "chunk $c: still differs after 3 writes" >&2; exit 1; }
		dd if="$IMG" bs=4096 skip=$(( off / 4096 )) count=$n4k 2>/dev/null |
			adb -s "$SERIAL" exec-in "su -c 'dd of=$DEV bs=4096 seek=$skip4k conv=fsync 2>/dev/null'"
		printf 'chunk %d/%d written (try %d)\n' $((c + 1)) $nchunks $try
	done
done
echo "ok: all $nchunks chunks match"
