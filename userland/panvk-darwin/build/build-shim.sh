#!/bin/sh
# build-shim.sh PREFIX [host]: the libdrm replacement for iOS arm64e (or, with "host", for this Mac, which only
# satisfies the libdrm dependency of Mesa's build-machine tools) as PREFIX/lib/libdrm.a, headers and libdrm.pc.
set -eu
P=${1:?usage: build-shim.sh PREFIX [host]}
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ "${2:-}" = host ]; then
	SDKNAME=macosx
	TARGET=
else
	SDKNAME=iphoneos
	TARGET="-arch arm64e -miphoneos-version-min=26.0"
fi
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
W=$P/.work
mkdir -p "$P/lib/pkgconfig" "$P/include" "$W/inc"
ln -sfn "$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/IOKit.framework/Headers" "$W/inc/IOKit"
rsync -a "$HERE/shim/include/" "$P/include/"
xcrun --sdk $SDKNAME clang $TARGET -isysroot "$SDK" -O2 -g -Wall -Werror -fvisibility=hidden \
	-I "$W/inc" -I "$P/include" -I "$P/include/libdrm" -c "$HERE/shim/husky_drm.c" -o "$W/husky_drm.o"
rm -f "$P/lib/libdrm.a"
xcrun --sdk $SDKNAME ar rcs "$P/lib/libdrm.a" "$W/husky_drm.o"
cat > "$P/lib/pkgconfig/libdrm.pc" <<PC
prefix=$P
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: libdrm
Description: libdrm replacement carrying panthor ioctls over the husky IOKit user client
Version: 2.4.134
Libs: -L\${libdir} -ldrm -framework IOKit
Cflags: -I\${includedir} -I\${includedir}/libdrm
PC
