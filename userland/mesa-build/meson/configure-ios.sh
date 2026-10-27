#!/usr/bin/env bash
# ALTERNATIVE single-stage route: cross-build Mesa's own meson graph for iOS (macOS runner only).
# Primary route is CMake (scripts/build-ios.sh). Unverified on this box (no iOS SDK).
#
#   MESA_SRC=... ./meson/configure-ios.sh <build-dir>  &&  ninja -C <build-dir> $(scripts/make_manifest.py --print-ninja-targets)
#
# Differences from the host-gen.sh options: none of the OpenGL/gallium/vulkan options matter; they
# only keep nir_stub.c out (src/compiler/nir/meson.build:81). panfrost_compile/mesa_clc (libpan) are
# NOT part of this set (see scripts/host-libpan.sh).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?usage: configure-ios.sh <build-dir>}"
: "${MESA_SRC:?set MESA_SRC to a full Mesa checkout}"
ARCH="${MALI_IOS_ARCH:-arm64e}"; MIN="${MALI_IOS_MIN:-26.0}"
SDK="${MALI_IOS_SDK_PATH:-$(xcrun --sdk iphoneos --show-sdk-path)}"
PAC_ARGS=""
if [ "$ARCH" = "arm64e" ]; then
  PAC_ARGS=", '-fptrauth-calls', '-fptrauth-returns', '-fptrauth-auth-traps', '-fptrauth-indirect-gotos'"
fi
mkdir -p "$BUILD"
sed -e "s|@ARCH@|$ARCH|g" -e "s|@SDK@|$SDK|g" -e "s|@MIN@|$MIN|g" -e "s|@PAC_ARGS@|$PAC_ARGS|g" \
    "$HERE/meson/ios-arm64.cross.ini" > "$BUILD/ios-arm64.resolved.ini"
exec meson setup "$BUILD" "$MESA_SRC" --cross-file "$BUILD/ios-arm64.resolved.ini" \
  -Dbuildtype=release -Db_ndebug=true \
  -Dtools=panfrost \
  -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dvulkan-layers= \
  -Dopengl=true -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false \
  -Dgallium-va=disabled -Dgallium-rusticl=false -Dpanfrost-rust=false \
  -Dshader-cache=disabled -Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled
