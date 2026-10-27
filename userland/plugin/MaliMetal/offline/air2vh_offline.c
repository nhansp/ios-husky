/*
 * air2vh API served from a precompiled MVP1 pack instead of compiling on the device. A key miss dumps
 * the request's AIR to MVP_MISS_DIR for the host packer (userland/shadercache).
 */
#include "mvpack.h"
#include "air2vh_ext.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MVP_MISS_DIR "/private/var/tmp/malimetal"

struct air2vh_compiler {
    int unused;
};

static struct {
    pthread_once_t once;
    const uint8_t *base;
    size_t size;
    const mvp_index *index;
    uint32_t count;
    char id[256];
} g_pack = { .once = PTHREAD_ONCE_INIT };

static void mvp_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    fprintf(stderr, "%s", buf);
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) {
        (void)write(fd, buf, (size_t)n);
        close(fd);
    }
}

static void pack_load(void)
{
    Dl_info di;
    char path[1024];
    if (!dladdr((const void *)&pack_load, &di) || !di.dli_fname)
        return;
    const char *slash = strrchr(di.dli_fname, '/');
    int dir_len = slash ? (int)(slash - di.dli_fname) : 0;
    snprintf(path, sizeof(path), "%.*s/%s", dir_len, di.dli_fname, MVP_FILENAME);

    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(mvp_header)) {
        mvp_log("MaliMetal: no shader pack at %s\n", path);
        if (fd >= 0)
            close(fd);
        return;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return;

    const mvp_header *h = m;
    size_t idx_off = (sizeof(*h) + h->id_len + 7) & ~(size_t)7;
    if (memcmp(h->magic, MVP_MAGIC, 4) || h->version != MVP_VERSION || h->id_len >= sizeof(g_pack.id) ||
        idx_off > (size_t)st.st_size ||
        (size_t)h->count > ((size_t)st.st_size - idx_off) / sizeof(mvp_index)) {
        mvp_log("MaliMetal: %s is not an MVP1 pack\n", path);
        munmap(m, (size_t)st.st_size);
        return;
    }
    memcpy(g_pack.id, (const char *)m + sizeof(*h), h->id_len);
    g_pack.base = m;
    g_pack.size = (size_t)st.st_size;
    g_pack.index = (const mvp_index *)((const uint8_t *)m + idx_off);
    g_pack.count = h->count;
    mvp_log("MaliMetal: shader pack %s: %u entries (%s)\n", path, h->count, g_pack.id);
}

