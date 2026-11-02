#!/usr/bin/env bash
# vm-userland.sh OUTDIR: build the gpu-userland artifact (MaliMetal.bundle with libMaliCompiler.dylib, metal_smoke,
# fsprobe) in the local macOS VM ("tahoe") with the steps of .github/workflows/gpu-userland.yml, then copy it to
# OUTDIR. Mesa source, the Mesa iOS libraries and LLVM for iOS are kept in ~/husky/ucache and rebuilt only when
# their inputs change (the workflow's cache keys).
set -euo pipefail
O=${1:?usage: vm-userland.sh OUTDIR}
cd /home/nhansp/aosp/ios-husky
VM=${VM:-tahoe}
MESA_REV=$(sed -n 's/^  MESA_REV: //p' .github/workflows/gpu-userland.yml)
KEY_MESA=$(find userland/mesa-build -type f | sort | xargs cat | sha256sum | cut -c1-16)
KEY_LLVM=$(sha256sum userland/air2valhall/llvm-ios-build.sh | cut -c1-16)
ssh -o BatchMode=yes "$VM" 'mkdir -p ~/husky/usrc ~/husky/ucache'
rsync -a --delete --exclude 'mali-shadercache' --exclude '*.o' userland tools/metal-smoke tools/fsprobe "$VM":husky/usrc/
ssh -o BatchMode=yes "$VM" "MESA_REV=$MESA_REV KEY_MESA=$KEY_MESA KEY_LLVM=$KEY_LLVM" bash -s <<'VMEOF'
set -euo pipefail
eval "$(/usr/local/bin/brew shellenv)"
export HOMEBREW_NO_AUTO_UPDATE=1
C=~/husky/ucache
cd ~/husky/usrc
mkdir -p tools && rm -rf tools/metal-smoke tools/fsprobe && mv metal-smoke fsprobe tools/
if [ ! -f "$C/mesa-$MESA_REV/meson.build" ]; then
	rm -rf "$C/mesa-$MESA_REV" && git init -q "$C/mesa-$MESA_REV"
	git -C "$C/mesa-$MESA_REV" fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_REV" ||
		git -C "$C/mesa-$MESA_REV" fetch -q --depth 1 https://github.com/mesa3d/mesa.git "$MESA_REV"
	git -C "$C/mesa-$MESA_REV" checkout -q FETCH_HEAD
fi
MESA=$C/mesa-$MESA_REV
MO=$C/mesa-ios-$KEY_MESA
if [ ! -f "$MO/lib/libmali_mesa.a" ]; then
	rm -rf "$MO" && mkdir -p "$MO/gen"
	tar -xJf userland/mesa-build/gen-1f9035d.tar.xz -C "$MO/gen" --strip-components=1
	MESA_SRC=$MESA MESA_GEN=$MO/gen MESA_OUT_LIB=$MO/lib MESA_BUILD_TREE=$MO/tree ARCH=arm64e \
		bash userland/mesa-build/ios-build.sh > ~/husky/mesa-build.log 2>&1
fi
LL=$C/llvm-ios-$KEY_LLVM
if [ ! -d "$LL/lib" ]; then
	OUT=$LL WORK=$C/llvm-work sh userland/air2valhall/llvm-ios-build.sh > ~/husky/llvm-build.log 2>&1
fi
rm -rf out && mkdir -p out/plugin out/smoke out/inc
MESA_SRC=$MESA MESA_TREE=$MO/tree MESA_LIB=$MO/lib/libmali_mesa.a LLVM_DIR=$LL OUT=$PWD/out/compiler \
	sh userland/air2valhall/ios-build.sh > ~/husky/compiler-build.log 2>&1
codesign -f -s - out/compiler/libMaliCompiler.dylib
xcrun -sdk iphoneos metal -c -mios-version-min=26.0 userland/plugin/MaliMetal/shaders/MaliInternal.metal -o out/plugin/MaliInternal.air
xcrun -sdk iphoneos metallib out/plugin/MaliInternal.air -o out/plugin/MaliInternal.metallib
PACK=""
[ -f userland/shadercache/shaders.mvpack ] && PACK=$PWD/userland/shadercache/shaders.mvpack
INTERNAL_AIR=$PWD/out/plugin/MaliInternal.air MESA_SRC=$MESA MESA_TREE=$MO/tree MESA_LIB=$MO/lib/libmali_mesa.a \
	OUT=$PWD/out/plugin PACK="$PACK" sh userland/plugin/MaliMetal/ios-build.sh > ~/husky/plugin-build.log 2>&1 ||
	{ grep -E ' error: |Undefined symbols|ld: ' ~/husky/plugin-build.log | head -40; exit 1; }
mkdir -p out/plugin/MaliMetal.bundle/Frameworks
cp out/compiler/libMaliCompiler.dylib out/plugin/MaliMetal.bundle/Frameworks/
codesign -f -s - out/plugin/MaliMetal.bundle
xcrun -sdk iphoneos metal -c -mios-version-min=26.0 tools/metal-smoke/metal_smoke.metal -o out/smoke/metal_smoke.air
xcrun -sdk iphoneos metallib out/smoke/metal_smoke.air -o out/smoke/metal_smoke.metallib
ln -sfn "$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/IOKit.framework/Headers" out/inc/IOKit
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -fobjc-arc -O1 -Wall -Werror -I out/inc \
	tools/metal-smoke/metal_smoke.m -framework Foundation -framework Metal -framework IOKit -framework IOSurface -o out/smoke/metal_smoke
codesign -f -s - --entitlements tools/metal-smoke/ent.plist out/smoke/metal_smoke
cp tools/metal-smoke/com.husky.metal-smoke.plist out/smoke/
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -fobjc-arc -O1 -Wall -Werror \
	tools/fsprobe/fsprobe.m -framework Foundation -o out/smoke/fsprobe
codesign -f -s - out/smoke/fsprobe
cp tools/fsprobe/com.husky.fsprobe.plist out/smoke/
rm -rf artifact && mkdir artifact
cp -R out/plugin/MaliMetal.bundle out/plugin/MaliInternal.metallib artifact/
cp out/smoke/metal_smoke out/smoke/fsprobe out/smoke/metal_smoke.metallib out/smoke/*.plist artifact/
(cd artifact && find . -type f ! -name SHA256SUMS -exec shasum -a 256 {} + > ../SHA256SUMS && mv ../SHA256SUMS .)
VMEOF
rm -rf "$O" && mkdir -p "$O"
rsync -a "$VM":husky/usrc/artifact/ "$O"/
(cd "$O" && sha256sum -c SHA256SUMS | tail -1)
