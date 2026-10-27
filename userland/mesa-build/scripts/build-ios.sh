#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# iOS static libraries: wrapper invoking the root ios-build.sh entry point.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# If architectures are passed as positional arguments, export ARCH
if [ $# -gt 0 ]; then
  ARCH="$*"
  export ARCH
fi

exec bash "$HERE/ios-build.sh" "$@"
