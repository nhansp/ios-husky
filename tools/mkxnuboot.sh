#!/bin/bash
# Build a husky boot image that boots xnu: the shim as the kernel, the
# HUSKYXNU payload (xnu Mach-O + ADT, with the auth fixups resolved/tabled)
# as the ramdisk. ABL loads the ramdisk and records linux,initrd-start in the
# FDT /chosen; the shim's loader finds the payload there.
#
#   tools/mkxnuboot.sh KERNEL [OUT.img] [-- mkpayload/mkadt args]
#
# Example (first boot, defaults):
#   tools/mkxnuboot.sh out/xnu-husky-P1B/kernel.development.husky out/boot/xnu-boot.img
# After one diagnostic run, pin the per-unit values the shim logged:
#   tools/mkxnuboot.sh KERNEL OUT.img --boot-mpidr 0x0 --timebase 24576000
#
# Delivery (vbmeta already flashed with --disable-verification in P0):
#   fastboot boot OUT.img
#   adb shell cat /sys/fs/pstore/pmsg-ramoops-0   # after it resets back to Android
set -euo pipefail
cd "$(dirname "$0")/.."
KERNEL=${1:?usage: mkxnuboot.sh KERNEL [OUT.img] [extra mkadt/mkpayload args]}
OUT=${2:-out/boot/xnu-boot.img}
shift || true; shift || true
EXTRA=("$@")					# forwarded to mkadt.py (e.g. --boot-mpidr/--timebase/--lowmem)
MKBOOTIMG=${MKBOOTIMG:-artifacts/mkbootimg/mkbootimg.py}
CMDLINE=${CMDLINE:-"debug=0xc serial=3 cpus=1 keepsyms=1"}

mkdir -p out/boot
echo ">> building shim"
( cd shim && bash build.sh >/dev/null )
echo ">> building ADT"
python3 -I tools/mkadt.py out/boot/husky.adt "${EXTRA[@]}"
echo ">> building payload"
python3 -I tools/mkpayload.py "$KERNEL" out/boot/husky.adt out/boot/payload.bin \
	--cmdline "$CMDLINE" ${RAMDISK:+--ramdisk "$RAMDISK"}
echo ">> packing boot image"
python3 "$MKBOOTIMG" --header_version 4 \
	--kernel shim/out/Image --ramdisk out/boot/payload.bin -o "$OUT"
ls -l "$OUT" out/boot/payload.bin
echo ">> deliver with:  fastboot boot $OUT"
