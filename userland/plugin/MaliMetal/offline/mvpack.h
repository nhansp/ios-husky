/* MVP1: precompiled air2vh results, shared by the plug-in's offline air2vh shim and the host packer. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "air2vh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MVP_MAGIC        "MVP1"
#define MVP_VERSION      1u
#define MVP_ENTRY_MAX    64u
#define MVP_FILENAME     "shaders.mvpack"

typedef struct mvp_header {
    char     magic[4];
    uint32_t version, count, id_len;
    /* id[id_len], padded to 8, then count x mvp_index sorted by key, then records */
} mvp_header;

typedef struct mvp_index {
    char     key[64];
    char     entry[MVP_ENTRY_MAX];
    uint32_t stage, flags, num_constants, reserved;
    uint64_t offset, length;
} mvp_index;

typedef struct mvp_record {
    uint32_t program_size, code_size, sidecar_size, layout_size;
    /* air2vh_program (pointers zeroed) | code | sidecar JSON | VS layout blob */
} mvp_record;

void mvp_key(const air2vh_compile_info *info, char key_hex[65]);

typedef struct mvp_sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t used;
} mvp_sha256;
void mvp_sha256_init(mvp_sha256 *s);
void mvp_sha256_update(mvp_sha256 *s, const void *data, size_t n);
void mvp_sha256_final(mvp_sha256 *s, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
