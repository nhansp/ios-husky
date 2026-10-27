#!/usr/bin/env python3
"""Turn a native Mesa meson build (compile_commands.json) into the CMake source manifest.

Stdlib only. ORIGINAL tooling; it reads Mesa's (MIT) compile graph and rewrites it as data,
it does not copy Mesa source.

  make_manifest.py --print-ninja-targets
  make_manifest.py --print-generated-sources BUILD_DIR
  make_manifest.py --mesa-src SRC --build-dir BUILD --out-cmake cmake/mesa_sources.cmake \
                   --out-lists sources/

For every library in LIBS it records, from the real compile commands of the host build:
  * the translation units (split into "in Mesa tree" vs "generated, lives in the staged gen tree"),
  * the include directories in meson's own order (matters: bifrost/compiler.h vs midgard/compiler.h
    vs panfrost/compiler/ share basenames, so a global include list is wrong),
  * the per-library -D flags that are not part of Mesa's global pre_args (PAN_ARCH, XXH_*, ...).
Global -D flags are NOT copied: for an iOS target they come from cmake/mesa_config.cmake, which probes
the target SDK instead of trusting what the Linux host found (HAVE_LIBDRM, HAVE_MEMFD_CREATE, ...).
"""
import argparse
import collections
import json
import os
import re
import shlex
import sys

# (meson target path, cmake target, group)
# group: util | nir | pancomp | panlib | pandecode
# The set is the closure of the arch-11 pieces of spec 06 s4 (P1 genxml packers, P3 model/props, P4
# descriptors, P5 texture/format, P6 blend, P7 Valhall compiler, P9 NIR+util), exactly as meson
# links them for PAN_ARCH=11, minus the per-arch libraries for other generations.
LIBS = [
    ("src/c11/impl/libmesa_util_c11.a", "mm_util_c11", "util"),
    ("src/util/blake3/libblake3.a", "mm_blake3", "util"),
    ("src/util/libmesa_util_simd.a", "mm_util_simd", "util"),
    ("src/util/libmesa_util.a", "mm_util", "util"),
    ("src/compiler/libcompiler.a", "mm_compiler", "util"),
    ("src/compiler/isaspec/libisaspec.a", "mm_isaspec", "util"),
    ("src/compiler/nir/libnir.a", "mm_nir", "nir"),
    ("src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a", "mm_pan_valhall_disasm", "pancomp"),
    ("src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a", "mm_pan_bifrost_disasm", "pancomp"),
    ("src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a", "mm_pan_midgard_disasm", "pancomp"),
    ("src/panfrost/compiler/bifrost/libpanfrost_bifrost.a", "mm_pan_bifrost", "pancomp"),
    ("src/panfrost/compiler/midgard/libpanfrost_midgard.a", "mm_pan_midgard", "pancomp"),
    ("src/panfrost/compiler/libpanfrost_compiler.a", "mm_pan_compiler", "pancomp"),
    ("src/panfrost/shared/libpanfrost_shared.a", "mm_pan_shared", "panlib"),
    ("src/panfrost/model/libpanfrost_model_lib.a", "mm_pan_model", "panlib"),
    ("src/panfrost/lib/libpan-format-v11.a", "mm_pan_format_v11", "panlib"),
    ("src/panfrost/lib/libpan-arch-v11.a", "mm_pan_arch_v11", "panlib"),
    ("src/panfrost/lib/libpanfrost_lib.a", "mm_pan_lib", "panlib"),
    ("src/panfrost/genxml/libpandecode-arch-v11.a", "mm_pandecode_v11", "pandecode"),
    ("src/panfrost/genxml/libpanfrost_decode.a", "mm_pandecode", "pandecode"),
]

# Translation units dropped on purpose (reason in the comment). Matching is on the path relative
# to the Mesa tree / gen tree.
DROP = [
    # needs libdrm syncobj ioctls (Linux DRM); the iOS backend has no DRM, spec 06 s8 item 4
    (re.compile(r"(^|/)u_sync_provider\.c$"), "DRM syncobj (drmSyncobj*)"),
    # x86 assembly / x86 cache ops: replaced per-arch in CMakeLists.txt
    (re.compile(r"blake3_(sse2|sse41|avx2|avx512)_x86-64_(unix|windows_gnu)\.S$"), "x86 asm, CMake picks blake3_neon.c on arm64"),
    (re.compile(r"cache_ops_x86"), "x86 clflush libs not built on arm64"),
]
# libs that exist only for x86 and are never built for arm64
X86_ONLY_TARGETS = {"src/util/libmesa_util_clflush.a", "src/util/libmesa_util_clflushopt.a"}


def load_cc(build):
    with open(os.path.join(build, "compile_commands.json")) as f:
        return json.load(f)


