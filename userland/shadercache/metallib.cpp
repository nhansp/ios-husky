/* SPDX-License-Identifier: MIT
 * metallib.cpp - see metallib.h. */
#include "metallib.h"

#include <cstdio>
#include <cstring>

namespace {

struct Reader {
    const uint8_t *p;
    uint64_t n;
    bool ok = true;
    bool have(uint64_t off, uint64_t len) const { return off <= n && len <= n - off; }
    uint16_t u16(uint64_t off) { if (!have(off, 2)) { ok = false; return 0; } return (uint16_t)(p[off] | (p[off + 1] << 8)); }
    uint32_t u32(uint64_t off) { if (!have(off, 4)) { ok = false; return 0; } uint32_t v; memcpy(&v, p + off, 4); return v; }
    uint64_t u64(uint64_t off) { if (!have(off, 8)) { ok = false; return 0; } uint64_t v; memcpy(&v, p + off, 8); return v; }
};

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool fail(std::string *e, const char *what, unsigned long long a = 0) {
    if (e) { char b[200]; snprintf(b, sizeof b, "%s (%llu)", what, a); *e = b; }
    return false;
}

}  // namespace

const char *MtlLibrary::type_name(const MtlFunction &f) const {
    switch (f.type) {
    case 0: return "vertex";
    case 1: return "fragment";
    case 2: return "kernel";
    case 3: return "unqualified";
    case 4: return "visible";
    case 5: return "extern";
    case 6: return "intersection";
    case 7: return "mesh";
    case 8: return "object";
    }
    return "?";
}

bool metallib_is_bitcode(const uint8_t *p, uint64_t n) {
    if (n < 4) return false;
    return (p[0] == 0xde && p[1] == 0xc0 && p[2] == 0x17 && p[3] == 0x0b) || (p[0] == 'B' && p[1] == 'C' && p[2] == 0xc0 && p[3] == 0xde);
}

unsigned mtl_dtype_components(uint8_t t, char *kind) {
    /* MTLDataType numbering (Metal): Float 3..6, Half 16..19, Int 29..32, UInt 33..36, Short 37..40,
     * UShort 41..44, Char 45..48, UChar 49..52, Bool 53..56 */
    struct { uint8_t base; char k; } g[] = { {3,'f'}, {16,'f'}, {29,'i'}, {33,'u'}, {37,'i'}, {41,'u'}, {45,'i'}, {49,'u'}, {53,'b'} };
    for (auto &e : g)
        if (t >= e.base && t < e.base + 4) { if (kind) *kind = e.k; return (unsigned)(t - e.base + 1); }
    if (kind) *kind = '?';
    return 0;
}

