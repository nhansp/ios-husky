#!/usr/bin/env bash
# Mount husky's iOS root (the HFS+ volume at userdata+40 GiB) READ-ONLY on
# rooted Android, so its files can be read with adb (logs, keybags, ...):
#
#   tools/ios-mount.sh        # load hfsplus.ko, map, mount at /data/local/tmp/ios
#   tools/ios-mount.sh -u     # unmount and remove the mapping
#
# The Pixel GKI kernel has no HFS+; hfsplus.ko is built out of tree against
# the running kernel (kernel/common 8d0115b07367, the device's
# /proc/config.gz + CONFIG_HFSPLUS_FS=m, Android CI 16385437 vmlinux.symvers
# for the CRCs, vermagic 6.1.172-android14-11-g8d0115b07367-ab16385437);
# see artifacts/gki-hfsplus. A loop device cannot be used: UFS has 4 KiB
# logical sectors and loop issues 512-byte direct I/O; a read-only dm-linear
# target over the same range works. Nothing is ever written to the phone.
set -euo pipefail
SERIAL=${SERIAL:-3A110DLJG0019U}
KO=${KO:-$(dirname "$0")/../artifacts/gki-hfsplus/src/fs/hfsplus/hfsplus.ko}
MNT=/data/local/tmp/ios

su() { adb -s "$SERIAL" shell "su -c '$1'"; }

if [[ "${1:-}" == -u ]]; then
	su "umount $MNT 2>/dev/null; dmctl delete iosroot 2>/dev/null; true"
	echo "unmounted"
	exit 0
fi
[[ -f "$KO" ]] || { echo "no $KO (build it, see header)" >&2; exit 1; }
adb -s "$SERIAL" push "$KO" /data/local/tmp/hfsplus.ko >/dev/null
# 512-byte sectors: the whole iOS window, from 40 GiB (ADT /husky-ufs
# root-offset) to 64 MiB (log-size) before the end of userdata. Since xnu 0075
# grows the HFS+ volume to the window, its alternate volume header sits at the
# window's end, and hfsplus rejects the volume if the mapping is shorter.
# (Computed here: the device shell's arithmetic is 32-bit.)
USERDATA=$(adb -s "$SERIAL" shell "su -c 'blockdev --getsize64 /dev/block/by-name/userdata'" | tr -d '\r')
WINDOW_SECTORS=$(( (USERDATA - (40 << 30) - (64 << 20)) / 512 ))
su "grep -q hfsplus /proc/filesystems || insmod /data/local/tmp/hfsplus.ko
    dmctl getpath iosroot >/dev/null 2>&1 || dmctl create iosroot -ro linear 0 $WINDOW_SECTORS /dev/block/by-name/userdata 83886080
    mkdir -p $MNT
    grep -q \" $MNT \" /proc/mounts || mount -t hfsplus -o ro \$(dmctl getpath iosroot) $MNT
    grep \" $MNT \" /proc/mounts"
echo "iOS root mounted read-only at $MNT (adb shell su -c 'ls $MNT')"
