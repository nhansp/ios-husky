# mesa-build: Mesa arch-11 (Mali-G715 / Valhall CSF) as iOS static libraries

Build integration only: CMake + a generated source manifest + scripts + two shims. No Mesa
source or build tree is stored in this repository; Mesa (MIT) is compiled from a separate full checkout.
Spec: `artifacts/research/gpu-spec/06-mali-emit-path.md` s4 (what to compile) and s8 (iOS build
concerns). Pinned Mesa revision: `1f9035d9c04ca391a58155159027b1d85fba8be8` (27.0.0-devel).

## What you get

`libmali_mesa.a` (one archive, all objects) plus 20 per-component `libmm_*.a`, covering:

| spec 06 | component | libraries |
|---|---|---|
| P1 | genxml v11 packers (`v11_pack.h`, `common_pack.h`, `pan_pack*`) | generated headers (header-only, in `mali_mesa_headers`) |
| P2 | `cs_builder.h` (header-only; needs ralloc + `util_compute_fast_udiv_info`) | `mm_util` |
| P3 | model, props | `mm_pan_model`, `mm_pan_lib` (`pan_props.c`) |
| P4 | FB/TLS/tiler descriptors | `mm_pan_arch_v11` (`pan_desc.c`, `pan_fb.c`, `pan_fb_nir.c`), `mm_pan_lib` (`pan_tiler.c`, `pan_clear.c`, `pan_earlyzs.c`, `pan_samples.c`) |
| P5 | texture / format / layout / AFBC | `mm_pan_arch_v11` (`pan_texture.c`, `pan_mod.c`, `pan_buffer.c`), `mm_pan_format_v11`, `mm_pan_lib` (`pan_layout.c`, `pan_afbc.c`), `mm_pan_shared` (`pan_tiling.c`) |
| P6 | blend | `mm_pan_arch_v11` + `mm_pan_lib` (`pan_blend.c`), `mm_pan_compiler` (`pan_nir_lower_framebuffer.c`) |
| P7 | Valhall compiler (NIR -> Valhall binary) | `mm_pan_compiler`, `mm_pan_bifrost` (incl. the clause packer, spec s8 item 8), `mm_pan_midgard` (link surface of `pan_compiler.c`), `mm_pan_{valhall,bifrost,midgard}_disasm` |
| P9 | NIR + util | `mm_nir` (238 TUs, real NIR), `mm_util`, `mm_util_c11`, `mm_blake3`, `mm_util_simd`, `mm_compiler` (glsl_types), `mm_isaspec` |
| optional | pandecode (CS + descriptor dumper, debug aid) | `-DMALI_MESA_WITH_DECODE=ON`: `mm_pandecode_v11`, `mm_pandecode` |
| optional | precompiled libpan kernels (P8) | `scripts/host-libpan.sh` then `-DMALI_MESA_LIBPAN_DIR=...`: `mm_libpan_v11` |

469 translation units in 20 libraries; the exact list (S = in Mesa tree, G = generated) is
`sources/panfrost-subset.txt`, the 29 generated files are `sources/generated-files.txt`.
Unstripped `libmali_mesa.a` is about 19 MiB (aarch64, Release); the plug-in link uses
`-dead_strip` (everything is built with `-ffunction-sections -fdata-sections`) so the linked footprint is far smaller.

## Pipeline & CI Integration

```
 stage 1  scripts/host-gen.sh       any host with python3 (+ pip: mako, meson)   -> $MESA_GEN (29 files)
 stage 2  scripts/make_manifest.py  stdlib only; rewrites meson's compile db      -> cmake/mesa_sources.cmake
 stage 3  CMakeLists.txt            clang, per target                              -> libmali_mesa.a
            ios-build.sh             CI entry point hook for iOS arm64e/arm64
            scripts/build-proxy.sh   aarch64-linux proxy (verification without an SDK)
            scripts/build-host.sh    native build + runs tests/smoke.c
```

