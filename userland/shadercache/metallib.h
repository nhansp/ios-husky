/*
 * metallib.h - minimal reader for Apple .metallib files (fat container + MTLB slice).
 * -------------------------------------------------------------------------
 * Intended repo path: tests/host/metallib.h   (C++17; no Apple code, no LLVM)
 */
#ifndef METALLIB_H
#define METALLIB_H

#include <cstdint>
#include <string>
#include <vector>

struct MtlConstant {
    std::string name;
    uint8_t dtype = 0;     /* MTLDataType */
    uint16_t index = 0;
    uint8_t flag = 0;
};

struct MtlFunction {
    std::string name;
    uint8_t type = 0xff;   /* 0 vertex, 1 fragment, 2 kernel */
    uint8_t hash[32] = {};
    uint64_t pub_off = 0, priv_off = 0, bc_off = 0, bc_size = 0;
    uint16_t air_major = 0, air_minor = 0, lang_major = 0, lang_minor = 0;
    std::vector<MtlConstant> constants;
};

struct MtlLibrary {
    std::vector<uint8_t> blob;          /* the AIR slice only */
    uint32_t fat_cputype = 0;           /* 0 if the file was not fat */
    uint16_t platform = 0, os_major = 0, os_minor = 0;
    uint64_t bitcode_off = 0, bitcode_size = 0;
    std::vector<MtlFunction> functions;
    unsigned fat_slices = 0;

    const uint8_t *bitcode(const MtlFunction &f) const { return blob.data() + bitcode_off + f.bc_off; }
    const char *type_name(const MtlFunction &f) const;
};

/* Load and validate a metallib. Returns false and fills *err on any structural problem. */
bool metallib_load(const char *path, MtlLibrary *out, std::string *err);

/* LLVM bitcode magic check: wrapper 0x0B17C0DE or raw 'BC' 0xC0DE. */
bool metallib_is_bitcode(const uint8_t *p, uint64_t n);

/* Number of scalar components of an MTLDataType, 0 if unknown; *kind: 'b' bool, 'i' signed, 'u' unsigned, 'f' float. */
unsigned mtl_dtype_components(uint8_t dtype, char *kind);

#endif /* METALLIB_H */
