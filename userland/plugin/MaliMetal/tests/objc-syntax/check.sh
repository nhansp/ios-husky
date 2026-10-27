#!/bin/sh
# Syntax/type check of the Objective-C layer on a Linux host (no Apple SDK needed).
# Intended repo path: plugin/MaliMetal/tests/objc-syntax/check.sh
#
# The stubs/ directory holds *minimal* stand-ins for Foundation, Metal, IOKit, IOSurface, CommonCrypto, os/log and the
# mach-o section API: only what the plug-in's own sources use. They exist to catch typos, missing declarations,
# wrong selector/method names inside the plug-in and ARC/bridging mistakes; they say nothing about the real SDK's
# protocol requirements (build.sh against the iOS SDK is the real check).
#   CONTRACT  directory holding iogpu_uc_abi.h mali_backend_ops.h air2vh_api.h
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
: "${CONTRACT:?}"
rc=0
for f in MaliMTLDevice MaliResources MaliResourceUsage MaliCommandBuffer MaliShader MaliPipeline MaliInternal MaliBindings \
         MaliRenderEncoder MaliComputeEncoder MaliBlitEncoder; do
  echo "== $f.m"
  clang -x objective-c -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fconstant-string-class=NSConstantString -fsyntax-only \
    -D_GNU_SOURCE -std=gnu11 -Wall -Wincomplete-implementation -Wundeclared-selector -Wno-unused-function -Wno-objc-method-access \
    -Wno-nullability-completeness -Wno-gnu-zero-variadic-macro-arguments -Wno-unused-value -Wno-protocol \
    -Wno-objc-protocol-property-synthesis -Wno-sizeof-array-decay \
    -I"$HERE/stubs" -I"$ROOT" -I"$ROOT/core" -I"$CONTRACT" "$ROOT/$f.m" || rc=1
done
exit $rc
