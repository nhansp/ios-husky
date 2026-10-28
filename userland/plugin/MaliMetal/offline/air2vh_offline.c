/*
 * air2vh API served from a precompiled MVP1 pack. A key miss goes to the per-user record cache, then to
 * the on-device compiler (Frameworks/libMaliCompiler.dylib, loaded on first use), whose results are cached
 * as MVP1 records; when that fails too, the request's AIR is dumped to MVP_MISS_DIR for the host packer
 * (userland/shadercache). Same lookup order as SnapMetal's AdrenoShader.m.
 */
#include "mvpack.h"
#include "air2vh_ext.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MVP_MISS_DIR "/private/var/tmp/malimetal"
#define MVP_COMPILER "Frameworks/libMaliCompiler.dylib"
#define MVP_CACHE_SUBDIR "org.mali-ios.MaliMetal"

struct air2vh_compiler {
    uint64_t gpu_id;
    uint32_t core_count;
    uint64_t gpu_features;
};

/* the on-device compiler, shared by every air2vh_compiler of the process */
static struct {
    pthread_once_t once;
    air2vh_compiler *comp;
    int (*compile)(air2vh_compiler *, const air2vh_compile_info *, air2vh_result *);
    int (*compile_blend)(air2vh_compiler *, const void *, size_t, air2vh_result *);
    void (*result_free)(air2vh_result *);
    const air2vh_program_ext *(*get_ext)(const air2vh_program *);
    char cache_dir[1024];       /* "" = no disk cache */
    char error[256];
    pthread_mutex_t lock;       /* the compiler object is not reentrant */
} g_rt = { .once = PTHREAD_ONCE_INIT, .lock = PTHREAD_MUTEX_INITIALIZER };
static air2vh_compiler g_rt_params;

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

static int bundle_path(char *path, size_t cap, const char *name)
{
    Dl_info di;
    if (!dladdr((const void *)&bundle_path, &di) || !di.dli_fname)
        return -1;
    const char *slash = strrchr(di.dli_fname, '/');
    int dir_len = slash ? (int)(slash - di.dli_fname) : 0;
    snprintf(path, cap, "%.*s/%s", dir_len, di.dli_fname, name);
    return 0;
}

