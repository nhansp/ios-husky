#!/usr/bin/env bash
# Build + RUN the smoke test on the build machine (x86_64 or arm64 macOS/Linux).
# Fastest way to check a manifest/define change: executes the real Valhall compiler, packers, and ABI asserts.
#   scripts/build-host.sh [assert]      ("assert" keeps assert(); use for cs_builder bring-up)
set -euo pipefail
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

if [ ! -f "$MESA_GEN/MESA_REV.txt" ]; then
  echo "MESA_GEN='$MESA_GEN' not found; running host-gen.sh..."
  bash "$HERE/scripts/host-gen.sh"
fi

OPTS=()
B="$BUILD_HOST"
if [ "${1:-}" = "assert" ]; then
  OPTS+=(-DMALI_MESA_ASSERTS=ON -DCMAKE_BUILD_TYPE=Debug)
  B="$BUILD_HOST-assert"
fi

cmake -S "$HERE" -B "$B" -G Ninja \
      -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
      -DMESA_SRC="$MESA_SRC" -DMESA_GEN="$MESA_GEN" ${OPTS[@]+"${OPTS[@]}"}
ninja -C "$B"
"$B/mali_mesa_smoke"
