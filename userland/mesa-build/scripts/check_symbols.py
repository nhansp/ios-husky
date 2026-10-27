#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exported-symbol check and undefined-symbol audit of libmali_mesa.a.

Two checks:
1. Exported symbols check:
   Verifies that ALL symbols needed by the Metal plug-in (MaliMetal) and the shader compiler
   (air2valhall) are present and defined in libmali_mesa.a.
2. Undefined symbols audit:
   - Proves no DRM (drmIoctl, pan_kmod_*, etc.) leaked into the archive.
   - In darwin mode, verifies undefined symbols resolve against Darwin SDK / libc surface.
   - In linux-proxy mode, verifies 0 unknown symbols (only expected glibc / Linux-only symbols).

Usage:
  check_symbols.py ARCHIVE [--mode linux-proxy|darwin] [--nm llvm-nm] [--sdk SDK_PATH] [--show-allowed]
"""
import argparse
import os
import re
import subprocess
import sys

# Symbols required by the MaliMetal plug-in core
REQUIRED_PLUGIN_SYMBOLS = [
    "pan_align_fb_tiling_area_v11",
    "pan_blend_can_fixed_function",
    "pan_blend_constant_mask",
    "pan_blend_is_homogenous_constant",
    "pan_blend_is_opaque",
    "pan_blend_optimize_equation",
    "pan_blend_reads_dest",
    "pan_blend_to_fixed_function_equation",
    "pan_blendable_formats_v11",
    "pan_compute_max_thread_count",
    "pan_emit_fb_desc_v11",
    "pan_emit_tls_v11",
    "pan_fb_needs_zs_crc_ext_v11",
    "pan_get_model",
    "pan_get_total_stack_size",
    "pan_image_layout_init",
    "pan_mod_get_handler_v11",
    "pan_pack_blend_constant",
    "pan_pipe_format_v11",
    "pan_query_core_count",
    "pan_query_core_id_range",
    "pan_query_thread_tls_alloc",
    "pan_query_tib_size",
    "pan_query_tiler_features",
    "pan_query_z_tib_size",
    "pan_sample_positions_buffer_size",
    "pan_sample_positions_offset",
    "pan_sampled_texture_emit_v11",
    "pan_select_fb_tile_size_v11",
    "pan_select_tiler_hierarchy_mask_v11",
    "pan_storage_texture_emit_v11",
    "pan_texture_estimate_payload_size_v11",
    "pan_texture_get_payload_alignment_v11",
    "pan_upload_sample_positions",
    "ralloc_free",
    "ralloc_size",
    "ralloc_context",
    "util_dynarray_grow_bytes_slow",
    "util_dynarray_is_data_stack_allocated",
    "util_format_description",
    "util_format_is_float",
    "util_format_is_pure_sint",
    "util_format_is_pure_uint",
]

# Symbols required by air2valhall compiler & NIR
REQUIRED_COMPILER_SYMBOLS = [
    "pan_get_nir_shader_compiler_options",
    "pan_preprocess_nir",
    "pan_postprocess_nir",
    "pan_shader_compile",
    "pan_varying_collect_formats",
    "pan_build_varying_layout_compact",
    "pan_nir_lower_fs_outputs",
    "pan_nir_lower_fs_inputs",
    "pan_nir_lower_noperspective_vs",
    "pan_get_compiler_flags",
    "glsl_type_singleton_init_or_ref",
    "glsl_type_singleton_decref",
    "glsl_array_type",
    "glsl_simple_explicit_type",
    "glsl_type_builtin_uint",
    "glsl_type_builtin_int",
    "glsl_type_builtin_float",
    "glsl_type_builtin_vec4",
    "glsl_vec_type",
    "glsl_uvec_type",
    "glsl_ivec_type",
    "glsl_without_array",
    "nir_shader_create",
    "nir_variable_create",
    "nir_local_variable_create",
    "nir_builder_init_simple_shader",
    "nir_lower_compute_system_values",
    "nir_assign_io_var_locations",
    "nir_lower_io",
    "nir_lower_io_vars_to_temporaries",
    "util_compute_fast_udiv_info",
]

# Base allowlist for Darwin libc / libSystem symbols
ALLOWED_EXACT = set("""
abort accept access asprintf atexit bcmp bind bzero calloc clock_gettime clock_nanosleep
clock_gettime_nsec_np close cos cosf dladdr dlclose dlerror dlopen dlsym dup2 exit exp2f
fclose fcntl fdopen fegetexcept feholdexcept fesetenv fflush fgets fileno flock fprintf
fputc fputs fread free freelocale frexp frexpf fseek ftell ftruncate fwrite getegid getenv
geteuid getgid getpid getuid getprogname host_statistics64 ldexp ldexpf listen llround
llroundf log10 log2 log2f lround lroundf malloc memchr memcmp memcpy memmove memset mkdir
mmap munmap nanosleep newlocale open open_memstream openlog os_log poll posix_memalign pow
powf printf putc putchar puts qsort rand read readlink realloc realpath recv rewind send
setenv setpriority sigdelset sigfillset sin sinf snprintf socket stat fstat strcasecmp
strcat strchr strcmp strcpy strcspn strdup strerror strlen strncasecmp strncat strncmp
strncpy strndup strnlen strpbrk strrchr strstr strtod_l strtof_l strtok_r strlcpy sysconf
syslog tanh tanhf time timespec_get unlink unsetenv usleep vasprintf vfprintf vsnprintf
lseek sched_yield pthread_self stderr stdout stdin sscanf strtol strtoll strtoul strtoull
strtod strtof mkostemp mkstemps fopen getpwuid_r isatty clock write madvise mprotect
fstatat dirfd closedir opendir readdir getpagesize vm_page_size abs fabs fabsf sqrt sqrtf
floor floorf ceil ceilf round roundf trunc truncf fmod fmodf exp expf log logf exp2
nearbyint nearbyintf rint rintf cbrt cbrtf atan atan2 atan2f asin asinf acos acosf sinh
sinhf cosh coshf fma fmaf memset_s qsort_s getrusage gettimeofday qsort_r ___chkstk_darwin
__chkstk_darwin
""".split())

ALLOWED_PREFIX = (
    "pthread_", "mach_", "os_unfair_lock", "sysctl", "_NSGetExecutablePath", "dispatch_",
    "__aarch64_", "__udiv", "__div", "__mod", "_Unwind_", "_ZNSt3__1", "_ZNKSt3__1",
    "_Znw", "_Zna", "_Zdl", "_Zda", "__cxa_", "___cxa_", "__gxx_personality",
    "__dso_handle", "_GLOBAL_OFFSET_TABLE_", "dyld_", "__stack_chk", "__stderrp", "__stdoutp",
    "__sincosf_stret", "__tlv_bootstrap", "os_log", "___chkstk", "__chkstk", "host_", "vm_",
)

GLIBC_RX = re.compile(
    r"^(__.*_chk|__isoc23_.*|.*64|__errno_location|__ctype_.*_loc|_ZSt.*|_ZNSt[^3].*|"
    r"_ZNKSt[^3].*|__sched_cpucount|__libc_.*|__tls_get_addr|__assert_fail)$"
)

LINUX_ONLY_RX = re.compile(
    r"^(drm\w*|memfd_create|getrandom|eventfd\w*|inotify_\w+|epoll_\w+|dl_iterate_phdr|"
    r"sched_getaffinity|sched_setaffinity|sched_getcpu|program_invocation_name|"
    r"program_invocation_short_name|secure_getenv|syscall|"
    r"pthread_(barrier\w*|mutex_timedlock|condattr_setclock|getaffinity_np|setaffinity_np|"
    r"getcpuclockid|setschedparam_np)|mallinfo2?|random_r|posix_fallocate|issignaling|"
    r"__sched_cpucount|ioctl|xf86\w+|udev_\w+|gz\w+|crc32|inflate\w*|deflate\w*|ZSTD_\w+)$"
)


def load_tbd_symbols(tbd_path, sdk=None, visited=None):
    """Extract exported and weak symbols from an Apple .tbd stub file and its sub-umbrella re-exports."""
    if visited is None:
        visited = set()
    tbd_path = os.path.realpath(tbd_path)
    if tbd_path in visited or not os.path.isfile(tbd_path):
        return set()
    visited.add(tbd_path)

    symbols = set()
    try:
        with open(tbd_path, "r", errors="ignore") as f:
            content = f.read()
    except Exception:
        return symbols

    # 1. Match symbols: [ ... ] and weak-symbols: [ ... ] blocks
    for m in re.finditer(r"(?:weak-)?symbols:\s*\[([^\]]+)\]", content):
        items = m.group(1).split(",")
        for item in items:
            s = item.strip().strip("'\"")
            if s.startswith("_"):
                symbols.add(s[1:])
            elif s:
                symbols.add(s)

    # 2. Check for re-exported libraries / sub-umbrellas
    reexported = []
    for m in re.finditer(r"(?:re-exports|reexported-libraries):\s*\[([^\]]+)\]", content):
        items = m.group(1).split(",")
        for item in items:
            p = item.strip().strip("'\"")
            if p:
                reexported.append(p)

    for m in re.finditer(r"libraries:\s*\[([^\]]+)\]", content):
        items = m.group(1).split(",")
        for item in items:
            p = item.strip().strip("'\"")
            if p:
                reexported.append(p)

    # If this is libSystem.B.tbd, also scan usr/lib/system/*.tbd directly
    parent_dir = os.path.dirname(tbd_path)
    system_dir = os.path.join(parent_dir, "system")
    if os.path.isdir(system_dir):
        for f in os.listdir(system_dir):
            if f.endswith(".tbd"):
                reexported.append(os.path.join(system_dir, f))

    # If this is libc++.tbd, also scan libc++.1.tbd and libc++abi.tbd in the same directory
    if os.path.basename(tbd_path) == "libc++.tbd":
        for sub in ["libc++.1.tbd", "libc++abi.tbd"]:
            sub_path = os.path.join(parent_dir, sub)
            if os.path.isfile(sub_path):
                reexported.append(sub_path)

    # Resolve and recursively load re-exports
    for rpath in reexported:
        cand_paths = []
        if os.path.isabs(rpath) and os.path.isfile(rpath):
            cand_paths.append(rpath)
        if sdk:
            rel = rpath.lstrip("/")
            cand_paths.append(os.path.join(sdk, rel))
            if rel.endswith(".dylib"):
                cand_paths.append(os.path.join(sdk, rel[:-6] + ".tbd"))
        cand_paths.append(os.path.join(parent_dir, os.path.basename(rpath)))
        if rpath.endswith(".dylib"):
            cand_paths.append(os.path.join(parent_dir, os.path.basename(rpath)[:-6] + ".tbd"))

        for cand in cand_paths:
            if os.path.isfile(cand) and os.path.realpath(cand) not in visited:
                symbols |= load_tbd_symbols(cand, sdk=sdk, visited=visited)
                break

    return symbols


def nm(path, tool, flags):
    cmd = [tool] + flags + [path]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
        return out.splitlines()
    except subprocess.CalledProcessError as e:
        sys.exit(f"Error running {' '.join(cmd)}: {e.stderr}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive", help="Path to static archive (.a)")
    ap.add_argument("--mode", choices=["linux-proxy", "darwin"], default="linux-proxy")
    ap.add_argument("--nm", default="llvm-nm", help="nm tool (default: llvm-nm)")
    ap.add_argument("--sdk", default="", help="Optional iPhoneOS SDK path to check .tbd exports")
    ap.add_argument("--show-allowed", action="store_true")
    a = ap.parse_args()

    # If xcrun is available on macOS and tool is llvm-nm, resolve it
    tool = a.nm
    if tool == "llvm-nm" and os.path.exists("/usr/bin/xcrun"):
        try:
            res = subprocess.run(["xcrun", "-f", "llvm-nm"], capture_output=True, text=True)
            if res.returncode == 0 and res.stdout.strip():
                tool = res.stdout.strip()
        except Exception:
            pass

    defined, undefined = set(), set()
    for line in nm(a.archive, tool, ["-g", "--defined-only"]):
        p = line.split()
        if len(p) >= 3:
            defined.add(p[-1])
    for line in nm(a.archive, tool, ["-u"]):
        p = line.split()
        if len(p) >= 2 and not line.endswith(":"):
            undefined.add(p[-1])

    strip = (lambda s: s[1:] if s.startswith("_") and a.mode == "darwin" else s)
    d = {strip(s) for s in defined} | defined
    unres = sorted(s for s in undefined if s not in d and strip(s) not in d)

    # -------------------------------------------------------------------------
    # 1. EXPORTED SYMBOLS CHECK (plug-in and compiler requirements)
    # -------------------------------------------------------------------------
    print("=== EXPORTED SYMBOLS VERIFICATION ===")
    missing_plugin = [s for s in REQUIRED_PLUGIN_SYMBOLS if s not in d]
    missing_compiler = [s for s in REQUIRED_COMPILER_SYMBOLS if s not in d]

    print(f"Required plug-in symbols : {len(REQUIRED_PLUGIN_SYMBOLS)} total, {len(REQUIRED_PLUGIN_SYMBOLS) - len(missing_plugin)} found")
    if missing_plugin:
        print("  FAIL: Missing plug-in symbols:")
        for s in missing_plugin:
            print(f"    - {s}")

    print(f"Required compiler symbols: {len(REQUIRED_COMPILER_SYMBOLS)} total, {len(REQUIRED_COMPILER_SYMBOLS) - len(missing_compiler)} found")
    if missing_compiler:
        print("  FAIL: Missing compiler symbols:")
        for s in missing_compiler:
            print(f"    - {s}")

    export_failed = bool(missing_plugin or missing_compiler)

    # -------------------------------------------------------------------------
    # 2. UNDEFINED SYMBOLS AUDIT
    # -------------------------------------------------------------------------
    print("\n=== UNDEFINED SYMBOLS AUDIT ===")
    allowed_set = set(ALLOWED_EXACT)
    if a.sdk:
        sys_tbd = os.path.join(a.sdk, "usr/lib/libSystem.B.tbd")
        cxx_tbd = os.path.join(a.sdk, "usr/lib/libc++.tbd")
        if os.path.isfile(sys_tbd):
            tbd_syms = load_tbd_symbols(sys_tbd, sdk=a.sdk)
            allowed_set |= tbd_syms
            print(f"Loaded {len(tbd_syms)} symbols from {sys_tbd} (including sub-umbrellas)")
        if os.path.isfile(cxx_tbd):
            tbd_syms = load_tbd_symbols(cxx_tbd, sdk=a.sdk)
            allowed_set |= tbd_syms
            print(f"Loaded {len(tbd_syms)} symbols from {cxx_tbd} (including sub-umbrellas)")

    cls = {"allowed": [], "glibc": [], "linuxonly": [], "unknown": []}
    for raw in unres:
        s = strip(raw)
        if LINUX_ONLY_RX.match(s):
            cls["linuxonly"].append(raw)
        elif s in allowed_set or s.startswith(ALLOWED_PREFIX):
            cls["allowed"].append(raw)
        elif GLIBC_RX.match(s):
            cls["glibc"].append(raw)
        else:
            cls["unknown"].append(raw)

    print(f"archive: {a.archive}\nmode: {a.mode}\ndefined: {len(defined)}  undefined(unresolved): {len(unres)}")
    for k in ("allowed", "glibc", "linuxonly", "unknown"):
        v = cls[k]
        if k == "allowed" and not a.show_allowed:
            print(f"{k:9s}: {len(v)}")
            continue
        print(f"{k:9s}: {len(v)}" + (("  " + " ".join(v)) if v else ""))

    audit_failed = bool(cls["unknown"])
    if a.mode == "darwin":
        audit_failed = audit_failed or bool(cls["linuxonly"]) or bool(cls["glibc"])

    overall_failed = export_failed or audit_failed
    print("\nOVERALL RESULT:", "FAIL" if overall_failed else "OK")
    return 1 if overall_failed else 0


if __name__ == "__main__":
    sys.exit(main())