static void pack_load(void)
{
    char path[1024];
    if (bundle_path(path, sizeof(path), MVP_FILENAME) != 0)
        return;

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
    /* Metal names every generated stage alike (air.vertexFetchFunction): only the exact AIR identifies one. */
    if (!info->entry || info->num_constants || !strncmp(info->entry, "air.", 4))
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

static int fill_record(const uint8_t *p, uint64_t length, const char key[64], air2vh_result *r)
{
    if (length < sizeof(mvp_record))
        return -1;
    const mvp_record *rec = (const mvp_record *)p;
    uint64_t need = (uint64_t)sizeof(*rec) + rec->program_size + rec->code_size + rec->sidecar_size + rec->layout_size;
    if (need > length || (rec->program_size != sizeof(air2vh_program) &&
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
    memcpy(r->key_hex, key, 64);
    r->key_hex[64] = 0;
    return 0;
}

static int fill_result(const mvp_index *e, air2vh_result *r)
{
    if (e->offset > g_pack.size || e->length > g_pack.size - e->offset)
        return -1;
    return fill_record(g_pack.base + e->offset, e->length, e->key, r);
}

/* ---- on-device compiler ---- */
static void rt_load(void)
{
    char path[1024];
    if (bundle_path(path, sizeof(path), MVP_COMPILER) != 0) {
        snprintf(g_rt.error, sizeof(g_rt.error), "cannot locate the bundle");
        return;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(g_rt.error, sizeof(g_rt.error), "%s", dlerror());
        return;
    }
    uint32_t (*api)(void) = (uint32_t (*)(void))dlsym(h, "air2vh_api_version");
    uint32_t (*abi)(void) = (uint32_t (*)(void))dlsym(h, "air2vh_abi_version");
    const char *(*id)(void) = (const char *(*)(void))dlsym(h, "air2vh_compiler_id");
    air2vh_compiler *(*create)(uint64_t, uint32_t, uint64_t, char **) =
        (air2vh_compiler *(*)(uint64_t, uint32_t, uint64_t, char **))dlsym(h, "air2vh_compiler_create");
    void (*rt_free)(void *) = (void (*)(void *))dlsym(h, "air2vh_free");
    g_rt.compile = (int (*)(air2vh_compiler *, const air2vh_compile_info *, air2vh_result *))dlsym(h, "air2vh_compile");
    g_rt.compile_blend = (int (*)(air2vh_compiler *, const void *, size_t, air2vh_result *))dlsym(h, "air2vh_compile_blend_shader");
    g_rt.result_free = (void (*)(air2vh_result *))dlsym(h, "air2vh_result_free");
    g_rt.get_ext = (const air2vh_program_ext *(*)(const air2vh_program *))dlsym(h, "air2vh_program_get_ext");
    if (!api || !abi || !id || !create || !rt_free || !g_rt.compile || !g_rt.compile_blend || !g_rt.result_free ||
        !g_rt.get_ext) {
        snprintf(g_rt.error, sizeof(g_rt.error), "%s lacks the air2vh API", path);
        return;
    }
    if (api() != AIR2VH_API_VERSION || abi() != AIR2VH_ABI_VERSION) {
        snprintf(g_rt.error, sizeof(g_rt.error), "compiler API %u ABI %u, plug-in wants %u/%u", api(), abi(),
                 AIR2VH_API_VERSION, AIR2VH_ABI_VERSION);
        return;
    }
    char *err = NULL;
    g_rt.comp = create(g_rt_params.gpu_id, g_rt_params.core_count, g_rt_params.gpu_features, &err);
    if (!g_rt.comp) {
        snprintf(g_rt.error, sizeof(g_rt.error), "compiler_create: %s", err ? err : "failed");
        rt_free(err);
        return;
    }

    /* records are only valid for the compiler that wrote them: one directory per compiler id */
    char base[768];
    const char *cid = id();
    if (confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof(base)) > 0 && base[0]) {
        uint64_t fnv = 0xcbf29ce484222325ull;
        for (const char *c = cid; *c; c++)
            fnv = (fnv ^ (uint8_t)*c) * 0x100000001b3ull;
        snprintf(g_rt.cache_dir, sizeof(g_rt.cache_dir), "%s/" MVP_CACHE_SUBDIR, base);
        mkdir(g_rt.cache_dir, 0755);
        size_t n = strlen(g_rt.cache_dir);
        snprintf(g_rt.cache_dir + n, sizeof(g_rt.cache_dir) - n, "/%016llx", (unsigned long long)fnv);
        if (mkdir(g_rt.cache_dir, 0755) != 0 && errno != EEXIST)
            g_rt.cache_dir[0] = 0;
    }
    mvp_log("MaliMetal: on-device compiler %s, cache %s\n", cid, g_rt.cache_dir[0] ? g_rt.cache_dir : "off");
}

static int cache_read(const char key[65], air2vh_result *r)
{
    char path[1200];
    if (!g_rt.cache_dir[0])
        return -1;
    snprintf(path, sizeof(path), "%s/%s.mvr", g_rt.cache_dir, key);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0)
        return -1;
    int rc = -1;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(mvp_record) && st.st_size < (64 << 20)) {
        uint8_t *buf = malloc((size_t)st.st_size);
        if (buf && read(fd, buf, (size_t)st.st_size) == (ssize_t)st.st_size)
            rc = fill_record(buf, (uint64_t)st.st_size, key, r);
        free(buf);
    }
    close(fd);
    return rc;
}