### CI Hook Contract (`ios-build.sh`)
Located at the root of `mesa-build/` as required by CI (`ci/.github/scripts/gpu/build-mesa-ios.sh:62-65`).
Accepts the following environment variables:
- `MESA_SRC`: path to pinned Mesa clone
- `MESA_OUT_LIB`: output directory where `libmali_mesa.a` is copied
- `MESA_BUILD_TREE`: output directory where generated headers (`gen/` + `mesa_defs.txt`) are copied
- `SDK`: target SDK (defaults to `iphoneos`)
- `PLATFORM`: target platform (`iphoneos`, `iphonesimulator`, or `macosx`)
- `ARCH`: architecture slice(s) (defaults to `arm64e`, supports `arm64e arm64`)
- `--print-plan`: dry-run mode printing planned cmake invocation and flags without requiring Xcode or an SDK.

`ios-build.sh` writes a `META` file into `MESA_OUT_LIB` containing `rev=1f9035d9c04ca391a58155159027b1d85fba8be8`.

### Unified Toolchain (`cmake/ios-arm64.toolchain.cmake`)
Supports both Apple iOS cross-compilation and Linux aarch64 proxy compilation:
- **iOS arm64e / arm64**:
  - Minimum deployment target: iOS 26.0 (`-miphoneos-version-min=26.0`).
  - arm64e PAC flags matching Apple clang: `-fptrauth-calls -fptrauth-returns -fptrauth-auth-traps -fptrauth-indirect-gotos`.
  - No `-march` guessing: Apple clang targets the native Apple CPU profile.
- **Linux aarch64 proxy**:
  - Activated via `-DMALI_PROXY_BUILD=ON` or when `CMAKE_SYSROOT` points to a Linux sysroot (`/usr/aarch64-linux-gnu`).
  - Sets `-target aarch64-linux-gnu`.

## Canonical Preprocessor Definitions (`cmake/mesa_defs.cmake`)

ONE single source of truth for preprocessor definitions, replacing previous divergent lists across `mesa_config.cmake`, `mesa_defs_common.txt`, and `mesa_defs.mk`.

Generated files exported at configure time:
- `mesa_defs.txt`: consumed by shell scripts (Metal plug-in `build.sh`, CI `build-common.sh`).
- `mesa_defs.mk`: consumed by host Makefiles (`tests/common/`).

### Linux vs Darwin Define Comparison

Derived from `compile_commands.json` of Mesa 1f9035d configured with minimal Panfrost options:

| Flag | Linux | Darwin (iOS) | Rationale |
|---|---|---|---|
| `HAVE_FUNC_ATTRIBUTE_PACKED` | Yes | Yes | **ABI CRITICAL**: Required for `sizeof(struct pan_varying_layout) == 152`. Asserted at compile time. |
| `HAVE_FUNC_ATTRIBUTE_ALIAS` | Yes | **No** | Mach-O does not support symbol aliases (`aliases are not supported on darwin`). |
| `_GNU_SOURCE` | Yes | **No** | Darwin uses BSD headers; `_GNU_SOURCE` enables Linux-specific extensions not present in XNU/Darwin libc. |
| `_DARWIN_C_SOURCE` | No | **Yes** | Exposes Darwin/BSD libc extensions (`mkostemp`, `flock`, `dladdr`, `MAP_ANONYMOUS`, etc.) under `-std=c11` (`__STRICT_ANSI__`). |
| `HAVE_REALLOCARRAY` | Yes | **No** | Absent in Apple `libSystem`; Mesa falls back to its inline definition in `util/reallocarray.h`. |
| `HAVE_SC_PHYS_PAGES` | Yes | **No** | POSIX `_SC_PHYS_PAGES` absent on Darwin. Unsetting it allows `os_misc.c:379` to reach the `#elif DETECT_OS_APPLE` branch using `sysctl(CTL_HW, HW_MEMSIZE)`. |
| `HAVE_MEMFD_CREATE`, `HAVE_GETRANDOM`, `HAVE_POSIX_FALLOCATE` | Yes | **No** | Linux syscalls absent on Darwin libc / iOS SDK. |
| `HAVE_LINUX_FUTEX_H`, `HAVE_SYS_INOTIFY_H`, `HAVE_SYS_SHM_H` | Yes | **No** | Linux kernel headers absent on Darwin. |
| `HAVE_ZLIB`, `HAVE_ZSTD`, `HAVE_COMPRESSION` | No | **No** | Shader cache is disabled; iOS SDK lacks libzstd; no compression libraries linked. |
| `PAN_ARCH` | *Per-target* | *Per-target* | **Must not be defined globally**: Arch-independent libraries (`mm_pan_lib`, `pan_samples.c`) require `PAN_ARCH` to be undefined so `gen_macros.h` includes `common_pack.h`. Defined only for arch-specific libraries (`mm_pan_arch_v11`, `mm_pan_format_v11`, `mm_pandecode_v11`) and downstream consumers (`smoke.c`, Metal plug-in). |

