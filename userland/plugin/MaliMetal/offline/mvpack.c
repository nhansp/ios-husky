#include "mvpack.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void block(mvp_sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void mvp_sha256_init(mvp_sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->used = 0;
}

void mvp_sha256_update(mvp_sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, take);
        s->used += (uint32_t)take;
        p += take;
        n -= take;
        if (s->used == 64) {
            block(s, s->buf);
            s->used = 0;
        }
    }
}

void mvp_sha256_final(mvp_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    mvp_sha256_update(s, &pad, 1);
    while (s->used != 56)
        mvp_sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    mvp_sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void put_u32(mvp_sha256 *s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    mvp_sha256_update(s, b, 4);
}

static void put_str(mvp_sha256 *s, const char *str)
{
    size_t n = str ? strlen(str) : 0;
    put_u32(s, (uint32_t)n);
    mvp_sha256_update(s, str, n);
}

void mvp_key(const air2vh_compile_info *info, char key_hex[65])
{
    static const char hex[] = "0123456789abcdef";
    mvp_sha256 s;
    uint8_t d[32];
    mvp_sha256_init(&s);
    mvp_sha256_update(&s, MVP_MAGIC, 4);
    put_u32(&s, info->stage);
    put_u32(&s, info->flags);
    put_str(&s, info->entry);
    put_u32(&s, info->num_constants);
    for (uint32_t i = 0; i < info->num_constants; i++) {
        const air2vh_constant *c = &info->constants[i];
        put_u32(&s, (uint32_t)c->index);
        put_str(&s, c->name);
        put_u32(&s, c->num_values);
        mvp_sha256_update(&s, c->values, sizeof(double) * (c->num_values < 4 ? c->num_values : 4));
    }
    mvp_sha256_update(&s, info->air, info->air_size);
    mvp_sha256_final(&s, d);
    for (int i = 0; i < 32; i++) {
        key_hex[2 * i] = hex[d[i] >> 4];
        key_hex[2 * i + 1] = hex[d[i] & 15];
    }
    key_hex[64] = 0;
}
