# Shared settings for the mesa-build scripts. Source, do not execute.
#
#   MESA_SRC   a FULL Mesa checkout (the sparse gpuref/mesa tree cannot build: spec 06 s8 item 1)
#   WORK       scratch root; every generated/built artifact goes here, never next to these scripts
#   VENV       python venv holding mako + meson (stdlib-only is NOT enough: spec 06 s8 item 6)

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"

# Resolve repository root and default out/ directory relative to this script
REPO_ROOT="$(cd "$HERE/../.." 2>/dev/null && pwd || true)"

# Default scratch directory under out/work
export WORK="${WORK:-$HERE/../../out/work}"
mkdir -p "$WORK"

# Auto-detect MESA_SRC if not given in environment
if [ -z "${MESA_SRC:-}" ]; then
  for cand in "$HERE/../../../refs/mesa" "$HERE/../../refs/mesa" "$HERE/../refs/mesa" "$REPO_ROOT/refs/mesa" "$REPO_ROOT/mesa"; do
    if [ -f "$cand/meson.build" ]; then
      export MESA_SRC="$(cd "$cand" && pwd)"
      break
    fi
  done
fi

if [ -z "${MESA_SRC:-}" ] || [ ! -f "${MESA_SRC}/meson.build" ]; then
  echo "ERROR: MESA_SRC is not set or not a valid Mesa tree (${MESA_SRC:-unset})" >&2
  echo "Please set MESA_SRC to a full Mesa checkout (e.g. export MESA_SRC=/path/to/mesa)" >&2
  return 1 2>/dev/null || exit 1
fi

export VENV="${VENV:-$WORK/venv}"

# host (Linux/macOS) generator build, produces the generated sources/headers
export HOST_BUILD="${HOST_BUILD:-$WORK/build-min}"
# staged copy of exactly the generated files the CMake build needs (see host-gen.sh)
export MESA_GEN="${MESA_GEN:-$WORK/gen}"

# per-target CMake build roots
export BUILD_IOS="${BUILD_IOS:-$WORK/cmake-ios-arm64}"
export BUILD_PROXY="${BUILD_PROXY:-$WORK/cmake-proxy-aarch64-linux}"
export BUILD_HOST="${BUILD_HOST:-$WORK/cmake-host}"

# bootstrap the venv when missing
mali_mesa_ensure_venv() {
  if [ ! -x "$VENV/bin/python3" ] || [ ! -x "$VENV/bin/meson" ]; then
    python3 -m venv --system-site-packages "$VENV"
    # Prefer offline wheel cache if available
    if [ -d "$HOME/.cache/pip/wheels" ]; then
      "$VENV/bin/pip" install --find-links "$HOME/.cache/pip/wheels" 'mako==1.4.3' 'markupsafe==3.0.4' 'meson==1.12.1' 'packaging' 'pyyaml' 2>/dev/null || \
      "$VENV/bin/pip" install 'mako==1.4.3' 'markupsafe==3.0.4' 'meson==1.12.1' 'packaging' 'pyyaml'
    else
      "$VENV/bin/pip" install 'mako==1.4.3' 'markupsafe==3.0.4' 'meson==1.12.1' 'packaging' 'pyyaml'
    fi
  fi
  "$VENV/bin/python3" -c 'import mako, mesonbuild, yaml' >/dev/null
}
