#!/bin/sh
# build.sh: HuskyVKMetal.bundle for iOS 26 arm64e. Runs in the macOS VM (see tools/dev/vm-vkmetal.sh); incremental.
#   W            work directory (default ~/husky/vkmetal)
#   SRC          this directory (default: where this script is)
#   OUT          where the bundle is left (default $W/out)
#   LLVM_BIN     llvm-objcopy's directory (the host LLVM of the panvk port)
#   MESA_INC     a Mesa checkout's include directory (the Vulkan headers RustPacBoundary.c needs)
#   PANVK_DYLIB  optional: a libvulkan_panfrost.dylib to ship inside the bundle (Frameworks/)
#   CARGO_JOBS   cargo -j (default 4: the VM has 7 GB)
#   LTO          thin (default, built and verified) | fat (untested in the 7 GB VM) | off
#
# Pieces (see README.md for the licensing arrangement):
#   metal2vulkan (pinned, patched by bridge/patches/) + bridge/NativeMetalBridge.rs  -> libhusky_vkmetal_bridge.a   (LGPL-3.0-or-later side)
#   libhusky_vkmetal_bridge.a is NOT linked into the bundle executable: it is linked into its own dylib, Frameworks/libhuskyvk_m2v.dylib, which the
#   plug-in executable loads by install name, so a user can rebuild and replace the LGPL part alone.
set -eu
SRC=${SRC:-$(cd "$(dirname "$0")" && pwd)}
W=${W:-$HOME/husky/vkmetal}
OUT=${OUT:-$W/out}
LLVM_BIN=${LLVM_BIN:-$HOME/husky/panvk/cache/llvm-native/bin}
MESA_INC=${MESA_INC:-$HOME/husky/panvk/mesa/include}
CARGO_JOBS=${CARGO_JOBS:-4}
LTO=${LTO:-thin}
PIN=555b13d57e09bff70996a88cff64f1275256d5cf
export PATH="$HOME/.cargo/bin:$PATH"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
TARGET="arm64e-apple-ios26.0"
mkdir -p "$W" "$OUT"