### Compile-Time ABI Assertions
Both `cmake/mesa_defs.cmake` and `shims/include/mali_mesa_abi_check.h` assert:
1. `HAVE_FUNC_ATTRIBUTE_PACKED` is defined.
2. `sizeof(struct pan_varying_layout) == 152`.
3. `sizeof(struct pan_varying_slot) == 4`.
4. `sizeof(enum pan_varying_knowledge) == 1` (without packed attribute, it defaults to 4 bytes, inserting 3 bytes padding after `count` and 2 bytes after `generic_size_B`, increasing layout size to 160).
5. Exact field offsets: `count` @ 0, `known` @ 1, `generic_size_B` @ 2, `slots` @ 4.

## Consumer Rules

### Link Order & Combined Archive
- **Apple ld64**: Resolves static archive symbols order-independently using multi-pass symbol resolution.
- **GNU ld / bfd ld**: Static libraries that reference each other circularly (`mm_util` <-> `mm_nir` <-> `mm_pan_compiler`) require `--start-group ... --end-group` or a single combined archive.
- **Always link the combined archive `libmali_mesa.a`**: It cleanly bundles all 20 libraries into one file, eliminating link order sensitivity on all platforms.
- **Duplicate member handling**: `libmali_mesa.a` contains objects with identical basenames from different modules:
  - `pan_blend.c.o` (in `mm_pan_arch_v11` and `mm_pan_lib`)
  - `disassemble.c.o` (in `mm_pan_bifrost_disasm` and `mm_pan_midgard_disasm`)
  - `midgard_ops.c.o` (in `mm_pan_midgard` and `mm_pan_midgard_disasm`)
  These are preserved by MRI scripts on Linux (`ar -M`) and accepted by Apple `libtool -static`.

### Header Include Order Rule
`contract/mali_backend_ops.h` defines opcode macros such as `#define MALI_CS_CALL ...` and `#define MALI_CS_ERROR_BARRIER ...`.
Mesa's `genxml/v11_pack.h` and `cs_builder.h` declare `struct MALI_CS_CALL` and `struct MALI_CS_ERROR_BARRIER`.
**Rule**: Always include Mesa headers (`genxml/v11_pack.h`, `cs_builder.h`, `pan_*.h`) **before** contract headers (`mali_backend_ops.h`) to avoid preprocessor macro collisions with C struct declarations.

## Verification

- **Linux Host Build (`scripts/build-host.sh`)**:
  Builds all 487 targets, links native `libmali_mesa.a`, and runs `tests/smoke.c` verifying:
  - ABI struct size (152 bytes) and packing
  - GenXML v11 pack/unpack
  - `cs_builder` 40-byte instruction sequence
  - All 8 contract CS opcodes/words: `MOVE32`, `MOVE48`, `WAIT`, `CALL`, `ERROR_BARRIER`, `FLUSH_CACHE2`, `SYNC_ADD64`, `STORE_MULTIPLE`
  - Model detection (`Mali-G715` from product ID `0xb8a21020`)
  - Valhall compiler emitting 128-byte shader binary
  **Result: 16/16 assertions PASS, 0 failures**.

- **aarch64-linux Proxy Build & Symbol Audit (`scripts/build-proxy.sh`)**:
  Builds `libmali_mesa.a` for aarch64 using clang cross-compiler.
  Runs `scripts/check_symbols.py`:
  - 43 / 43 required Metal plug-in symbols found.
  - 16 / 16 required shader compiler / NIR symbols found.
  - 0 unknown unresolved symbols.
  - 0 DRM (`drmIoctl`, `pan_kmod_*`) leaks.
  **Result: OVERALL RESULT: OK**.

- **Shell Script Validation**:
  `bash -n` on all 7 `.sh` scripts: **PASS**.
  `ios-build.sh --print-plan`: **PASS**.
