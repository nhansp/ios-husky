/*
 * MaliInternal.m - the plug-in's built-in pipelines (shaders/MaliInternal.metal)
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliInternal.m
 *
 * The AIR module is linked into the bundle as section __MALI,__internal (build.sh -sectcreate); it holds the
 * fullscreen "load" vertex/fragment shaders and the blit/mip kernels. Programs are compiled by air2valhall on first
 * use, uploaded to executable memory and kept for the life of the device.
 */
#import "MaliMTLObjectsPrivate.h"
#import <os/log.h>
#import <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

@interface MaliInternalEntry : NSObject {
@public
    mali_gfx_pipeline_t gfx;
    mali_compute_pipeline_t comp;
}
@property (nonatomic, strong) NSMutableArray *holders;
@end
@implementation MaliInternalEntry
- (instancetype)init { if ((self = [super init])) { memset(&gfx, 0, sizeof gfx); memset(&comp, 0, sizeof comp); _holders = [NSMutableArray array]; } return self; }
@end

@implementation MaliInternalShaders {
    __unsafe_unretained MaliMTLDevice *_device;      /* the device owns this object */
    NSData *_air;
    NSLock *_lock;
    NSMutableDictionary<NSString *, MaliInternalEntry *> *_entries;
    NSMutableDictionary<NSString *, MaliShaderProgram *> *_programs;
    NSString *_loadFailure;
}
- (instancetype)initWithDevice:(MaliMTLDevice *)device {
    if ((self = [super init])) {
        _device = device;
        _lock = [NSLock new];
        _entries = [NSMutableDictionary dictionary];
        _programs = [NSMutableDictionary dictionary];
        Dl_info image;
        unsigned long size = 0;
        const uint8_t *air = (dladdr((const void *)&MaliUploadShader, &image) && image.dli_fbase)
            ? getsectiondata((const struct mach_header_64 *)image.dli_fbase, "__MALI", "__internal", &size) : NULL;
        if (air && size) _air = [NSData dataWithBytes:air length:size];
        else _loadFailure = @"the built-in AIR module (__MALI,__internal) is missing from the bundle";
        if (_loadFailure) MaliDiagLine(_loadFailure);
    }
    return self;
}

/* Compile (or fetch) one entry point of the built-in module. Caller holds _lock. */
- (MaliShaderProgram *)programNamed:(NSString *)name stage:(NSUInteger)stage vs:(MaliShaderProgram *)vs {
    MaliShaderProgram *hit = _programs[name];
    if (hit) return hit;
    if (!_air) return nil;
    NSString *err = nil;
    MaliCompilerInput *in = [MaliCompilerInput inputWithAIR:_air entry:name stage:stage error:&err];
    MaliShaderProgram *p = in ? [[_device maliShaderCompiler] compile:in flags:0 vsLayout:vs error:&err] : nil;
    if (!p) { MaliDiagLine([NSString stringWithFormat:@"built-in shader %@ failed: %@", name, err]); return nil; }
    _programs[name] = p;
    return p;
}

- (const mali_gfx_pipeline_t *)loadPipelineForRT:(uint32_t)rt format:(uint32_t)fmt kind:(int)kind resources:(NSArray **)res {
    if (rt >= MALI_MAX_RTS || kind < 0 || kind > 2) return NULL;
    NSString *key = [NSString stringWithFormat:@"load.%u.%u.%d", rt, fmt, kind];
    [_lock lock];
    @try {
        MaliInternalEntry *e = _entries[key];
        if (!e) {
            MaliShaderProgram *vs = [self programNamed:@"mali_fullscreen_vs" stage:0 vs:nil];
            if (!vs) return NULL;
            const char suffix[3] = {'f', 'u', 'i'};
            MaliShaderProgram *fs = [self programNamed:[NSString stringWithFormat:@"mali_load_%c%u", suffix[kind], rt] stage:1 vs:vs];
            if (!fs) return NULL;
            e = [MaliInternalEntry new];
            if (!MaliUploadShader(_device, vs, &e->gfx.vs, e.holders, NULL) || !MaliUploadShader(_device, fs, &e->gfx.fs, e.holders, NULL)) return NULL;
            e->gfx.rt_count = rt + 1;
            e->gfx.rt[rt].mtl_format = fmt;
            e->gfx.rt[rt].write_mask = 0xf;
            e->gfx.sample_count = 1;
            e->gfx.rasterization_enabled = true;
            e->gfx.input_prim_class = 3;
            _entries[key] = e;
        }
        *res = e.holders;
        return &e->gfx;
    } @finally { [_lock unlock]; }
}

- (const mali_compute_pipeline_t *)computePipeline:(NSString *)name resources:(NSArray **)res {
    NSString *key = [@"cs." stringByAppendingString:name];
    [_lock lock];
    @try {
        MaliInternalEntry *e = _entries[key];
        if (!e) {
            MaliShaderProgram *p = [self programNamed:name stage:2 vs:nil];
            if (!p) return NULL;
            e = [MaliInternalEntry new];
            if (!MaliUploadShader(_device, p, &e->comp.cs, e.holders, NULL)) return NULL;
            e->comp.tg_threads_hint = 64;
            _entries[key] = e;
        }
        *res = e.holders;
        return &e->comp;
    } @finally { [_lock unlock]; }
}
@end