# ---- 1. metal2vulkan at its pin, with the recorded patches ----
# $W/m2v-pin is the pristine tree of the pin (tools/dev/vm-vkmetal.sh sends it as `git archive`: a shallow git fetch from the VM stalls); the
# patched copy is $W/metal2vulkan, rebuilt from it whenever the patches change.
PINDIR=$W/m2v-pin
M2V=$W/metal2vulkan
[ "$(cat "$PINDIR/.pin" 2>/dev/null)" = "$PIN" ] || { echo "$PINDIR is not the tree of $PIN (run tools/dev/vm-vkmetal.sh)" >&2; exit 1; }
STAMP=$(cat "$SRC"/bridge/patches/*.patch | shasum -a 256 | cut -c1-16)
if [ "$(cat "$M2V/.husky-patches" 2>/dev/null)" != "$STAMP" ]; then
	rm -rf "$M2V" && cp -R "$PINDIR" "$M2V"
	for p in "$SRC"/bridge/patches/*.patch; do
		(cd "$M2V" && git apply --whitespace=nowarn "$p") || { echo "patch $(basename "$p") does not apply" >&2; exit 1; }
	done
	echo "$STAMP" > "$M2V/.husky-patches"
fi

# ---- 2. the Rust bridge (arm64e iOS: no prebuilt std, so build-std) ----
B=$W/bridge-build
mkdir -p "$B"
case $LTO in off) LTOV=false; CGU=16 ;; thin) LTOV=thin; CGU=16 ;; *) LTOV=fat; CGU=1 ;; esac
sed -e "s|@M2V@|$M2V|g" -e "s|@BRIDGE@|$SRC/bridge|g" -e "s|@LTO@|$LTOV|" -e "s|@CGU@|$CGU|" "$SRC/bridge/Cargo.toml.in" > "$B/Cargo.toml"
[ -f "$B/Cargo.lock" ] || { [ -f "$SRC/bridge/Cargo.lock" ] && cp "$SRC/bridge/Cargo.lock" "$B/Cargo.lock"; } || true
RUNTIME_ID=$(printf '%s %s' "$PIN" "$STAMP" | shasum -a 256 | cut -c1-32)
BSTAMP=$(cat "$SRC"/bridge/*.rs "$SRC"/bridge/Cargo.toml.in | shasum -a 256 | cut -c1-16)-$STAMP-$LTOV
if [ "$(cat "$B/.built" 2>/dev/null)" != "$BSTAMP" ] || [ ! -f "$B/target/arm64e-apple-ios/release/libhusky_vkmetal_bridge.a" ]; then
	RUSTC_BOOTSTRAP=1 IPHONEOS_DEPLOYMENT_TARGET=26.0 NMB_RUNTIME_ID=$RUNTIME_ID \
		cargo +1.98.1 build --release -j "$CARGO_JOBS" -Z build-std=std,panic_unwind --target arm64e-apple-ios \
		--manifest-path "$B/Cargo.toml" --target-dir "$B/target" > "$B/cargo.log" 2>&1 ||
		{ tail -40 "$B/cargo.log"; exit 1; }
	echo "$BSTAMP" > "$B/.built"
fi
cp "$B/Cargo.lock" "$OUT/Cargo.lock"
LIB=$B/target/arm64e-apple-ios/release/libhusky_vkmetal_bridge.a
[ -f "$LIB" ] || { echo "no $LIB (the crate's library name)" >&2; ls "$B/target/arm64e-apple-ios/release" >&2; exit 1; }

# ---- 3. the PAC boundary and the bridge dylib ----
# rustc compiles arm64e without pointer authentication: imports that take a callback are renamed in the Rust objects to the signing wrappers of
# RustPacBoundary.c, and the Rust code's indirect branches are routed through stripping trampolines after the link (scripts/rust-pac-rewrite.py).
C="xcrun -sdk iphoneos clang -target $TARGET -isysroot $SDK -O2 -g -Wall -Wextra -Werror -fstack-protector-strong"
PAC=$B/libhusky_vkmetal_bridge_pac.a
"$LLVM_BIN/llvm-objcopy" --redefine-sym=_pthread_create=_nmb_pac_pthread_create --redefine-sym=__tlv_atexit=_nmb_pac_tlv_atexit \
	--redefine-sym=_atexit=_nmb_pac_atexit --redefine-sym=__dyld_register_func_for_add_image=_nmb_pac_dyld_register_func_for_add_image \
	--redefine-sym=__Unwind_Backtrace=_nmb_pac_Unwind_Backtrace --redefine-sym=_dlsym=_nmb_pac_dlsym "$LIB" "$PAC"
$C -std=c11 -I "$MESA_INC" -c "$SRC/bridge/RustPacBoundary.c" -o "$B/RustPacBoundary.o"
BR=$B/libhuskyvk_m2v.dylib
printf "_nmb_*\n" > "$B/exports.txt"
# The bridge dylib exports the nmb_* C ABI and nothing else it needs the plug-in to see.
$C -dynamiclib -install_name @loader_path/Frameworks/libhuskyvk_m2v.dylib -Wl,-map,"$B/m2v.map" -Wl,-dead_strip \
	-Wl,-exported_symbols_list,"$B/exports.txt" \
	"$B/RustPacBoundary.o" -Wl,-force_load,"$PAC" -framework Foundation -framework CoreFoundation -framework Security -lc++ -lz -lresolv \
	-o "$BR"
python3 "$SRC/scripts/rust-pac-rewrite.py" "$BR" "$B/m2v.map" libhusky_vkmetal_bridge_pac.a

# ---- 4. the plug-in executable ----
O=$B/obj
mkdir -p "$O"
CXX="xcrun -sdk iphoneos clang++ -target $TARGET -isysroot $SDK -O2 -g -Wall -std=c++17 -fobjc-arc -Wno-deprecated-declarations -Wno-unused-function -x objective-c++"
$CXX -I "$SRC" -c "$SRC/HuskyVKMetalFrontend.mm" -o "$O/HuskyVKMetalFrontend.o"
$CXX -I "$SRC" -c "$SRC/HuskyVKMetalPlugin.mm" -o "$O/HuskyVKMetalPlugin.o"
BUNDLE=$OUT/HuskyVKMetal.bundle
rm -rf "$BUNDLE" && mkdir -p "$BUNDLE/Frameworks"
cp "$BR" "$BUNDLE/Frameworks/libhuskyvk_m2v.dylib"
# the plug-in names the bridge by @loader_path-relative install name; the executable sits next to Frameworks/
xcrun -sdk iphoneos clang++ -target $TARGET -isysroot $SDK -bundle -Wl,-dead_strip -o "$BUNDLE/HuskyVKMetal" \
	"$O/HuskyVKMetalFrontend.o" "$O/HuskyVKMetalPlugin.o" "$BUNDLE/Frameworks/libhuskyvk_m2v.dylib" \
	"$SRC/stubs/Metal.tbd" -lobjc -framework Foundation -framework CoreFoundation -framework IOSurface -framework IOKit -lc++ -lz
install_name_tool -change @loader_path/Frameworks/libhuskyvk_m2v.dylib @loader_path/Frameworks/libhuskyvk_m2v.dylib "$BUNDLE/HuskyVKMetal" || true
cp "$SRC/Info.plist" "$BUNDLE/Info.plist"
mkdir -p "$BUNDLE/licenses" && cp "$SRC"/licenses/* "$BUNDLE/licenses/"
if [ -n "${PANVK_DYLIB:-}" ] && [ -f "$PANVK_DYLIB" ]; then cp "$PANVK_DYLIB" "$BUNDLE/Frameworks/libvulkan_panfrost.dylib"; fi
strip -x "$BUNDLE/HuskyVKMetal" "$BUNDLE/Frameworks/libhuskyvk_m2v.dylib" 2>/dev/null || true

# ---- 5. signing: ad hoc, 4 KiB code-signing pages (the Rust and Mesa objects' pages are verified at the kernel's 4 KiB granule) ----
for f in "$BUNDLE"/Frameworks/*.dylib; do codesign -f -s - --pagesize 4096 "$f"; done
codesign -f -s - --pagesize 4096 --identifier org.husky.HuskyVKMetal "$BUNDLE"
codesign --verify --strict "$BUNDLE"
codesign --display --verbose=4 "$BUNDLE" 2>&1 | grep -E 'CDHash|Page size' || true
(cd "$OUT" && find HuskyVKMetal.bundle -type f -exec shasum -a 256 {} + | sort -k2 > SHA256SUMS)
du -sk "$BUNDLE" | awk '{print "bundle size: " $1 " KiB"}'
