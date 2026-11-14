#!/bin/sh
# vm-build.sh: runs in the macOS VM (see tools/dev/vm-panvk.sh). Expects ~/husky/panvk/src/{panvk-darwin,vk-smoke}.
# Builds libvulkan_panfrost.dylib (arm64e, iOS 26) and vk_smoke into ~/husky/panvk/out. Incremental: the native
# toolchain, the Mesa tree, both meson build directories and the object files persist between runs.
set -eu
W=$HOME/husky/panvk
SRC=$W/src/panvk-darwin
REV=${MESA_REV:?}
J=${PANVK_JOBS:-$(sysctl -n hw.ncpu)}
export PATH=$W/cache/native/venv/bin:/usr/local/bin:$PATH
export LDFLAGS=
cd "$W"

sh "$SRC/build/native-deps.sh" > "$W/native-deps.log" 2>&1 || { tail -30 "$W/native-deps.log"; exit 1; }

MESA=$W/mesa
if [ ! -f "$MESA/meson.build" ]; then
	rm -rf "$MESA" && cp -a "$HOME/husky/ucache/mesa-$REV" "$MESA"
fi
[ "$(git -C "$MESA" rev-parse HEAD)" = "$REV" ] || { echo "Mesa tree is not $REV" >&2; exit 1; }

# Patch series: re-applied from the pristine tree whenever it changes.
STAMP=$(cat "$SRC"/patches/series "$SRC"/patches/*.patch | shasum -a 256 | cut -c1-16)
if [ "$(cat "$MESA/.husky-patches" 2>/dev/null)" != "$STAMP" ]; then
	git -C "$MESA" checkout -q -- . && git -C "$MESA" clean -qfd
	while read -r p; do
		case $p in ''|'#'*) continue ;; esac
		git -C "$MESA" apply --whitespace=nowarn "$SRC/patches/$p" || { echo "patch $p does not apply" >&2; exit 1; }
	done < "$SRC/patches/series"
	echo "$STAMP" > "$MESA/.husky-patches"
fi

SHIM=$W/shim-prefix
sh "$SRC/build/build-shim.sh" "$SHIM"

# Build-machine tools: OpenCL-C -> SPIR-V -> NIR for the libpan shaders.
TOOLS=$W/tools-bin
mkdir -p "$TOOLS"
sh "$SRC/build/build-shim.sh" "$W/shim-host" host
mkdir -p "$W/ini"
cat > "$W/ini/native-host.ini" <<INI
[binaries]
c = '/usr/bin/clang'
cpp = '/usr/bin/clang++'
objc = '/usr/bin/clang'
llvm-config = '$W/cache/native/bin/llvm-config'

[built-in options]
pkg_config_path = ['$W/shim-host/lib/pkgconfig', '$W/cache/native/lib/pkgconfig']
INI
if [ ! -d "$W/bd-native" ]; then
	meson setup "$W/bd-native" "$MESA" --native-file "$W/ini/native-host.ini" --wrap-mode=nodownload -Dbuildtype=release -Dvulkan-drivers=panfrost \
		-Dgallium-drivers= -Dplatforms= -Dllvm=enabled -Dpanfrost-rust=false -Dmesa-clc=enabled -Dprecomp-compiler=enabled \
		-Dinstall-mesa-clc=true -Dinstall-precomp-compiler=true -Dglx=disabled -Degl=disabled -Dgbm=disabled \
		-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dtools= \
		-Dbuild-tests=false -Dvideo-codecs= -Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled \
		-Dexpat=disabled -Dzstd=disabled -Dshader-cache=disabled -Dspirv-tools=enabled -Dshared-llvm=disabled
fi
ninja -C "$W/bd-native" -j "$J" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile
cp "$W/bd-native/src/compiler/clc/mesa_clc" "$W/bd-native/src/compiler/spirv/vtn_bindgen2" \
	"$W/bd-native/src/panfrost/clc/panfrost_compile" "$TOOLS/"

sh "$SRC/build/mk-cross.sh" "$W/ini" "$SHIM" "$TOOLS"
OPTS="-Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dllvm=disabled -Dpanfrost-rust=false -Dmesa-clc=system
-Dprecomp-compiler=system -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
-Dshared-glapi=disabled -Dxmlconfig=disabled -Dtools= -Dbuild-tests=false -Dvideo-codecs= -Dvulkan-layers=
-Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled -Dexpat=disabled -Dzstd=disabled -Dbuildtype=release
-Db_ndebug=true -Dshader-cache=enabled"
OKEY=$( (printf '%s' "$OPTS"; cat "$W/ini/cross.ini" "$W/ini/native.ini") | shasum -a 256 | cut -c1-16)
if [ ! -f "$W/bd-cross/build.ninja" ] || [ "$(cat "$W/bd-cross/.opts" 2>/dev/null)" != "$OKEY" ]; then
	rm -rf "$W/bd-cross"
	# shellcheck disable=SC2086
	meson setup "$W/bd-cross" "$MESA" --cross-file "$W/ini/cross.ini" --native-file "$W/ini/native.ini" \
		--wrap-mode=nodownload $OPTS
	echo "$OKEY" > "$W/bd-cross/.opts"
fi
ninja -C "$W/bd-cross" -j "$J" src/panfrost/vulkan/libvulkan_panfrost.dylib

O=$W/out
rm -rf "$O" && mkdir -p "$O"
cp "$W/bd-cross/src/panfrost/vulkan/libvulkan_panfrost.dylib" "$O/"
# Mesa's entrypoint tables take the address of every Vulkan function through weak declarations; the ones panvk does not
# implement are weak flat imports (hence -undefined dynamic_lookup). Bind those to the image so dyld leaves them NULL
# without a flat-namespace search, and make sure no strong import slipped through that flat lookup.
if nm -um "$O/libvulkan_panfrost.dylib" | grep 'dynamically looked up' | grep -v 'weak' >&2; then
	echo "strong undefined symbols in libvulkan_panfrost.dylib" >&2
	exit 1
fi
python3 "$SRC/build/macho-self-bind-weak-imports.py" "$O/libvulkan_panfrost.dylib"
xcrun -sdk iphoneos install_name_tool -id /usr/local/lib/libvulkan_panfrost.dylib "$O/libvulkan_panfrost.dylib"
codesign -f -s - "$O/libvulkan_panfrost.dylib"

SM=$W/src/vk-smoke
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -O1 -Wall -Werror -I "$MESA/include" -I "$SM" \
	"$SM/vk_smoke.c" -L "$O" -lvulkan_panfrost -o "$O/vk_smoke"
codesign -f -s - --entitlements "$SM/ent.plist" "$O/vk_smoke"
cp "$SM/com.husky.vk-smoke.plist" "$O/"
(cd "$O" && shasum -a 256 libvulkan_panfrost.dylib vk_smoke com.husky.vk-smoke.plist > SHA256SUMS)
