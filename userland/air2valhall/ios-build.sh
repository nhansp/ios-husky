#!/bin/sh
# libMaliCompiler.dylib: air2valhall + Mesa + LLVM for iOS 26 arm64e, loaded by MaliMetal on a shader-pack miss.
#   MESA_SRC, MESA_TREE, MESA_LIB   as for plugin/MaliMetal/ios-build.sh
#   LLVM_DIR                        LLVM built for iphoneos arm64e (include/, lib/*.a)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA_SRC:?}" "${MESA_TREE:?}" "${MESA_LIB:?}" "${LLVM_DIR:?}"
CONTRACT=${CONTRACT:-$HERE/../contract}
SDK=${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}
OUT=${OUT:-$HERE/out}
OBJ=$OUT/obj
rm -rf "$OBJ"
mkdir -p "$OBJ"

ARCH="-arch arm64e -isysroot $SDK -miphoneos-version-min=26.0"
DEFS="$(cat "$MESA_TREE/mesa_defs.txt") -D_DARWIN_C_SOURCE"
T=$MESA_TREE
S=$MESA_SRC
INC="-I$HERE/include -I$HERE/src -I$CONTRACT -I$HERE/../mesa-build/shims/include"
for d in $T/src/panfrost $T/src/panfrost/genxml $T/src/panfrost/lib $T/src $T/include $T/src/compiler/nir \
	$T/src/util $T/src/compiler $T/src/compiler/glsl $S/src/panfrost/lib $S/src/panfrost/genxml $S/src/panfrost/shared \
	$S/src/panfrost/compiler $S/src/panfrost/compiler/bifrost $S/src/panfrost/model $S/src/panfrost $S/src $S/include \
	$S/src/util $S/src/compiler $S/src/compiler/nir $S/src/compiler/glsl; do
	INC="$INC -isystem $d"
done
INC="$INC -isystem $LLVM_DIR/include"

failed=""
for f in vh_desc vh_sampler vh_fau; do
	clang $ARCH -std=gnu11 -O2 -g -Wall -Wno-unused-parameter $DEFS $INC -c "$HERE/src/$f.c" -o "$OBJ/$f.o" ||
		failed="$failed $f"
done
for f in vh_frontend vh_lower_bindings vh_library; do
	clang++ $ARCH -std=c++17 -O2 -g -fno-rtti -Wno-deprecated-declarations $DEFS $INC -c "$HERE/src/$f.cpp" \
		-o "$OBJ/$f.o" || failed="$failed $f"
done
if [ -n "$failed" ]; then
	echo "failed:$failed" >&2
	exit 1
fi

clang++ $ARCH -dynamiclib -dead_strip -o "$OUT/libMaliCompiler.dylib" "$OBJ"/*.o "$MESA_LIB" "$LLVM_DIR"/lib/libLLVM*.a \
	-Wl,-exported_symbol,_air2vh_* -install_name @rpath/libMaliCompiler.dylib -lc++ -lz
echo "built $OUT/libMaliCompiler.dylib"
