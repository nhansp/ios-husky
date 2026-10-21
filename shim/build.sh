#!/bin/bash
# Build the shim and wrap it in a kernel-only boot.img v4, the same shape
# as the stock husky boot.img (ramdisk lives in init_boot, DTB in
# vendor_kernel_boot, both still taken from the device on `fastboot boot`).
set -euo pipefail
cd "$(dirname "$0")"
MKBOOTIMG=${MKBOOTIMG:-../artifacts/mkbootimg/mkbootimg.py}
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
# HANG=entry spins at the first instruction; HANG=probe stamps "RUN!" into
# the ramoops console zone first, so a handoff shows up after a reset.
CPPFLAGS=""
[ "${HANG:-}" = entry ] && CPPFLAGS="-DHANG_AT_ENTRY"
[ "${HANG:-}" = probe ] && CPPFLAGS="-DPROBE_RAMOOPS"
# NOSIGN=1: loader leaves arm64e auth fixups unsigned (pairs with the kernel's
# HUSKY PAC-off bring-up build).
[ "${NOSIGN:-}" = 1 ] && CPPFLAGS="$CPPFLAGS -DHUSKY_NO_PAC_SIGN"
# UFSPROBE=1: read-only probe of the UFS host ABL left running (shim/ufs.c),
# logged to pmsg before the xnu handoff.
[ "${UFSPROBE:-}" = 1 ] && CPPFLAGS="$CPPFLAGS -DUFSPROBE"
# -march=armv8.3-a: loader.c signs arm64e auth pointers with pacia/pacib/pacda/pacdb.
CFLAGS="$CPPFLAGS -O2 -ffreestanding -nostdlib -fno-pic -march=armv8.3-a -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
SRCS="start.S shim.c console.c fdt.c pac.S loader.c ufs.c"
mkdir -p out
objs=()
for f in $SRCS; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
	objs+=(out/${f%.*}.o)
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r "${objs[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"${objs[@]}" -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((0x$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
img=out/shim-boot${HANG:+-hang-$HANG}.img
python3 "$MKBOOTIMG" --header_version 4 --kernel out/Image -o $img
# No AVB footer or partition-size padding: vbmeta is flashed with
# --disable-verification, so ABL accepts an unsigned kernel-only boot.img.
ls -l out/Image out/shim-boot*.img
