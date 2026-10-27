#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ios-build.sh - CI entry point for building Mesa arch-11 static library for iOS.
#
# Called by CI:
#   MESA_SRC=... MESA_OUT_LIB=... MESA_BUILD_TREE=... SDK=... PLATFORM=... ARCH=... ios-build.sh
#
# Dry-run flag:
#   ios-build.sh --print-plan
#   Prints the planned CMake invocation without requiring Xcode or macOS.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration & environment variables
PLATFORM="${PLATFORM:-iphoneos}"
ARCH="${ARCH:-arm64e}"
DEPLOY_TARGET="${DEPLOY_TARGET:-26.0}"

# Dry run / print plan check
if [ "${1:-}" = "--print-plan" ]; then
  PLAN_SDK="${SDK:-iphoneos}"
  PLAN_SRC="${MESA_SRC:-<MESA_SRC>}"
  PLAN_BUILD="${MESA_BUILD_TREE:-$HERE/out/build}"
  PLAN_OUT_LIB="${MESA_OUT_LIB:-$HERE/out/lib}"

  ARCH_LIST=()
  for a in $ARCH; do
    ARCH_LIST+=("$a")
  done
  [ ${#ARCH_LIST[@]} -eq 0 ] && ARCH_LIST=(arm64e)

  has_arm64e=0
  for a in "${ARCH_LIST[@]}"; do
    if [ "$a" = "arm64e" ]; then
      has_arm64e=1
      break
    fi
  done

  if [ "$has_arm64e" -eq 1 ]; then
    PAC_STR="-fptrauth-calls -fptrauth-returns -fptrauth-auth-traps -fptrauth-indirect-gotos (applied to arm64e)"
  else
    PAC_STR="none"
  fi

  echo "=== Planned iOS CMake Configuration ==="
  echo "Target Platform   : $PLATFORM"
  echo "Architectures     : ${ARCH_LIST[*]}"
  echo "Deployment Target : $DEPLOY_TARGET"
  echo "SDK Sysroot       : $PLAN_SDK"
  echo "Pointer Auth Flags: $PAC_STR"
  echo ""
  echo "Planned invocations:"
  planned_archives=()
  for a in "${ARCH_LIST[@]}"; do
    bld_dir="$PLAN_BUILD/$a"
    echo "# Build slice: $a"
    echo "cmake -S \"$HERE\" -B \"$bld_dir\" -G Ninja \\"
    echo "      -DCMAKE_TOOLCHAIN_FILE=\"$HERE/cmake/ios-arm64.toolchain.cmake\" \\"
    echo "      -DCMAKE_OSX_SYSROOT=\"$PLAN_SDK\" \\"
    echo "      -DCMAKE_OSX_DEPLOYMENT_TARGET=\"$DEPLOY_TARGET\" \\"
    echo "      -DMALI_IOS_ARCH=\"$a\" \\"
    echo "      -DMESA_SRC=\"$PLAN_SRC\" \\"
    echo "      -DMESA_GEN=\"$PLAN_BUILD\" \\"
    echo "      -DMESA_BUILD_TREE=\"$PLAN_BUILD\" \\"
    echo "      -DCMAKE_BUILD_TYPE=Release"
    echo "ninja -C \"$bld_dir\" mali_mesa_combined"
    planned_archives+=("$bld_dir/lib/libmali_mesa.a")
  done
  echo ""
  echo "# Packaging static library:"
  if [ ${#planned_archives[@]} -gt 1 ]; then
    echo "lipo -create ${planned_archives[*]} -output \"$PLAN_OUT_LIB/libmali_mesa.a\""
  else
    echo "mkdir -p \"$PLAN_OUT_LIB\""
    echo "cp \"${planned_archives[0]}\" \"$PLAN_OUT_LIB/libmali_mesa.a\""
  fi
  echo "# Staging definitions & metadata:"
  echo "cp \"$HERE/mesa_defs.txt\" \"$PLAN_BUILD/\""
  echo "echo \"rev=...\" > \"$PLAN_OUT_LIB/META\""
  exit 0
fi

# Normal execution requires macOS with Xcode/iPhoneOS SDK
if [ "$(uname -s)" != "Darwin" ]; then
  echo "ERROR: ios-build.sh requires macOS + Xcode (running on $(uname -s)). Use --print-plan on non-Darwin hosts." >&2
  exit 2
fi

# Ensure MESA_SRC is provided
if [ -z "${MESA_SRC:-}" ] || [ ! -f "${MESA_SRC}/meson.build" ]; then
  echo "ERROR: MESA_SRC is not set or not a valid Mesa tree (${MESA_SRC:-unset})" >&2
  exit 1
fi

MESA_OUT_LIB="${MESA_OUT_LIB:-$HERE/out/lib}"
MESA_BUILD_TREE="${MESA_BUILD_TREE:-$HERE/out/build}"
SDK="${SDK:-$(xcrun --sdk "$PLATFORM" --show-sdk-path)}"
MESA_REV="$(cd "$MESA_SRC" && { git rev-parse HEAD 2>/dev/null || cat VERSION 2>/dev/null || echo "1f9035d9c04ca391a58155159027b1d85fba8be8"; })"

mkdir -p "$MESA_OUT_LIB" "$MESA_BUILD_TREE"

# Step 1: Ensure generated files are staged
if [ ! -f "$MESA_BUILD_TREE/src/panfrost/genxml/v11_pack.h" ]; then
  if [ -n "${MESA_GEN:-}" ] && [ -f "$MESA_GEN/src/panfrost/genxml/v11_pack.h" ]; then
    cp -R "$MESA_GEN/." "$MESA_BUILD_TREE/"
  else
    MESA_GEN="$MESA_BUILD_TREE" HOST_BUILD="$MESA_BUILD_TREE/host-build" bash "$HERE/scripts/host-gen.sh"
  fi
fi

# Step 2: Build slices for requested architectures
# Support building single slice (arm64e or arm64) or both slices and lipo
ARCH_LIST=()
for a in $ARCH; do
  ARCH_LIST+=("$a")
done
if [ ${#ARCH_LIST[@]} -eq 0 ]; then
  ARCH_LIST=(arm64e)
fi

built_archives=()
for a in "${ARCH_LIST[@]}"; do
  bld_dir="$MESA_BUILD_TREE/$a"
  cmake -S "$HERE" -B "$bld_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/ios-arm64.toolchain.cmake" \
        -DCMAKE_OSX_SYSROOT="$SDK" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY_TARGET" \
        -DMALI_IOS_ARCH="$a" \
        -DMESA_SRC="$MESA_SRC" \
        -DMESA_GEN="$MESA_BUILD_TREE" \
        -DMESA_BUILD_TREE="$MESA_BUILD_TREE" \
        -DCMAKE_BUILD_TYPE=Release
  ninja -C "$bld_dir" mali_mesa_combined
  built_archives+=("$bld_dir/lib/libmali_mesa.a")
done

# Stage definition files to MESA_BUILD_TREE
cp "$HERE/mesa_defs.txt" "$HERE/mesa_defs.mk" "$MESA_BUILD_TREE/" 2>/dev/null || true

# Step 3: Output archive to MESA_OUT_LIB
if [ ${#built_archives[@]} -gt 1 ]; then
  lipo -create "${built_archives[@]}" -output "$MESA_OUT_LIB/libmali_mesa.a"
  echo "Created multi-arch universal static library at $MESA_OUT_LIB/libmali_mesa.a"
else
  cp "${built_archives[0]}" "$MESA_OUT_LIB/libmali_mesa.a"
  echo "Created static library at $MESA_OUT_LIB/libmali_mesa.a"
fi

# Step 4: Export metadata file META
META_FILE="$(dirname "$MESA_OUT_LIB")/META"
{
  echo "rev=$MESA_REV"
  echo "url=${MESA_URL:-https://gitlab.freedesktop.org/mesa/mesa.git}"
  echo "platform=$PLATFORM arch=$ARCH target=$DEPLOY_TARGET sdk=$SDK"
  echo "options=cmake-ios-arm64"
} > "$META_FILE"
# Also mirror inside MESA_OUT_LIB if different directory
if [ "$META_FILE" != "$MESA_OUT_LIB/META" ]; then
  cp "$META_FILE" "$MESA_OUT_LIB/META"
fi

# Step 5: Symbol audit
NM_TOOL="$(xcrun --sdk "$PLATFORM" -f llvm-nm 2>/dev/null || xcrun -f llvm-nm 2>/dev/null || echo llvm-nm)"
python3 "$HERE/scripts/check_symbols.py" "$MESA_OUT_LIB/libmali_mesa.a" --mode darwin --nm "$NM_TOOL" --sdk "$SDK"
