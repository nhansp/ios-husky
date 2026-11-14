#!/usr/bin/env bash
# vm-panvk.sh OUTDIR: build panvk (libvulkan_panfrost.dylib, arm64e iOS 26) and tools/vk-smoke in the local macOS VM
# ("tahoe") and copy libvulkan_panfrost.dylib, vk_smoke and its launchd plist to OUTDIR. Rerunnable: the host toolchain
# (LLVM, SPIRV-Tools), the patched Mesa tree and the object files live in ~/husky/panvk on the VM.
set -euo pipefail
O=${1:?usage: vm-panvk.sh OUTDIR}
cd /home/nhansp/aosp/ios-husky
VM=${VM:-tahoe}
MESA_REV=$(sed -n 's/^  MESA_REV: //p' .github/workflows/gpu-userland.yml)
ssh -o BatchMode=yes "$VM" 'mkdir -p ~/husky/panvk/src'
rsync -a --delete userland/panvk-darwin "$VM":husky/panvk/src/
rsync -a --delete tools/vk-smoke "$VM":husky/panvk/src/
ssh -o BatchMode=yes "$VM" "MESA_REV=$MESA_REV PANVK_JOBS=${PANVK_JOBS:-6} sh \$HOME/husky/panvk/src/panvk-darwin/build/vm-build.sh"
rm -rf "$O" && mkdir -p "$O"
rsync -a "$VM":husky/panvk/out/ "$O"/
(cd "$O" && sha256sum -c SHA256SUMS | tail -3)
