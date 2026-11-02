#!/usr/bin/env bash
# vm-xnu.sh OUTDIR: build kernel.development.husky in the local macOS VM ("tahoe", reims-ctl) the way
# .github/workflows/xnu.yml does on the runner (DXB at the same SHA, xnu/patches applied to pristine xnu,
# unchanged sources aged by xnu-mtime-sync.sh so make only rebuilds what the patches change), then copy the
# kernel to OUTDIR. The DXB tree stays in the VM (~/husky/dxb), so every build after the first is incremental.
set -euo pipefail
O=${1:?usage: vm-xnu.sh OUTDIR}
cd /home/nhansp/aosp/ios-husky
VM=${VM:-tahoe}
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
ssh -o BatchMode=yes "$VM" 'mkdir -p ~/husky/patches ~/husky/scripts'
rsync -a --delete xnu/patches/ "$VM":husky/patches/
rsync -a .github/scripts/xnu-mtime-sync.sh "$VM":husky/scripts/
ssh -o BatchMode=yes "$VM" DXB_SHA=$DXB_SHA bash -s <<'VMEOF'
set -euo pipefail
eval "$(/usr/local/bin/brew shellenv)"
export HOMEBREW_NO_AUTO_UPDATE=1
cd ~/husky
[ -x /usr/local/bin/xcodes ] || { printf '#!/bin/sh\nexit 0\n' | sudo tee /usr/local/bin/xcodes >/dev/null; sudo chmod +x /usr/local/bin/xcodes; }
if [ ! -d dxb/.git ]; then
	git init -q dxb
	git -C dxb fetch -q --depth 1 https://github.com/blacktop/darwin-xnu-build "$DXB_SHA"
	git -C dxb checkout -q FETCH_HEAD
	INJ='for v in RELEASE DEVELOPMENT DEBUG; do KS="${KDKROOT}/System/Library/KernelSupport"; [ -f "$KS/libVMAPPLE.os.$v.a" ] \&\& sudo ln -sf "libVMAPPLE.os.$v.a" "$KS/libHUSKY.os.$v.a"; done'
	for fn in build_xnu_library build_xnu; do
		sed -i '' "s|^$fn() {|$fn() { $INJ|" dxb/build.sh
	done
	sed -i '' 's|^    patches$|    patches; bash "'"$HOME"'/husky/scripts/xnu-mtime-sync.sh" "${WORK_DIR}/xnu" "${BUILD_DIR}/xnu-src.manifest"|' dxb/build.sh
fi
PD=dxb/patches/26.5; [ -d "$PD" ] || PD=dxb/patches/26.0
rm -f "$PD"/[0-9][0-9][0-9][0-9]-*.patch "$PD"/[0-9][0-9][0-9][0-9]-*.sh
cp patches/*.patch "$PD"/
compgen -G 'patches/*.sh' >/dev/null && cp patches/*.sh "$PD"/
if [ -d dxb/xnu/.git ]; then
	git -C dxb/xnu checkout -q -- .
	git -C dxb/xnu clean -fdqx -e compile_commands.json
	rm -rf dxb/build/xnu.obj/kernel.* dxb/fakeroot/System/Library/Kernels/kernel.*
fi
cd dxb
MACOS_VERSION=26.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=HUSKY MAKEFLAGS=-k \
	/usr/local/bin/bash build.sh > ~/husky/build.log 2>&1 || {
	grep -nE 'error:|fatal error:|No rule to make target|Undefined symbols|ld: ' ~/husky/build.log | awk '!s[$0]++' | head -40
	exit 1
}
ls -la fakeroot/System/Library/Kernels/
VMEOF
mkdir -p "$O"
scp -q "$VM":husky/dxb/fakeroot/System/Library/Kernels/kernel.development.husky "$O"/
(cd "$O" && sha256sum kernel.development.husky)
