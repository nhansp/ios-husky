#!/bin/sh
# MaliMetal.bundle for iOS 26 arm64e.
#   MESA_SRC   Mesa checkout at userland/mesa-build's pinned revision
#   MESA_TREE  generated headers + mesa_defs.txt (userland/mesa-build ios-build.sh MESA_BUILD_TREE)
#   MESA_LIB   libmali_mesa.a
#   PACK       shaders.mvpack to ship (optional)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA_SRC:?}" "${MESA_TREE:?}" "${MESA_LIB:?}"
CONTRACT=${CONTRACT:-$HERE/../../contract}
SDK=${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}
OUT=${OUT:-$HERE/out}
BUNDLE=$OUT/MaliMetal.bundle
OBJ=$OUT/obj
rm -rf "$BUNDLE" "$OBJ"
mkdir -p "$BUNDLE" "$OBJ"

ARCH="-arch arm64e -isysroot $SDK -miphoneos-version-min=26.0"
DEFS="$(cat "$MESA_TREE/mesa_defs.txt") -D_DARWIN_C_SOURCE"
T=$MESA_TREE
S=$MESA_SRC
MESA_INC="-I$T/src/panfrost -I$T/src/panfrost/genxml -I$T/src/panfrost/lib -I$T/src -I$T/include \
 -I$T/src/compiler/nir -I$T/src/util -I$T/src/compiler \
 -I$S/src/panfrost/lib -I$S/src/panfrost/genxml -I$S/src/panfrost/shared -I$S/src/panfrost/compiler \
 -I$S/src/panfrost/model -I$S/src/panfrost -I$S/src -I$S/include -I$S/src/util -I$S/src/compiler -I$S/src/compiler/nir"

for f in mali_formats mali_texture mali_desc mali_state mali_cs; do
	clang $ARCH -std=gnu11 -O2 -g -Wall -Werror -DPAN_ARCH=11 $DEFS \
		-I"$HERE/core" -I"$HERE/compat" -I"$CONTRACT" $MESA_INC -c "$HERE/core/$f.c" -o "$OBJ/$f.o"
done

for f in mvpack air2vh_offline; do
	clang $ARCH -std=gnu11 -O2 -g -Wall -Werror -I"$CONTRACT" -c "$HERE/offline/$f.c" -o "$OBJ/$f.o"
done

for f in MaliMTLDevice MaliResources MaliResourceUsage MaliCommandBuffer MaliShader MaliPipeline MaliInternal \
	MaliBindings MaliRenderEncoder MaliComputeEncoder MaliBlitEncoder; do
	clang $ARCH -fobjc-arc -std=gnu11 -O2 -g -Wall -Werror -I"$HERE" -I"$HERE/core" -I"$CONTRACT" \
		-c "$HERE/$f.m" -o "$OBJ/$f.o"
done

clang $ARCH -bundle -dead_strip -o "$BUNDLE/MaliMetal" "$OBJ"/*.o "$MESA_LIB" \
	"$HERE/stubs/IOGPU.tbd" "$HERE/stubs/Metal.tbd" \
	-framework Foundation -framework CoreFoundation -framework IOKit -framework IOSurface -lc++ -lz
cp "$HERE/Info.plist" "$BUNDLE/Info.plist"
if [ -n "${PACK:-}" ]; then cp "$PACK" "$BUNDLE/shaders.mvpack"; fi
echo "built $BUNDLE"