def lib_of(output):
    m = re.match(r"(.*/)?(lib[^/]*\.a)\.p/", output)
    return ((m.group(1) or "") + m.group(2)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--print-ninja-targets", action="store_true")
    ap.add_argument("--print-generated-sources", metavar="BUILD")
    ap.add_argument("--mesa-src")
    ap.add_argument("--build-dir")
    ap.add_argument("--out-cmake")
    ap.add_argument("--out-lists")
    a = ap.parse_args()

    if a.print_ninja_targets:
        print(" ".join(t for t, _, _ in LIBS))
        return
    if a.print_generated_sources:
        bld = os.path.realpath(a.print_generated_sources)
        for e in load_cc(bld):
            p = os.path.realpath(os.path.join(e["directory"], e["file"]))
            if p.startswith(bld + os.sep):
                print(p)
        return

    src = os.path.realpath(a.mesa_src)
    bld = os.path.realpath(a.build_dir)
    cc = load_cc(bld)
    by = collections.defaultdict(list)
    for e in cc:
        k = lib_of(e["output"])
        if k:
            by[k].append(e)

    glob_defs = None
    per = {}
    for meson_path, cname, group in LIBS:
        ents = by.get(meson_path)
        if not ents:
            sys.exit(f"{meson_path}: no compile commands (did host-gen.sh build it?)")
        srcs, gens, incs_s, incs_g, defs = [], [], [], [], set()
        langs = set()
        for e in ents:
            fpath = os.path.realpath(os.path.join(e["directory"], e["file"]))
            if fpath.startswith(bld + os.sep):
                rel, kind = os.path.relpath(fpath, bld), "gen"
            else:
                rel, kind = os.path.relpath(fpath, src), "src"
            if any(rx.search(rel) for rx, _ in DROP):
                continue
            (gens if kind == "gen" else srcs).append(rel)
            langs.add(os.path.splitext(rel)[1])
            toks = shlex.split(e["command"])
            i = 1
            while i < len(toks):
                t = toks[i]
                inc = None
                if t in ("-I", "-isystem"):
                    inc, i = toks[i + 1], i + 1
                elif t.startswith("-I"):
                    inc = t[2:]
                elif t.startswith("-D"):
                    defs.add(t)
                if inc:
                    p = os.path.realpath(os.path.join(e["directory"], inc))
                    if "libdrm" in p:      # host libdrm headers: never on the iOS include path
                        i += 1
                        continue
                    if p.startswith(bld + os.sep) or p == bld:
                        r = os.path.relpath(p, bld)
                        if r not in incs_g:
                            incs_g.append(r)
                    elif p.startswith(src + os.sep) or p == src:
                        r = os.path.relpath(p, src)
                        if r not in incs_s:
                            incs_s.append(r)
                    # *.p private dirs (generated config) are build-relative and kept above
                i += 1
        per[cname] = dict(meson=meson_path, group=group, src=sorted(set(srcs)), gen=sorted(set(gens)),
                          inc_s=incs_s, inc_g=incs_g, defs=defs, langs=langs)
        glob_defs = defs if glob_defs is None else (glob_defs & defs)

    rev = "unknown"
    try:
        rev = open(os.path.join(src, "VERSION")).read().strip()
    except OSError:
        pass

    out = []
    w = out.append
    w("# GENERATED by scripts/make_manifest.py from a native Mesa meson build. Do not edit.")
    w(f"# Mesa version: {rev}   (stage the matching tree: scripts/host-gen.sh writes MESA_REV.txt)")
    w("# Paths: *_SRC are relative to ${MESA_SRC}; *_GEN relative to ${MESA_GEN}.")
    w("# Include dirs keep meson's order; *_INC_SRC/_INC_GEN likewise.")
    w("")
    w(f'set(MM_MESA_VERSION "{rev}")')
    w("set(MM_LIBS " + " ".join(c for _, c, _ in LIBS) + ")")
    w("")
    for meson_path, cname, group in LIBS:
        d = per[cname]
        extra = sorted(x for x in d["defs"] if x not in glob_defs)
        # drop defs that mesa_config.cmake owns
        extra = [x for x in extra if not re.match(
            r"-D(HAVE_|MESA_|VIDEO_|USE_|ALLOW_|GLAPI|DRAW_|AMD_|GALLIVM|HAS_|MAJOR_IN|THREAD_SAN)", x)]
        w(f"# {meson_path}  [{d['group']}]  {len(d['src']) + len(d['gen'])} TUs")
        w(f"set({cname}_GROUP {d['group']})")
        w(f"set({cname}_HAS_CXX {'ON' if '.cpp' in d['langs'] else 'OFF'})")
        w(f"set({cname}_DEFS {' '.join(extra)})".rstrip() if extra else f"set({cname}_DEFS)")
        for tag, items in (("SRC", d["src"]), ("GEN", d["gen"])):
            w(f"set({cname}_{tag}")
            for it in items:
                w(f"  {it}")
            w(")")
        for tag, items in (("INC_SRC", d["inc_s"]), ("INC_GEN", d["inc_g"])):
            w(f"set({cname}_{tag}")
            for it in items:
                w(f"  {it}")
            w(")")
        w("")
    if a.out_cmake:
        os.makedirs(os.path.dirname(os.path.abspath(a.out_cmake)), exist_ok=True)
        with open(a.out_cmake, "w") as f:
            f.write("\n".join(out) + "\n")

    if a.out_lists:
        os.makedirs(a.out_lists, exist_ok=True)
        with open(os.path.join(a.out_lists, "panfrost-subset.txt"), "w") as f:
            f.write(f"# Mesa {rev}: translation units of the arch-11 library set (one per line)\n")
            f.write("# S = in the Mesa tree, G = generated by host-gen.sh (staged gen tree)\n")
            total = 0
            for meson_path, cname, group in LIBS:
                d = per[cname]
                f.write(f"\n## {cname}  <-  {meson_path}  [{group}]\n")
                for it in d["src"]:
                    f.write(f"S {it}\n")
                for it in d["gen"]:
                    f.write(f"G {it}\n")
                total += len(d["src"]) + len(d["gen"])
            f.write(f"\n# total {total} translation units in {len(LIBS)} libraries\n")
        print(f"manifest: {sum(len(per[c]['src']) + len(per[c]['gen']) for c in per)} TUs, {len(LIBS)} libs")


if __name__ == "__main__":
    main()
