/*
 * MaliShader.m - MTLB parsing + air2valhall front
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliShader.m
 *
 * The MTLB container layout (header 88 bytes, four sections: function table, public metadata, private
 * metadata, bitcode; per-function tags NAME/TYPE/HASH/MDSZ/OFFT/ENDT) is the Metal library format that
 * SnapMetal's AdrenoCompilerInput.m also reads; the field offsets below were re-derived from the
 * iOS QuartzCore default.metallib slice (spec 05 section 5.1) and are re-validated at runtime (every range is
 * bounds-checked, the SHA-256 of the module must equal the HASH tag).
 */
#import "MaliShader.h"
#import <CommonCrypto/CommonDigest.h>
#include <string.h>

/* ---------------------------------------------------------------------- *
 * little-endian readers and range checks
 * ---------------------------------------------------------------------- */
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static BOOL within(uint64_t off, uint64_t size, uint64_t total) { return off <= total && size <= total - off; }
static id fail(NSString **error, NSString *msg) { if (error) *error = msg; return nil; }

static NSString *hex32(const unsigned char *d) {
    NSMutableString *s = [NSMutableString stringWithCapacity:64];
    for (unsigned i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", d[i]];
    return s;
}

/* LLVM bitcode, raw (BC 0xC0DE) or inside the 0x0B17C0DE wrapper. */
static BOOL looks_like_bitcode(const unsigned char *air, uint64_t n) {
    if (n < 4) return NO;
    if (rd32(air) == 0x0b17c0de) {
        if (n < 20 || rd32(air + 4) != 0 || rd32(air + 8) < 20 || rd32(air + 12) < 4 ||
            !within(rd32(air + 8), rd32(air + 12), n) || rd32(air + rd32(air + 8)) != 0xdec04342)
            return NO;
        return YES;
    }
    return rd32(air) == 0xdec04342;
}

@implementation MaliCompilerInput
+ (instancetype)inputWithCompilerOutput:(id)output stage:(NSUInteger)stage entry:(NSString *)entry error:(NSString **)error {
    if (error) *error = nil;
    if (stage > 2) return fail(error, @"unsupported shader stage");
    const NSUInteger limit = 16u << 20;
    NSData *data = nil;
    if ([output isKindOfClass:[NSData class]]) {
        if ([output length] > limit) return fail(error, @"compiler output exceeds bound");
        data = [output copy];
    } else {
        Class dd = NSClassFromString(@"OS_dispatch_data");
        if (!dd || ![output isKindOfClass:dd]) return fail(error, @"unsupported compiler output object");
        dispatch_data_t raw = (dispatch_data_t)output;
        if (dispatch_data_get_size(raw) > limit) return fail(error, @"compiler output exceeds bound");
        const void *bytes = NULL;
        size_t size = 0;
        dispatch_data_t mapped = dispatch_data_create_map(raw, &bytes, &size);
        if (!mapped || size > limit) return fail(error, @"compiler output cannot be mapped");
        data = [NSData dataWithBytes:bytes length:size];
    }
    const unsigned char *b = data.bytes;
    const uint64_t total = data.length;
    if (total < 88 || memcmp(b, "MTLB", 4) || rd16(b + 6) != 2 || rd16(b + 8) != 4)
        return fail(error, @"expected MTLB 2.4 compiler output");
    if (rd64(b + 16) != total) return fail(error, @"MTLB length mismatch");
    uint64_t off[4], size[4];
    for (unsigned i = 0; i < 4; i++) {
        off[i] = rd64(b + 24 + i * 16);
        size[i] = rd64(b + 32 + i * 16);
        const uint64_t extra = i == 0 ? 4 : 0;           /* the function table's 4-byte count precedes its entries */
        if (off[i] < 88 || !within(off[i], extra, total) || !within(off[i] + extra, size[i], total))
            return fail(error, @"MTLB section outside container");
    }
    uint64_t at = off[0] + 4;
    const uint64_t end = at + size[0];
    const uint32_t count = rd32(b + off[0]);
    if (!count || count > 4096 || count > size[0] / 8) return fail(error, @"invalid MTLB function count");

    NSData *selected = nil;
    NSString *selectedName = nil;
    unsigned char selectedHash[CC_SHA256_DIGEST_LENGTH] = {0};
    for (uint32_t index = 0; index < count; index++) {
        if (!within(at, 4, end)) return fail(error, @"short function entry");
        const uint64_t entrySize = rd32(b + at);
        if (entrySize < 8 || !within(at, entrySize, end)) return fail(error, @"function entry exceeds table");
        const uint64_t stop = at + entrySize;
        uint64_t tag = at + 4, moduleOffset = 0, moduleSize = 0;
        NSData *hash = nil;
        NSString *name = nil;
        NSUInteger type = NSUIntegerMax;
        unsigned seen = 0;
        BOOL ended = NO;
        while (within(tag, 4, stop)) {
            const unsigned char *kind = b + tag;
            tag += 4;
            if (!memcmp(kind, "ENDT", 4)) { ended = YES; break; }
            if (!within(tag, 2, stop)) return fail(error, @"short function tag");
            const uint16_t length = rd16(b + tag);
            tag += 2;
            if (!within(tag, length, stop)) return fail(error, @"function tag exceeds entry");
            const unsigned char *value = b + tag;
            unsigned flag = 0;
            if (!memcmp(kind, "NAME", 4)) {
                flag = 1;
                if (length < 2 || value[length - 1] || memchr(value, 0, length - 1)) return fail(error, @"malformed entry name");
                name = [[NSString alloc] initWithBytes:value length:length - 1 encoding:NSUTF8StringEncoding];
                if (!name.length) return fail(error, @"invalid entry name encoding");
            } else if (!memcmp(kind, "TYPE", 4)) {
                flag = 2;
                if (length != 1) return fail(error, @"malformed stage tag");
                type = value[0];
            } else if (!memcmp(kind, "HASH", 4)) {
                flag = 4;
                if (length != CC_SHA256_DIGEST_LENGTH) return fail(error, @"malformed module hash");
                hash = [NSData dataWithBytes:value length:length];
            } else if (!memcmp(kind, "MDSZ", 4)) {
                flag = 8;
                if (length != 8) return fail(error, @"malformed module size tag");
                moduleSize = rd64(value);
            } else if (!memcmp(kind, "OFFT", 4)) {
                flag = 16;
                if (length != 24 || rd64(value) >= size[1] || rd64(value + 8) >= size[2]) return fail(error, @"malformed module offsets");
                moduleOffset = rd64(value + 16);
            }
            if (flag && (seen & flag)) return fail(error, @"duplicate identity tag");
            seen |= flag;
            tag += length;
        }
        if (!ended || tag != stop || seen != 31 || moduleSize < 4 || !within(moduleOffset, moduleSize, size[3]))
            return fail(error, @"incomplete function entry or module range");
        if (type == stage && (!entry || [entry isEqualToString:name])) {
            if (selected) return fail(error, @"ambiguous compiler output entry");
            selected = [data subdataWithRange:NSMakeRange((NSUInteger)(off[3] + moduleOffset), (NSUInteger)moduleSize)];
            selectedName = name;
            memcpy(selectedHash, hash.bytes, sizeof selectedHash);
        }
        at = stop;
    }
    if (at != end || !selected) return fail(error, @"no unique matching shader stage and entry");
    unsigned char actual[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(selected.bytes, (CC_LONG)selected.length, actual);
    if (memcmp(actual, selectedHash, sizeof actual)) return fail(error, @"module hash mismatch");
    if (!looks_like_bitcode(selected.bytes, selected.length)) return fail(error, @"module is not LLVM bitcode");

    MaliCompilerInput *in = [MaliCompilerInput new];
    in->_airModule = selected;
    in->_entry = selectedName;
    in->_stage = stage;
    in->_airSHA256 = hex32(actual);
    return in;
}

+ (instancetype)inputWithAIR:(NSData *)air entry:(NSString *)entry stage:(NSUInteger)stage error:(NSString **)error {
    if (error) *error = nil;
    if (!air.length || !looks_like_bitcode(air.bytes, air.length)) return fail(error, @"built-in module is not LLVM bitcode");
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(air.bytes, (CC_LONG)air.length, d);
    MaliCompilerInput *in = [MaliCompilerInput new];
    in->_airModule = [air copy];
    in->_entry = [entry copy];
    in->_stage = stage;
    in->_airSHA256 = hex32(d);
    return in;
}
@end

/* ---------------------------------------------------------------------- *
 * program holder
 * ---------------------------------------------------------------------- */
@implementation MaliShaderProgram {
@public
    NSData *_code, *_layoutBlob;
    NSString *_key, *_entry;
}
- (NSData *)code { return _code; }
- (NSData *)layoutBlob { return _layoutBlob; }
- (NSString *)key { return _key; }
- (NSString *)entry { return _entry; }
@end

/* The sidecar's constexpr samplers (spec 07 8.3). The JSON key names are the contract's wording
 * ("constant_samplers.samplers[]" with "slot", "mali_sampler", "mali_sampler_int_border"); anything that does not
 * match is ignored and the shader then samples with whatever the API bound, which is logged by the caller. */
static void parse_const_samplers(MaliShaderProgram *p, const char *json, size_t len) {
    p->csamp_count = 0;
    if (!json || !len) return;
    NSData *d = [NSData dataWithBytesNoCopy:(void *)json length:len freeWhenDone:NO];
    id root = [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
    if (![root isKindOfClass:[NSDictionary class]]) return;
    id cs = ((NSDictionary *)root)[@"constant_samplers"];
    NSArray *list = nil;
    if ([cs isKindOfClass:[NSDictionary class]]) list = ((NSDictionary *)cs)[@"samplers"];
    else if ([cs isKindOfClass:[NSArray class]]) list = cs;
    if (![list isKindOfClass:[NSArray class]]) return;
    for (id e in list) {
        if (![e isKindOfClass:[NSDictionary class]] || p->csamp_count >= MALI_MAX_CONST_SAMPLERS) continue;
        NSDictionary *dict = e;
        NSNumber *slot = dict[@"slot"];
        NSArray *w = dict[@"mali_sampler"], *ib = dict[@"mali_sampler_int_border"];
        if (![slot isKindOfClass:[NSNumber class]] || ![w isKindOfClass:[NSArray class]] || w.count != 8) continue;
        MaliConstSampler *s = &p->csamp[p->csamp_count];
        memset(s, 0, sizeof *s);
        s->slot = slot.unsignedIntValue;
        for (unsigned i = 0; i < 8; i++) s->desc[i] = [w[i] respondsToSelector:@selector(unsignedIntValue)] ? [w[i] unsignedIntValue] : 0;
        if ([ib isKindOfClass:[NSArray class]] && ib.count == 4)
            for (unsigned i = 0; i < 4; i++) s->int_border[i] = [ib[i] respondsToSelector:@selector(unsignedIntValue)] ? [ib[i] unsignedIntValue] : 0;
        else
            memcpy(s->int_border, s->desc + 4, sizeof s->int_border);
        p->csamp_count++;
    }
}

/* ---------------------------------------------------------------------- *
 * compiler
 * ---------------------------------------------------------------------- */
@implementation MaliShaderCompiler {
    air2vh_compiler *_c;
    NSLock *_lock;                         /* air2vh_compile is not documented as reentrant */
    NSMutableDictionary<NSString *, MaliShaderProgram *> *_cache;
    NSMutableDictionary<NSData *, MaliShaderProgram *> *_blendCache;
    NSString *_compilerID;
}
- (instancetype)initWithProps:(const mali_dev_props_t *)p error:(NSString **)error {
    if ((self = [super init])) {
        if (air2vh_api_version() != AIR2VH_API_VERSION || air2vh_abi_version() != AIR2VH_ABI_VERSION) {
            if (error) *error = [NSString stringWithFormat:@"air2vh API %u / ABI %u, built against %u / %u", air2vh_api_version(),
                                 air2vh_abi_version(), AIR2VH_API_VERSION, AIR2VH_ABI_VERSION];
            return nil;
        }
        char *e = NULL;
        _c = air2vh_compiler_create(p->gpu_id, p->core_count, p->gpu_features, &e);
        if (!_c) {
            if (error) *error = e ? @(e) : @"air2vh_compiler_create failed";
            if (e) air2vh_free(e);
            return nil;
        }
        _lock = [NSLock new];
        _cache = [NSMutableDictionary dictionary];
        _blendCache = [NSMutableDictionary dictionary];
        const char *id_ = air2vh_compiler_id();
        _compilerID = id_ ? @(id_) : @"air2vh";
    }
    return self;
}
- (void)dealloc { if (_c) air2vh_compiler_destroy(_c); }
- (NSString *)compilerID { return _compilerID; }

/* Copy the compiler's result into an owning MaliShaderProgram; false (and *error) on a malformed result. */
- (MaliShaderProgram *)adopt:(const air2vh_result *)r entry:(NSString *)entry error:(NSString **)error {
    if (!r->code || !r->code_size || !r->program) return fail(error, @"compiler returned no code");
    const air2vh_program *prog = r->program;
    if (prog->abi_version != AIR2VH_ABI_VERSION || prog->struct_size < offsetof(air2vh_program, contains_barrier))
        return fail(error, [NSString stringWithFormat:@"air2vh_program ABI %u size %u not understood", prog->abi_version, prog->struct_size]);
    MaliShaderProgram *p = [MaliShaderProgram new];
    memset(&p->prog, 0, sizeof p->prog);
    memcpy(&p->prog, prog, MIN((size_t)prog->struct_size, sizeof p->prog));
    p->_code = [NSData dataWithBytes:r->code length:r->code_size];
    if (r->vs_layout_out.blob && r->vs_layout_out.blob_size) {
        p->_layoutBlob = [NSData dataWithBytes:r->vs_layout_out.blob length:r->vs_layout_out.blob_size];
        p->layout = r->vs_layout_out;
        p->layout.blob = p->_layoutBlob.bytes;
    }
    p->_key = @(r->key_hex);
    p->_entry = entry;
    parse_const_samplers(p, r->sidecar_json, r->sidecar_size);
    return p;
}

- (MaliShaderProgram *)compile:(MaliCompilerInput *)input flags:(uint32_t)flags vsLayout:(MaliShaderProgram *)vs error:(NSString **)error {
    if (error) *error = nil;
    air2vh_compile_info info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    info.air = input.airModule.bytes;
    info.air_size = input.airModule.length;
    info.entry = input.entry.UTF8String;
    info.stage = (uint32_t)input.stage + 1;              /* MTLB 0/1/2 -> AIR2VH_STAGE_VERTEX/FRAGMENT/KERNEL */
    info.flags = flags;
    info.vs_layout = (vs && vs->layout.blob) ? &vs->layout : NULL;
    if (info.stage == AIR2VH_STAGE_FRAGMENT && !info.vs_layout && vs)
        return fail(error, @"vertex program produced no varying layout");

    [_lock lock];
    @try {
        char key[65] = {0};
        char *e = NULL;
        if (air2vh_cache_key(_c, &info, key, &e) == 0) {
            MaliShaderProgram *hit = _cache[@(key)];
            if (hit) return hit;
        }
        if (e) { air2vh_free(e); e = NULL; }
        air2vh_result r;
        memset(&r, 0, sizeof r);
        const int rc = air2vh_compile(_c, &info, &r);
        if (rc != 0 || r.error) {
            NSString *why = r.error ? @(r.error) : [NSString stringWithFormat:@"air2vh_compile returned %d", rc];
            air2vh_result_free(&r);
            return fail(error, why);
        }
        MaliShaderProgram *p = [self adopt:&r entry:input.entry error:error];
        air2vh_result_free(&r);
        if (p) _cache[p.key] = p;
        return p;
    } @finally { [_lock unlock]; }
}

- (MaliShaderProgram *)compileBlendShaderKey:(NSData *)key error:(NSString **)error {
    if (error) *error = nil;
    [_lock lock];
    @try {
        MaliShaderProgram *hit = _blendCache[key];
        if (hit) return hit;
        air2vh_result r;
        memset(&r, 0, sizeof r);
        const int rc = air2vh_compile_blend_shader(_c, key.bytes, key.length, &r);
        if (rc != 0 || r.error) {
            NSString *why = r.error ? @(r.error) : [NSString stringWithFormat:@"air2vh_compile_blend_shader returned %d", rc];
            air2vh_result_free(&r);
            return fail(error, why);
        }
        /* A blend shader's result has code (+ possibly no program struct): synthesise an empty one. */
        MaliShaderProgram *p = nil;
        if (r.program) p = [self adopt:&r entry:@"blend" error:error];
        else if (r.code && r.code_size) {
            p = [MaliShaderProgram new];
            memset(&p->prog, 0, sizeof p->prog);
            p->prog.struct_size = sizeof p->prog;
            p->prog.abi_version = AIR2VH_ABI_VERSION;
            p->prog.code_size = (uint32_t)r.code_size;
            p->_code = [NSData dataWithBytes:r.code length:r.code_size];
            p->_key = @(r.key_hex);
            p->_entry = @"blend";
        } else fail(error, @"blend shader compile returned no code");
        air2vh_result_free(&r);
        if (p) _blendCache[[key copy]] = p;
        return p;
    } @finally { [_lock unlock]; }
}
@end