/* the result as one MVP1 record (the host packer's layout), into the cache and back through fill_record */
static int adopt_result(const char key[65], const air2vh_result *rr, air2vh_result *r)
{
    const air2vh_program_ext *ext = g_rt.get_ext(rr->program);
    mvp_record rec = {
        .program_size = (uint32_t)(sizeof(air2vh_program) + (ext ? sizeof(air2vh_program_ext) : 0)),
        .code_size = (uint32_t)rr->code_size,
        .sidecar_size = (uint32_t)rr->sidecar_size,
        .layout_size = rr->vs_layout_out.blob ? rr->vs_layout_out.blob_size : 0,
    };
    size_t total = sizeof(rec) + rec.program_size + rec.code_size + rec.sidecar_size + rec.layout_size;
    uint8_t *buf = malloc(total), *p = buf;
    if (!buf)
        return -1;
    memcpy(p, &rec, sizeof(rec));
    p += sizeof(rec);
    memcpy(p, rr->program, rec.program_size);
    ((air2vh_program *)p)->vs.layout.blob = NULL;
    p += rec.program_size;
    memcpy(p, rr->code, rec.code_size);
    p += rec.code_size;
    if (rec.sidecar_size)
        memcpy(p, rr->sidecar_json, rec.sidecar_size);
    p += rec.sidecar_size;
    if (rec.layout_size)
        memcpy(p, rr->vs_layout_out.blob, rec.layout_size);

    if (g_rt.cache_dir[0]) {
        char path[1200], tmp[1240];
        snprintf(path, sizeof(path), "%s/%s.mvr", g_rt.cache_dir, key);
        snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            bool ok = write(fd, buf, total) == (ssize_t)total;
            close(fd);
            if (!ok || rename(tmp, path) != 0)
                unlink(tmp);
        }
    }
    int rc = fill_record(buf, total, key, r);
    free(buf);
    return rc;
}

static int rt_compile(const air2vh_compile_info *info, const char key[65], air2vh_result *r)
{
    pthread_once(&g_rt.once, rt_load);
    if (!g_rt.comp)
        return -1;
    if (cache_read(key, r) == 0)
        return 0;
    air2vh_result rr;
    memset(&rr, 0, sizeof(rr));
    pthread_mutex_lock(&g_rt.lock);
    int rc = g_rt.compile(g_rt.comp, info, &rr);
    pthread_mutex_unlock(&g_rt.lock);
    if (rc == 0 && rr.program && rr.code)
        rc = adopt_result(key, &rr, r);
    else {
        snprintf(g_rt.error, sizeof(g_rt.error), "%s", rr.error ? rr.error : "compile failed");
        rc = -1;
    }
    g_rt.result_free(&rr);
    return rc;
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
    pthread_once(&g_pack.once, pack_load);
    air2vh_compiler *c = calloc(1, sizeof(*c));
    if (!c) {
        if (error)
            *error = dup_str("out of memory");
        return NULL;
    }
    c->gpu_id = gpu_id;
    c->core_count = core_count;
    c->gpu_features = gpu_features;
    if (!g_rt_params.gpu_id)
        g_rt_params = *c;
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
    if (rt_compile(info, key, r) == 0)
        return 0;

    dump_miss(info, key);
    snprintf(msg, sizeof(msg), "UNSUPPORTED: no binary for %s (key %s, AIR in " MVP_MISS_DIR "): %s",
             info->entry ? info->entry : "(first entry)", key, g_rt.error[0] ? g_rt.error : "?");
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
    (void)c;
    memset(r, 0, sizeof(*r));
    pthread_once(&g_rt.once, rt_load);
    if (!g_rt.comp) {
        r->error = dup_str(g_rt.error[0] ? g_rt.error : "no on-device compiler");
        return -1;
    }
    air2vh_result rr;
    memset(&rr, 0, sizeof(rr));
    pthread_mutex_lock(&g_rt.lock);
    int rc = g_rt.compile_blend(g_rt.comp, key, key_size, &rr);
    pthread_mutex_unlock(&g_rt.lock);
    if (rc == 0 && rr.code) {
        void *code = malloc(rr.code_size ? rr.code_size : 1);
        if (code) {
            memcpy(code, rr.code, rr.code_size);
            r->code = code;
            r->code_size = rr.code_size;
        } else
            rc = -1;
    } else {
        r->error = dup_str(rr.error ? rr.error : "blend shader compile failed");
        rc = -1;
    }
    g_rt.result_free(&rr);
    return rc;
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
