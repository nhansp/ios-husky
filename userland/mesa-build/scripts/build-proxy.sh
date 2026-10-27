#!/usr/bin/env bash
# aarch64-linux proxy build (arm64 codegen without an iOS SDK) + exported and undefined symbol audit.
# Compiles all TUs for arm64, builds libmali_mesa.a, compiles the smoke test object.
set -euo pipefail
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

if [ ! -f "$MESA_GEN/MESA_REV.txt" ]; then
  echo "MESA_GEN='$MESA_GEN' not found; running host-gen.sh..."
  bash "$HERE/scripts/host-gen.sh"
fi

cmake -S "$HERE" -B "$BUILD_PROXY" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/ios-arm64.toolchain.cmake" \
      -DMALI_PROXY_BUILD=ON \
      -DMESA_SRC="$MESA_SRC" -DMESA_GEN="$MESA_GEN"
ninja -C "$BUILD_PROXY"
python3 "$HERE/scripts/check_symbols.py" "$BUILD_PROXY/lib/libmali_mesa.a" --mode linux-proxy
