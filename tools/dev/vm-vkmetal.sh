#!/usr/bin/env bash
# vm-vkmetal.sh OUTDIR: build HuskyVKMetal.bundle (the Metal driver plug-in over metal2vulkan and panvk, userland/vkmetal) in the local macOS VM
# ("tahoe"), ad hoc signed with 4 KiB code-signing pages, and copy it with SHA256SUMS to OUTDIR. Incremental: the metal2vulkan checkout, the Rust
# target directory and the object files live in ~/husky/vkmetal on the VM. Needs the panvk port's host LLVM (~/husky/panvk/cache) and Mesa tree
# for tools only; set PANVK_DYLIB=<path on the host to libvulkan_panfrost.dylib> to ship panvk inside the bundle (Frameworks/).
# The VM has 7 GB: cargo runs with -j 4 (CARGO_JOBS), thin LTO by default (LTO=fat is the upstream setting; untested here, needs more memory; LTO=off when short).
set -euo pipefail
O=${1:?usage: vm-vkmetal.sh OUTDIR}
cd /home/nhansp/aosp/ios-husky
VM=${VM:-tahoe}
PIN=555b13d57e09bff70996a88cff64f1275256d5cf
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/husky-vkmetal/metal2vulkan
[ -d "$CACHE/.git" ] || { mkdir -p "$CACHE" && git init -q "$CACHE"; }
git -C "$CACHE" rev-parse -q --verify "$PIN^{commit}" > /dev/null ||
	git -C "$CACHE" fetch -q --depth 1 https://github.com/steelbrain/metal2vulkan.git "$PIN"
ssh -o BatchMode=yes "$VM" 'mkdir -p ~/husky/vkmetal/src'
# The pristine pin tree goes to the VM as an archive (a git fetch from the VM stalls); once.
if [ "$(ssh -o BatchMode=yes "$VM" 'cat ~/husky/vkmetal/m2v-pin/.pin 2>/dev/null')" != "$PIN" ]; then
	git -C "$CACHE" archive "$PIN" | ssh -o BatchMode=yes "$VM" "rm -rf ~/husky/vkmetal/m2v-pin && mkdir -p ~/husky/vkmetal/m2v-pin && tar -x -C ~/husky/vkmetal/m2v-pin && echo $PIN > ~/husky/vkmetal/m2v-pin/.pin"
fi
rsync -a --delete --exclude 'out' userland/vkmetal/ "$VM":husky/vkmetal/src/
EXTRA=""
if [ -n "${PANVK_DYLIB:-}" ]; then
	rsync -a "$PANVK_DYLIB" "$VM":husky/vkmetal/libvulkan_panfrost.dylib
	EXTRA="PANVK_DYLIB=\$HOME/husky/vkmetal/libvulkan_panfrost.dylib"
fi
ssh -o BatchMode=yes "$VM" "export HOMEBREW_NO_AUTO_UPDATE=1; eval \"\$(/usr/local/bin/brew shellenv)\"; CARGO_JOBS=${CARGO_JOBS:-4} LTO=${LTO:-thin} $EXTRA \
	sh \$HOME/husky/vkmetal/src/build.sh"
rm -rf "$O" && mkdir -p "$O"
rsync -a "$VM":husky/vkmetal/out/ "$O"/
(cd "$O" && sha256sum -c SHA256SUMS | tail -2)
du -sk "$O/HuskyVKMetal.bundle" | awk '{print "bundle size: " $1 " KiB"}'
