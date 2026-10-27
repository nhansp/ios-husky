#!/bin/sh
# Host unit test of the MaliMetal core.
#   MESA_SRC     a Mesa checkout (src/panfrost, src/util, src/compiler, include)
#   MESA_BUILD   a Meson build dir of the same checkout with libpan-arch-v11.a and the
#                generated headers (genxml/v11_pack.h, util/format/u_format_gen.h, nir)
#   CONTRACT     directory holding iogpu_uc_abi.h mali_backend_ops.h air2vh_api.h
#   MESA_LIBS    optional: flat directory with the six fat archives instead of the Meson tree layout
#   MESA_DEFS    optional: Mesa's config -D list (compile_commands.json); a short default is used otherwise
# Everything is compiled with PAN_ARCH=11 exactly like the iOS build (build.sh).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$HERE/../../core
: "${MESA_SRC:?}" "${MESA_BUILD:?}" "${CONTRACT:?}"
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"
INC="-I$CORE -I$CORE/../compat -I$CONTRACT -I$MESA_BUILD/src/panfrost -I$MESA_BUILD/src/panfrost/genxml -I$MESA_BUILD/src/panfrost/lib -I$MESA_BUILD/src -I$MESA_BUILD/include \
 -I$MESA_SRC/src/panfrost/lib -I$MESA_SRC/src/panfrost/genxml -I$MESA_SRC/src/panfrost/shared -I$MESA_SRC/src/panfrost/compiler \
 -I$MESA_SRC/src/panfrost/model -I$MESA_SRC/src/panfrost -I$MESA_SRC/src -I$MESA_SRC/include -I$MESA_SRC/src/util -I$MESA_SRC/src/compiler \
 -I$MESA_SRC/src/compiler/nir -I$MESA_BUILD/src/compiler/nir -I$MESA_BUILD/src/util -I$MESA_BUILD/src/compiler"
# Mesa's own configuration macros. They are ABI: struct pan_varying_layout is 'packed' only with HAVE_FUNC_ATTRIBUTE_PACKED, so
# they must equal what the Mesa libraries and libair2vh were built with (compile_commands.json); compat/mesa_defs_common.txt is the
# portable subset of that list.
DEFS=${MESA_DEFS:-"$(cat "$CORE/../compat/mesa_defs_common.txt") -D_GNU_SOURCE -DHAVE_ENDIAN_H -DHAVE_STRTOK_R -DHAVE_SECURE_GETENV -DHAVE_GNU_QSORT_R -DHAVE_MEMFD_CREATE"}
CFLAGS="-std=gnu11 -O1 -g -Wall -Wno-constant-conversion -Wno-unused-function -DPAN_ARCH=11 $DEFS"
for f in mali_formats mali_texture mali_desc mali_state mali_cs; do
  clang $CFLAGS $INC -c "$CORE/$f.c" -o "$OUT/$f.o"
done
clang $CFLAGS $INC -c "$HERE/test_core.c" -o "$OUT/test_core.o"
# Libraries: MESA_LIBS = one flat directory holding the six archives (fat, not thin), else the Meson tree layout.
if [ -n "${MESA_LIBS:-}" ]; then
  LIBS="$MESA_LIBS/libpan-arch-v11.a $MESA_LIBS/libpan-format-v11.a $MESA_LIBS/libpanfrost_lib.a $MESA_LIBS/libpanfrost_model_lib.a \
        $MESA_LIBS/libmesa_util.a $MESA_LIBS/libmesa_util_c11.a"
else
  L="$MESA_BUILD/src/panfrost/lib"
  LIBS="$L/libpan-arch-v11.a $L/libpan-format-v11.a $L/libpanfrost_lib.a $MESA_BUILD/src/panfrost/model/libpanfrost_model_lib.a \
        $MESA_BUILD/src/util/libmesa_util.a $MESA_BUILD/src/c11/impl/libmesa_util_c11.a"
fi
clang -o "$OUT/test_core" "$OUT"/*.o $LIBS -lm -lpthread -lz -lzstd
"$OUT/test_core"