bool metallib_load(const char *path, MtlLibrary *out, std::string *err) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, "cannot open file");
    std::vector<uint8_t> file;
    uint8_t buf[1 << 16];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + r);
    fclose(f);
    if (file.size() < 16) return fail(err, "file too small", file.size());

    uint64_t sl_off = 0, sl_size = file.size();
    out->fat_cputype = 0;
    out->fat_slices = 0;
    if (be32(file.data()) == 0xCAFEBABEu) {
        uint32_t n = be32(file.data() + 4);
        if (n == 0 || n > 16 || 8 + 20ull * n > file.size()) return fail(err, "bad fat header", n);
        out->fat_slices = n;
        bool found = false;
        for (uint32_t i = 0; i < n && !found; i++) {
            const uint8_t *e = file.data() + 8 + 20 * i;
            uint32_t cpu = be32(e), off = be32(e + 8), size = be32(e + 12);
            if ((uint64_t)off + size > file.size()) return fail(err, "fat slice out of range", i);
            if (size >= 4 && memcmp(file.data() + off, "MTLB", 4) == 0) {
                /* prefer the AIR slice (cputype 0x01000017); take the first MTLB otherwise */
                if (!found || cpu == 0x01000017u) { sl_off = off; sl_size = size; out->fat_cputype = cpu; found = true; if (cpu == 0x01000017u) break; }
            }
        }
        if (!found) return fail(err, "no MTLB slice in the fat container");
    }
    out->blob.assign(file.begin() + sl_off, file.begin() + sl_off + sl_size);
    Reader rd{out->blob.data(), out->blob.size()};
    if (memcmp(out->blob.data(), "MTLB", 4) != 0) return fail(err, "not an MTLB file");
    out->platform = rd.u16(4);
    out->os_major = rd.u16(0x0c);
    out->os_minor = rd.u16(0x0e);
    uint64_t fl_off = rd.u64(0x18), fl_size = rd.u64(0x20);
    uint64_t pub_off = rd.u64(0x28), pub_size = rd.u64(0x30);
    out->bitcode_off = rd.u64(0x48);
    out->bitcode_size = rd.u64(0x50);
    if (!rd.ok) return fail(err, "truncated MTLB header");
    if (!rd.have(fl_off, fl_size) || !rd.have(pub_off, pub_size) || !rd.have(out->bitcode_off, out->bitcode_size))
        return fail(err, "MTLB section out of range");

    uint64_t p = fl_off;
    uint32_t count = rd.u32(p);
    p += 4;
    if (count > 100000) return fail(err, "absurd function count", count);
    out->functions.clear();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t esz = rd.u32(p);
        if (!rd.ok || esz < 8 || !rd.have(p, esz)) return fail(err, "function entry out of range", i);
        uint64_t q = p + 4, end = p + esz;
        MtlFunction fn;
        bool ended = false;
        while (q + 4 <= end && !ended) {
            char tag[5] = {};
            memcpy(tag, out->blob.data() + q, 4);
            q += 4;
            if (!memcmp(tag, "ENDT", 4)) { ended = true; break; }
            uint16_t len = rd.u16(q);
            q += 2;
            if (!rd.have(q, len) || q + len > end) return fail(err, "tag payload out of range", i);
            const uint8_t *v = out->blob.data() + q;
            if (!memcmp(tag, "NAME", 4)) fn.name.assign((const char *)v, strnlen((const char *)v, len));
            else if (!memcmp(tag, "TYPE", 4) && len >= 1) fn.type = v[0];
            else if (!memcmp(tag, "HASH", 4) && len >= 32) memcpy(fn.hash, v, 32);
            else if (!memcmp(tag, "MDSZ", 4) && len >= 8) memcpy(&fn.bc_size, v, 8);
            else if (!memcmp(tag, "OFFT", 4) && len >= 24) { memcpy(&fn.pub_off, v, 8); memcpy(&fn.priv_off, v + 8, 8); memcpy(&fn.bc_off, v + 16, 8); }
            else if (!memcmp(tag, "VERS", 4) && len >= 8) {
                fn.air_major = (uint16_t)(v[0] | v[1] << 8); fn.air_minor = (uint16_t)(v[2] | v[3] << 8);
                fn.lang_major = (uint16_t)(v[4] | v[5] << 8); fn.lang_minor = (uint16_t)(v[6] | v[7] << 8);
            }
            q += len;
        }
        if (!ended) return fail(err, "function entry without ENDT", i);
        if (fn.name.empty()) return fail(err, "function without NAME", i);
        if (fn.bc_size == 0 || fn.bc_off > out->bitcode_size || fn.bc_size > out->bitcode_size - fn.bc_off)
            return fail(err, "function bitcode out of range", i);
        if (!metallib_is_bitcode(out->bitcode(fn), fn.bc_size)) return fail(err, "function payload is not LLVM bitcode", i);

        /* public metadata: u32 size, then tags; harvest CNST */
        uint64_t m = pub_off + fn.pub_off;
        if (rd.have(m, 4)) {
            uint32_t msz = rd.u32(m);
            if (msz >= 4 && rd.have(m, msz)) {
                uint64_t a = m + 4, mend = m + msz;
                while (a + 4 <= mend) {
                    char tg[5] = {};
                    memcpy(tg, out->blob.data() + a, 4);
                    a += 4;
                    if (!memcmp(tg, "ENDT", 4)) break;
                    uint16_t l = rd.u16(a);
                    a += 2;
                    if (a + l > mend) break;
                    if (!memcmp(tg, "CNST", 4) && l >= 2) {
                        const uint8_t *c = out->blob.data() + a;
                        unsigned nc = c[0] | c[1] << 8, o = 2;
                        for (unsigned k = 0; k < nc && o < l; k++) {
                            size_t nl = strnlen((const char *)c + o, l - o);
                            if (o + nl + 1 + 4 > l) break;
                            MtlConstant mc;
                            mc.name.assign((const char *)c + o, nl);
                            o += (unsigned)nl + 1;
                            mc.dtype = c[o];
                            mc.index = (uint16_t)(c[o + 1] | c[o + 2] << 8);
                            mc.flag = c[o + 3];
                            o += 4;
                            fn.constants.push_back(mc);
                        }
                    }
                    a += l;
                }
            }
        }
        out->functions.push_back(fn);
        p = end;
    }
    return true;
}