static const mvp_index *lookup_key(const char key[65])
{
    uint32_t lo = 0, hi = g_pack.count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = memcmp(g_pack.index[mid].key, key, 64);
        if (c == 0)
            return &g_pack.index[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* Metal's compiler service may hand over re-serialized AIR; a unique unspecialized name still matches. */
static const mvp_index *lookup_name(const air2vh_compile_info *info)
{
    const mvp_index *found = NULL;
    if (!info->entry || info->num_constants)
        return NULL;
    for (uint32_t i = 0; i < g_pack.count; i++) {
        const mvp_index *e = &g_pack.index[i];
        if (e->num_constants || e->flags != info->flags || strncmp(e->entry, info->entry, MVP_ENTRY_MAX))
            continue;
        if (info->stage != AIR2VH_STAGE_ANY && e->stage != info->stage)
            continue;
        if (found)
            return NULL;
        found = e;
    }
    return found;
}

static void dump_miss(const air2vh_compile_info *info, const char key[65])
{
    char path[256];
    mkdir(MVP_MISS_DIR, 0777);
    snprintf(path, sizeof(path), MVP_MISS_DIR "/%s.air", key);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        (void)write(fd, info->air, info->air_size);
        close(fd);
    }
    snprintf(path, sizeof(path), MVP_MISS_DIR "/%s.txt", key);
    FILE *f = fopen(path, "wx");
    if (f) {
        fprintf(f, "entry=%s\nstage=%u\nflags=%u\nconstants=%u\n", info->entry ? info->entry : "",
                info->stage, info->flags, info->num_constants);
        for (uint32_t i = 0; i < info->num_constants; i++) {
            const air2vh_constant *c = &info->constants[i];
            fprintf(f, "constant=%d,%s,%u", c->index, c->name ? c->name : "", c->num_values);
            for (uint32_t k = 0; k < c->num_values && k < 4; k++)
                fprintf(f, ",%.17g", c->values[k]);
            fprintf(f, "\n");
        }
        fclose(f);
    }
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static int fill_result(const mvp_index *e, air2vh_result *r)
{
    if (e->offset > g_pack.size || e->length > g_pack.size - e->offset || e->length < sizeof(mvp_record))
        return -1;
    const uint8_t *p = g_pack.base + e->offset;
    const mvp_record *rec = (const mvp_record *)p;
    uint64_t need = (uint64_t)sizeof(*rec) + rec->program_size + rec->code_size + rec->sidecar_size + rec->layout_size;
    if (need > e->length || (rec->program_size != sizeof(air2vh_program) &&
                             rec->program_size != sizeof(air2vh_program) + sizeof(air2vh_program_ext)))
        return -1;
    p += sizeof(*rec);

    air2vh_program *prog = malloc(rec->program_size);
    void *code = malloc(rec->code_size ? rec->code_size : 1);
    char *sidecar = malloc((size_t)rec->sidecar_size + 1);
    void *layout = rec->layout_size ? malloc(rec->layout_size) : NULL;
    if (!prog || !code || !sidecar || (rec->layout_size && !layout)) {
        free(prog); free(code); free(sidecar); free(layout);
        return -1;
    }
    memcpy(prog, p, rec->program_size);
    p += rec->program_size;
    memcpy(code, p, rec->code_size);
    p += rec->code_size;
    memcpy(sidecar, p, rec->sidecar_size);
    sidecar[rec->sidecar_size] = 0;
    p += rec->sidecar_size;
    if (layout)
        memcpy(layout, p, rec->layout_size);

    prog->vs.layout.blob = layout;
    prog->vs.layout.blob_size = rec->layout_size;
    r->code = code;
    r->code_size = rec->code_size;
    r->sidecar_json = sidecar;
    r->sidecar_size = rec->sidecar_size;
    r->program = prog;
    r->vs_layout_out = prog->vs.layout;
    memcpy(r->key_hex, e->key, 64);
    r->key_hex[64] = 0;
    return 0;
}

uint32_t air2vh_api_version(void) { return AIR2VH_API_VERSION; }
uint32_t air2vh_abi_version(void) { return AIR2VH_ABI_VERSION; }
uint32_t air2vh_cache_format_version(void) { return MVP_VERSION; }

const char *air2vh_compiler_id(void)
{
    pthread_once(&g_pack.once, pack_load);
    return g_pack.id[0] ? g_pack.id : "malimetal-offline (no pack)";
}

air2vh_compiler *air2vh_compiler_create(uint64_t gpu_id, uint32_t core_count, uint64_t gpu_features, char **error)
{
    (void)gpu_id; (void)core_count; (void)gpu_features;
    pthread_once(&g_pack.once, pack_load);
    air2vh_compiler *c = calloc(1, sizeof(*c));
    if (!c && error)
        *error = dup_str("out of memory");
    return c;
}

void air2vh_compiler_destroy(air2vh_compiler *c) { free(c); }

int air2vh_cache_key(air2vh_compiler *c, const air2vh_compile_info *info, char key_hex[65], char **error)
{
    (void)c; (void)error;
    mvp_key(info, key_hex);
    return 0;
}

int air2vh_compile(air2vh_compiler *c, const air2vh_compile_info *info, air2vh_result *r)
{
    char key[65], msg[256];
    (void)c;
    memset(r, 0, sizeof(*r));
    mvp_key(info, key);
    const mvp_index *e = lookup_key(key);
    if (!e && (e = lookup_name(info)))
        mvp_log("MaliMetal: %s: AIR differs from the pack, using the entry compiled from the library\n",
                info->entry);
    if (e && fill_result(e, r) == 0)
        return 0;

    dump_miss(info, key);
    snprintf(msg, sizeof(msg), "UNSUPPORTED: no offline binary for %s (key %s, AIR in " MVP_MISS_DIR ")",
             info->entry ? info->entry : "(first entry)", key);
    mvp_log("MaliMetal: %s\n", msg);
    memcpy(r->key_hex, key, sizeof(key));
    r->error = dup_str(msg);
    return -1;
}

const air2vh_program_ext *air2vh_program_get_ext(const air2vh_program *p)
{
    if (!p || p->struct_size != sizeof(air2vh_program))
        return NULL;
    const air2vh_program_ext *e = (const air2vh_program_ext *)((const uint8_t *)p + p->struct_size);
    if (e->magic != AIR2VH_EXT_MAGIC || e->version != AIR2VH_EXT_VERSION || e->size != sizeof(air2vh_program_ext))
        return NULL;
    return e;
}

#define VH_ALU_SIZE_MASK   0x79u
#define VH_ALU_BASE_MASK   0x86u
#define VH_SECTION_GENERIC 3u

static int generic_varying(const air2vh_varying_slot *s)
{
    return s->alu_type && s->section == VH_SECTION_GENERIC;
}

/* Tier-A linking by Metal varying name, as the compiler library does (P1 vh_desc.c). */
int air2vh_link_varyings(const air2vh_program *vs, const air2vh_program *fs, air2vh_varying_link *out, uint32_t max,
                         uint32_t *generic_size_B)
{
    const air2vh_program_ext *ve = air2vh_program_get_ext(vs);
    const air2vh_program_ext *fe = air2vh_program_get_ext(fs);
    if (!ve || !fe || vs->stage != AIR2VH_STAGE_VERTEX || fs->stage != AIR2VH_STAGE_FRAGMENT || (max && !out))
        return -1;
    if (generic_size_B)
        *generic_size_B = ve->generic_size_B;

    const uint32_t nf = fe->n_varyings < AIR2VH_EXT_MAX_VARYINGS ? fe->n_varyings : AIR2VH_EXT_MAX_VARYINGS;
    const uint32_t nv = ve->n_varyings < AIR2VH_EXT_MAX_VARYINGS ? ve->n_varyings : AIR2VH_EXT_MAX_VARYINGS;
    uint32_t total = 0;
    for (uint32_t i = 0; i < nf; i++)
        total += generic_varying(&fe->varyings[i]);
    if (!out || max == 0)
        return (int)total;
    if (max < total)
        return -1;

    uint32_t n = 0;
    for (uint32_t i = 0; i < nf; i++) {
        const air2vh_varying_slot *f = &fe->varyings[i];
        if (!generic_varying(f))
            continue;
        air2vh_varying_link *l = &out[n++];
        memset(l, 0, sizeof(*l));
        l->fs_index = i;
        for (uint32_t j = 0; j < nv; j++) {
            const air2vh_varying_slot *v = &ve->varyings[j];
            if (!generic_varying(v) || !f->name[0] || strncmp(v->name, f->name, AIR2VH_EXT_NAME_LEN) != 0)
                continue;
            l->matched = 1;
            l->vs_offset = v->offset >= 0 ? (uint32_t)v->offset : 0;
            l->alu_type = (uint8_t)((f->alu_type & VH_ALU_BASE_MASK) | (v->alu_type & VH_ALU_SIZE_MASK));
            l->ncomps = v->ncomps;
            break;
        }
    }
    return (int)n;
}

int air2vh_compile_blend_shader(air2vh_compiler *c, const void *key, size_t key_size, air2vh_result *r)
{
    (void)c; (void)key; (void)key_size;
    memset(r, 0, sizeof(*r));
    r->error = dup_str("UNSUPPORTED: blend shaders are not in the offline pack yet");
    return -1;
}

void air2vh_result_free(air2vh_result *r)
{
    if (!r)
        return;
    if (r->program)
        free((void *)r->program->vs.layout.blob);
    free(r->program);
    free((void *)r->code);
    free(r->sidecar_json);
    free(r->error);
    memset(r, 0, sizeof(*r));
}

void air2vh_free(void *p) { free(p); }
