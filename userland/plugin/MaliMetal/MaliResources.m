/*
 * MaliResources.m - buffers, textures, samplers and depth-stencil states
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliResources.m
 *
 * Object model forked from SnapMetal's AdrenoResources.m (native IOGPU allocations carrying a vendor argument
 * tail, a CPU view for replaceRegion:/getBytes:, aligned shadows for IOSurfaces whose rows are not 64-byte
 * multiples). Mali specifics:
 *   - the vendor tail is mali_new_resource_tail (MaliPluginABI.h D-2): MALI_BO_* flags, notably the NC mapping
 *     (the GPU is not IO-coherent, mali_backend_ops.h section 3);
 *   - texture layout and descriptors come from the C core (mali_texture.c, Mesa pan_image_layout_init), so the GPU
 *     descriptors and the CPU copy paths read one layout;
 *   - only linear images (no AFBC / 16x16 tiling) in v1, S06 section 11 item 9.
 */
#import "MaliMTLObjectsPrivate.h"
#import <os/log.h>
#import <objc/runtime.h>

static os_log_t mali_res_log(void) {
    static os_log_t l;
    static dispatch_once_t o;
    dispatch_once(&o, ^{ l = os_log_create("org.mali-ios.metal", "resources"); });
    return l;
}

#define PAGE MALI_PAGE_SIZE
static inline NSUInteger align_up(NSUInteger v, NSUInteger a) { return (v + a - 1) & ~(a - 1); }

/* ---------------------------------------------------------------------- *
 * argument tail
 * ---------------------------------------------------------------------- */
static void args_init(mali_new_resource_args_t *a, uint32_t bo_flags, uint32_t mtl_format) {
    memset(a, 0, sizeof *a);
    a->tail.magic = MALI_RES_TAIL_MAGIC;
    a->tail.bo_flags = bo_flags;
    a->tail.mtl_pixel_format = mtl_format;
}
/* Flags for a resource created with `options` (MTLResourceOptions): everything the CPU can map is NC (S06 6.3). */
static uint32_t bo_flags_for(MTLResourceOptions o, BOOL texture) {
    uint32_t f = MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_ZERO;
    const NSUInteger storage = (o >> MTLResourceStorageModeShift) & 0xf;
    if (storage != MTLStorageModePrivate && storage != MTLStorageModeMemoryless) f |= MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE;
    (void)texture;
    return f;
}

/* ====================================================================== *
 * IOSurface aligned shadows
 *   One per IOSurface id in this process, shared by every texture and view over it. The GPU reads and writes the
 *   shadow (64-byte aligned rows, the layout pan_image accepts); CPU writes to the surface (its seed, or a
 *   replaceRegion) refill it before the next GPU use, and command buffers that wrote it copy it back on completion.
 * ====================================================================== */
@interface MaliSurfaceShadow : NSObject {
@public
    IOSurfaceRef _surface;
    id<MTLBuffer> _buffer;
    NSUInteger _rowBytes, _surfaceRow, _width, _height, _cpp;
    uint32_t _seed;
    BOOL _dirty;
}
- (void)syncIfNeeded;
- (void)writeBack;
- (void)markDirty;
@end
@implementation MaliSurfaceShadow
- (void)dealloc { if (_surface) CFRelease(_surface); }
- (void)fillLocked {
    uint32_t seed = 0;
    if (IOSurfaceLock(_surface, kIOSurfaceLockReadOnly, &seed) != kIOReturnSuccess) {
        os_log_error(mali_res_log(), "IOSurface %u shadow fill: lock refused", IOSurfaceGetID(_surface));
        return;
    }
    const char *src = IOSurfaceGetBaseAddress(_surface);
    char *dst = _buffer.contents;
    const NSUInteger n = _width * _cpp;
    if (src && dst) for (NSUInteger y = 0; y < _height; y++) memcpy(dst + y * _rowBytes, src + y * _surfaceRow, n);
    IOSurfaceUnlock(_surface, kIOSurfaceLockReadOnly, &seed);
    _seed = IOSurfaceGetSeed(_surface);
    _dirty = NO;
}
- (void)syncIfNeeded {
    @synchronized(self) { if (_dirty || IOSurfaceGetSeed(_surface) != _seed) [self fillLocked]; }
}
- (void)writeBack {
    @synchronized(self) {
        if (IOSurfaceLock(_surface, 0, NULL) != kIOReturnSuccess) {
            os_log_error(mali_res_log(), "IOSurface %u shadow write-back: lock refused", IOSurfaceGetID(_surface));
            return;
        }
        char *dst = IOSurfaceGetBaseAddress(_surface);
        const char *src = _buffer.contents;
        const NSUInteger n = _width * _cpp;
        if (src && dst) for (NSUInteger y = 0; y < _height; y++) memcpy(dst + y * _surfaceRow, src + y * _rowBytes, n);
        IOSurfaceUnlock(_surface, 0, NULL);
        _seed = IOSurfaceGetSeed(_surface);          /* our own write is not a CPU change to refill from */
        _dirty = NO;
    }
}
- (void)markDirty { @synchronized(self) { _dirty = YES; } }
@end

static NSMapTable *shadows_by_surface(void) {          /* IOSurface id -> shadow (weak) */
    static NSMapTable *t;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ t = [NSMapTable strongToWeakObjectsMapTable]; });
    return t;
}
static NSMapTable *shadows_by_buffer(void) {           /* shadow buffer -> shadow (weak both) */
    static NSMapTable *t;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ t = [NSMapTable weakToWeakObjectsMapTable]; });
    return t;
}
static MaliSurfaceShadow *shadow_for_surface(id<MTLDevice> dev, IOSurfaceRef s, NSUInteger cpp) {
    const NSUInteger width = IOSurfaceGetWidth(s), height = IOSurfaceGetHeight(s), row = IOSurfaceGetBytesPerRow(s);
    if (!dev || !width || !height || !cpp || width > 16384 || height > 16384 || row < width * cpp) return nil;
    const NSUInteger aligned = align_up(width * cpp, 64);
    NSNumber *key = @(IOSurfaceGetID(s));
    @synchronized(shadows_by_surface()) {
        MaliSurfaceShadow *sh = [shadows_by_surface() objectForKey:key];
        if (sh && sh->_surface == s && sh->_rowBytes == aligned && sh->_surfaceRow == row && sh->_width == width &&
            sh->_height == height && sh->_cpp == cpp)
            return sh;
        sh = [MaliSurfaceShadow new];
        sh->_buffer = [dev newBufferWithLength:aligned * height options:MTLResourceStorageModeShared];
        if (!sh->_buffer || !sh->_buffer.contents || sh->_buffer.length < aligned * height) return nil;
        sh->_buffer.label = @"MaliMetal IOSurface shadow";
        sh->_surface = (IOSurfaceRef)CFRetain(s);
        sh->_rowBytes = aligned; sh->_surfaceRow = row; sh->_width = width; sh->_height = height; sh->_cpp = cpp;
        @synchronized(sh) { [sh fillLocked]; }
        [shadows_by_surface() setObject:sh forKey:key];
        @synchronized(shadows_by_buffer()) { [shadows_by_buffer() setObject:sh forKey:sh->_buffer]; }
        return sh;
    }
}
id MaliShadowOfBacking(id resource) {
    if (!resource) return nil;
    @synchronized(shadows_by_buffer()) { return [shadows_by_buffer() objectForKey:resource]; }
}
void MaliShadowWriteBack(id shadow) {
    if ([shadow isKindOfClass:[MaliSurfaceShadow class]]) [(MaliSurfaceShadow *)shadow writeBack];
}

/* plug-in private MaliTexture entry points used by the creation functions and by MaliBuffer */
@interface MaliTexture (MaliInternal)
- (void)maliSetUsage:(MTLTextureUsage)u;
- (void)maliRefuse:(NSString *)why;
- (BOOL)maliAttachLayout:(const mali_texture_params_t *)p ctx:(const mali_ctx_t *)ctx baseGPU:(uint64_t)base available:(uint64_t)avail;
- (void)maliAttachLinearBacking:(MaliBuffer *)buffer descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)bpr
                            ctx:(const mali_ctx_t *)ctx;
- (void)maliAttachShadow:(MaliSurfaceShadow *)sh;
- (void)maliSetCPU:(void *)base bytes:(NSUInteger)n;
- (void)maliInheritFrom:(MaliTexture *)parent;
- (void)maliSetMali:(mali_texture_t *)mt;
@end

/* ====================================================================== *
 * Buffers
 * ====================================================================== */
@implementation MaliBuffer
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)bpr {
    return [self newTiledTextureWithDescriptor:d offset:o bytesPerRow:bpr];
}
/* A linear 2D texture aliasing this buffer (SkyLight / CoreImage). Needs bytesPerRow a 64-byte multiple (pan_image). */
- (id<MTLTexture>)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)bpr {
    MaliMTLDevice *dev = (MaliMTLDevice *)self.device;
    mali_fmt_info_t fi;
    if (!d || d.textureType != MTLTextureType2D || !d.width || !d.height || d.depth > 1 || d.arrayLength > 1 ||
        d.mipmapLevelCount > 1 || d.sampleCount > 1 || !mali_fmt_lookup((uint32_t)d.pixelFormat, &fi) || !fi.bytes_per_pixel ||
        bpr < d.width * fi.bytes_per_pixel || (bpr & 63) || (o & 63) || o > self.length || d.height > (self.length - o) / bpr) {
        MaliLogOnce([NSString stringWithFormat:@"buffer texture refused: fmt %lu %lux%lu bpr %lu offset %lu", (unsigned long)d.pixelFormat,
                     (unsigned long)d.width, (unsigned long)d.height, (unsigned long)bpr, (unsigned long)o]);
        return nil;
    }
    MaliTexture *t = [[MaliTexture alloc] initWithBuffer:self descriptor:d offset:o bytesPerRow:bpr];
    if (!t) return nil;
    [t maliAttachLinearBacking:self descriptor:d offset:o bytesPerRow:bpr ctx:[dev maliCtx]];
    return t;
}
- (void)didModifyRange:(NSRange)r { /* CPU writes reach the GPU at the kernel's clean before submit (resource list) */ }
@end

id<MTLBuffer> MaliNewBuffer(id<MTLDevice> dev, void *noCopy, NSUInteger len, MTLResourceOptions o,
                            void (^dealloc)(void *, NSUInteger)) {
    const NSUInteger size = align_up(len, PAGE);
    mali_new_resource_args_t a;
    args_init(&a, bo_flags_for(o, NO), 0);
    return [[MaliBuffer alloc] initWithDevice:dev pointer:noCopy length:len alignment:PAGE options:o sysMemSize:size gpuAddress:0
                                         args:&a argsSize:sizeof a deallocator:dealloc];
}
id<MTLBuffer> MaliNewBufferWithIOSurface(id<MTLDevice> dev, IOSurfaceRef s) {
    mali_new_resource_args_t a;
    args_init(&a, MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE, 0);
    return [[MaliBuffer alloc] initWithDevice:dev iosurface:s args:&a argsSize:sizeof a];
}
id<MTLBuffer> MaliNewGPUBuffer(id<MTLDevice> dev, NSUInteger len, uint32_t boFlags) {
    const NSUInteger size = align_up(MAX(len, (NSUInteger)1), PAGE);
    mali_new_resource_args_t a;
    args_init(&a, boFlags | MALI_BO_ZERO, 0);
    return [[MaliBuffer alloc] initWithDevice:dev pointer:NULL length:size alignment:PAGE
                                      options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined
                                   sysMemSize:size gpuAddress:0 args:&a argsSize:sizeof a deallocator:nil];
}

/* ====================================================================== *
 * Textures
 * ====================================================================== */
static uint32_t slice_count_of(MTLTextureType type, NSUInteger arrayLength) {
    const uint32_t n = (uint32_t)MAX(arrayLength, (NSUInteger)1);
    return (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) ? n * 6 : n;
}

static BOOL params_from_descriptor(MTLTextureDescriptor *d, mali_texture_params_t *p) {
    memset(p, 0, sizeof *p);
    p->mtl_pixel_format = (uint32_t)d.pixelFormat;
    p->mtl_texture_type = (uint32_t)d.textureType;
    p->width = (uint32_t)MAX(d.width, (NSUInteger)1);
    p->height = (uint32_t)MAX(d.height, (NSUInteger)1);
    p->depth = (uint32_t)MAX(d.depth, (NSUInteger)1);
    p->array_length = (uint32_t)MAX(d.arrayLength, (NSUInteger)1);
    p->mip_levels = (uint32_t)MAX(d.mipmapLevelCount, (NSUInteger)1);
    p->sample_count = (uint32_t)MAX(d.sampleCount, (NSUInteger)1);
    return p->sample_count == 1 && d.textureType != MTLTextureTypeTextureBuffer;
}

@implementation MaliTexture {
    mali_texture_t *_mt;                 /* NULL: no usable GPU layout (logged), the object still exists */
    /* CPU view: the address of byte 0 of the resource, and how many bytes follow */
    char *_cpu;
    NSUInteger _cpuBytes;
    MaliSurfaceShadow *_shadow;          /* aligned shadow of an IOSurface with unaligned rows (views share it) */
    MTLTextureUsage _usage;
    BOOL _usageKnown;
    id _keepAlive;                       /* the buffer a linear texture aliases */
    NSString *_why;
}
- (void)dealloc { if (_mt) mali_texture_free(_mt); }
- (mali_texture_t *)maliTexture { return _mt; }
- (id)maliShadow { return _shadow; }
- (id)maliBackingResource { return _shadow ? (id)_shadow->_buffer : (id)self; }
- (void)maliPrepareForGPUUse { [_shadow syncIfNeeded]; }
- (MTLTextureUsage)usage { return _usageKnown ? _usage : [super usage]; }
- (void)maliSetUsage:(MTLTextureUsage)u { _usage = u; _usageKnown = YES; }
- (void)maliRefuse:(NSString *)why { _why = why; MaliLogOnce([NSString stringWithFormat:@"texture unusable by the GPU: %@", why]); }

/* Attach the Mesa layout of a fresh texture; `data_size` is checked against the resource. */
- (BOOL)maliAttachLayout:(const mali_texture_params_t *)p ctx:(const mali_ctx_t *)ctx baseGPU:(uint64_t)base available:(uint64_t)avail {
    uint64_t need = 0;
    _mt = mali_texture_new(ctx, p, &need);
    if (!_mt) { [self maliRefuse:[NSString stringWithFormat:@"no layout for fmt %u type %u %ux%u (pitch %u)", p->mtl_pixel_format,
                                  p->mtl_texture_type, p->width, p->height, p->explicit_row_pitch]]; return NO; }
    if (need > avail) {
        mali_texture_free(_mt); _mt = NULL;
        [self maliRefuse:[NSString stringWithFormat:@"layout needs %llu bytes, resource has %llu", need, avail]];
        return NO;
    }
    mali_texture_set_base(_mt, base);
    return YES;
}

- (void)maliAttachLinearBacking:(MaliBuffer *)buffer descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)bpr
                            ctx:(const mali_ctx_t *)ctx {
    [self maliSetUsage:d.usage];
    mali_texture_params_t p;
    if (!params_from_descriptor(d, &p)) { [self maliRefuse:@"unsupported buffer texture descriptor"]; return; }
    p.explicit_row_pitch = (uint32_t)bpr;
    p.plane_offset_B = o;
    if (![self maliAttachLayout:&p ctx:ctx baseGPU:buffer.gpuAddress available:buffer.length]) return;
    _keepAlive = buffer;
    if (buffer.contents) { _cpu = buffer.contents; _cpuBytes = buffer.length; }
}

- (void)maliAttachShadow:(MaliSurfaceShadow *)sh { _shadow = sh; }
- (void)maliSetCPU:(void *)base bytes:(NSUInteger)n { _cpu = base; _cpuBytes = base ? n : 0; }
- (void)maliInheritFrom:(MaliTexture *)parent {
    _cpu = parent->_cpu; _cpuBytes = parent->_cpuBytes; _shadow = parent->_shadow; _keepAlive = parent;
    _usage = parent.usage; _usageKnown = YES;
}
- (void)maliSetMali:(mali_texture_t *)mt { _mt = mt; }

/* ---- CPU copies ---------------------------------------------------------------------------------------------- */
- (BOOL)maliRefuseCPU:(const char *)why {
    MaliLogOnce([NSString stringWithFormat:@"texture CPU copy refused (fmt %lu type %lu): %s", (unsigned long)self.pixelFormat,
                 (unsigned long)self.textureType, why]);
    return NO;
}
/* Resolve one CPU copy to its first byte and pitches, bounds-checked against the level and the CPU-visible allocation. */
- (BOOL)maliCPURegion:(MTLRegion)r level:(NSUInteger)lvl slice:(NSUInteger)slice bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
                start:(char **)start rowBytes:(NSUInteger *)rowBytes imageBytes:(NSUInteger *)imageBytes bpp:(NSUInteger *)bppOut {
    if (!r.size.width || !r.size.height || !r.size.depth) return NO;
    mali_fmt_info_t fi;
    if (!mali_fmt_lookup((uint32_t)self.pixelFormat, &fi) || !fi.bytes_per_pixel || !(fi.flags & MALI_FMT_CPU_LINEAR))
        return [self maliRefuseCPU:"format has no linear pixel layout"];
    if (self.sampleCount > 1) return [self maliRefuseCPU:"multisample texture"];
    const NSUInteger bpp = fi.bytes_per_pixel;
    if (_shadow) {
        /* IOSurface with unaligned rows: the surface itself is plain linear 2D at the surface's pitch */
        if (lvl || slice || !_cpu) return [self maliRefuseCPU:"shadow-backed surface has one level, one slice"];
        const NSUInteger w = _shadow->_width, h = _shadow->_height, row = _shadow->_surfaceRow;
        if (r.origin.x > w || r.size.width > w - r.origin.x || r.origin.y > h || r.size.height > h - r.origin.y || r.origin.z || r.size.depth != 1)
            return [self maliRefuseCPU:"region outside the surface"];
        if (bpr < r.size.width * bpp) return [self maliRefuseCPU:"row pitch too small"];
        *start = _cpu + r.origin.y * row + r.origin.x * bpp;
        *rowBytes = row; *imageBytes = row * h; *bppOut = bpp;
        return YES;
    }
    if (!_mt) return [self maliRefuseCPU:"no GPU layout"];
    if (!_cpu) return [self maliRefuseCPU:"no CPU-visible allocation"];
    mali_subresource_t s;
    if (!mali_texture_subresource(_mt, (uint32_t)lvl, (uint32_t)slice, &s)) return [self maliRefuseCPU:"level or slice outside the texture"];
    if (r.origin.x > s.width || r.size.width > s.width - r.origin.x || r.origin.y > s.height || r.size.height > s.height - r.origin.y ||
        r.origin.z > s.depth || r.size.depth > s.depth - r.origin.z)
        return [self maliRefuseCPU:"region outside the level"];
    const NSUInteger payload = r.size.width * bpp;
    if (s.row_stride_B < s.width * bpp || bpr < payload || (r.size.depth > 1 && bpi < bpr * (r.size.height - 1) + payload))
        return [self maliRefuseCPU:"row or image pitch too small"];
    const NSUInteger image = s.depth > 1 ? (NSUInteger)s.slice_stride_B : (NSUInteger)(s.row_stride_B * s.height);
    const NSUInteger first = (NSUInteger)s.offset_B + r.origin.z * image + r.origin.y * s.row_stride_B + r.origin.x * bpp;
    const NSUInteger last = first + (r.size.depth - 1) * image + (r.size.height - 1) * s.row_stride_B + payload;
    if (last > _cpuBytes) return [self maliRefuseCPU:"subresource exceeds the CPU-visible allocation"];
    *start = _cpu + first; *rowBytes = s.row_stride_B; *imageBytes = image; *bppOut = bpp;
    return YES;
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice withBytes:(const void *)b
          bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    char *dst; NSUInteger rb, ib, bpp;
    if (!b || ![self maliCPURegion:r level:lvl slice:slice bytesPerRow:bpr bytesPerImage:bpi start:&dst rowBytes:&rb imageBytes:&ib bpp:&bpp]) return;
    for (NSUInteger z = 0; z < r.size.depth; z++)
        for (NSUInteger y = 0; y < r.size.height; y++)
            memcpy(dst + z * ib + y * rb, (const char *)b + z * bpi + y * bpr, r.size.width * bpp);
    [_shadow markDirty];       /* the surface changed without a seed change: refill the shadow before the next GPU use */
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl withBytes:(const void *)b bytesPerRow:(NSUInteger)bpr {
    [self replaceRegion:r mipmapLevel:lvl slice:0 withBytes:b bytesPerRow:bpr bytesPerImage:0];
}
- (void)getBytes:(void *)b bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice {
    char *src; NSUInteger rb, ib, bpp;
    if (!b || ![self maliCPURegion:r level:lvl slice:slice bytesPerRow:bpr bytesPerImage:bpi start:&src rowBytes:&rb imageBytes:&ib bpp:&bpp]) return;
    for (NSUInteger z = 0; z < r.size.depth; z++)
        for (NSUInteger y = 0; y < r.size.height; y++)
            memcpy((char *)b + z * bpi + y * bpr, src + z * ib + y * rb, r.size.width * bpp);
}
- (void)getBytes:(void *)b bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl {
    [self getBytes:b bytesPerRow:bpr bytesPerImage:0 fromRegion:r mipmapLevel:lvl slice:0];
}

/* ---- views ---------------------------------------------------------------------------------------------------- */
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)f {
    return [self newTextureViewWithPixelFormat:f textureType:self.textureType levels:NSMakeRange(0, self.mipmapLevelCount)
                                        slices:NSMakeRange(0, slice_count_of(self.textureType, self.arrayLength))];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)t levels:(NSRange)l slices:(NSRange)s {
    MaliTexture *v = [[MaliTexture alloc] initWithTexture:self pixelFormat:f textureType:t levels:l slices:s];
    if (!v) return nil;
    [v maliInheritFrom:self];
    if (_mt) {
        mali_texture_t *mv = mali_texture_view(_mt, (uint32_t)f, (uint32_t)t, (uint32_t)l.location, (uint32_t)l.length,
                                               (uint32_t)s.location, (uint32_t)s.length);
        if (mv) [v maliSetMali:mv];
        else [v maliRefuse:[NSString stringWithFormat:@"view fmt %lu type %lu of fmt %lu not expressible", (unsigned long)f,
                            (unsigned long)t, (unsigned long)self.pixelFormat]];
    }
    return v;
}
@end

/* ---- creation ---------------------------------------------------------------------------------------------- */
id<MTLTexture> MaliNewTexture(id<MTLDevice> dev, MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane) {
    MaliMTLDevice *mdev = (MaliMTLDevice *)dev;
    const mali_ctx_t *ctx = [mdev maliCtx];
    mali_texture_params_t p;
    const BOOL ok = d && d.width && d.height && params_from_descriptor(d, &p);
    mali_new_resource_args_t a;

    if (s) {
        args_init(&a, MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE, (uint32_t)d.pixelFormat);
        MaliTexture *t = [[MaliTexture alloc] initWithDevice:dev descriptor:d iosurface:s plane:(unsigned)plane field:0 args:&a argsSize:sizeof a];
        if (!t) return nil;
        [t maliSetUsage:d.usage];
        if (!ok) { [t maliRefuse:@"unsupported IOSurface texture descriptor"]; return t; }

        const size_t planes = IOSurfaceGetPlaneCount(s), alloc = IOSurfaceGetAllocSize(s);
        if ((planes && plane >= planes) || (!planes && plane)) { [t maliRefuse:@"IOSurface plane out of range"]; return t; }
        char *base = IOSurfaceGetBaseAddress(s);
        char *pbase = planes ? IOSurfaceGetBaseAddressOfPlane(s, plane) : base;
        const size_t row = planes ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s);
        const size_t bpe = planes ? IOSurfaceGetBytesPerElementOfPlane(s, plane) : IOSurfaceGetBytesPerElement(s);
        const size_t sw = planes ? IOSurfaceGetWidthOfPlane(s, plane) : IOSurfaceGetWidth(s);
        const size_t sh = planes ? IOSurfaceGetHeightOfPlane(s, plane) : IOSurfaceGetHeight(s);
        mali_fmt_info_t fi;
        if (!mali_fmt_lookup(p.mtl_pixel_format, &fi) || !fi.bytes_per_pixel) { [t maliRefuse:@"IOSurface format has no linear layout"]; return t; }
        if (bpe != fi.bytes_per_pixel || sw < d.width || sh < d.height) {
            [t maliRefuse:[NSString stringWithFormat:@"IOSurface element %zu B, %zux%zu does not fit fmt %lu %lux%lu", bpe, sw, sh,
                           (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height]];
            return t;
        }
        if (base && pbase >= base && (size_t)(pbase - base) < alloc) [t maliSetCPU:base bytes:alloc];
        const uint64_t plane_off = (base && pbase >= base) ? (uint64_t)(pbase - base) : 0;
        /* rows that are not 64-byte multiples (CoreImage's 1,120-byte RGBA16F rows): aligned shadow, single-plane 2D only */
        if ((row & 63) && !plane_off && planes <= 1 && d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 &&
            d.sampleCount == 1 && d.arrayLength == 1 && d.depth == 1) {
            MaliSurfaceShadow *sh2 = shadow_for_surface(dev, s, fi.bytes_per_pixel);
            if (!sh2) { [t maliRefuse:@"cannot create an aligned shadow for the IOSurface"]; return t; }
            [t maliAttachShadow:sh2];
            p.explicit_row_pitch = (uint32_t)sh2->_rowBytes;
            [t maliAttachLayout:&p ctx:ctx baseGPU:[(IOGPUMetalResource *)sh2->_buffer gpuAddress] available:sh2->_buffer.length];
            return t;
        }
        p.explicit_row_pitch = (uint32_t)row;
        p.plane_offset_B = plane_off;
        [t maliAttachLayout:&p ctx:ctx baseGPU:t.gpuAddress available:alloc];
        return t;
    }

    if (!ok) { MaliLogOnce([NSString stringWithFormat:@"texture refused: fmt %lu type %lu %lux%lu samples %lu", (unsigned long)d.pixelFormat,
                            (unsigned long)d.textureType, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.sampleCount]); return nil; }
    uint64_t need = 0;
    mali_texture_t *probe = mali_texture_new(ctx, &p, &need);
    if (!probe) { MaliLogOnce([NSString stringWithFormat:@"texture has no Mali layout: fmt %lu type %lu %lux%lu", (unsigned long)d.pixelFormat,
                               (unsigned long)d.textureType, (unsigned long)d.width, (unsigned long)d.height]); return nil; }
    mali_subresource_t s0;
    const uint32_t row0 = mali_texture_subresource(probe, 0, 0, &s0) ? s0.row_stride_B : 0;
    mali_texture_free(probe);
    const NSUInteger sz = align_up(MAX((NSUInteger)need, (NSUInteger)1), PAGE);
    args_init(&a, bo_flags_for(d.resourceOptions, YES), (uint32_t)d.pixelFormat);
    MaliTexture *t = [[MaliTexture alloc] initWithDevice:dev descriptor:d sysMemSize:sz sysMemRowBytes:row0 vidMemSize:0 vidMemRowBytes:0
                                                    args:&a argsSize:sizeof a];
    if (!t) return nil;
    [t maliSetUsage:d.usage];
    [t maliAttachLayout:&p ctx:ctx baseGPU:t.gpuAddress available:[t resourceSize]];
    void *va = [t virtualAddress];
    if (va) [t maliSetCPU:va bytes:(NSUInteger)MIN((uint64_t)[t resourceSize], (uint64_t)sz)];
    return t;
}

id<MTLTexture> MaliNewTextureNoCopy(id<MTLDevice> dev, MTLTextureDescriptor *d, void *bytes, NSUInteger len,
                                    void (^dealloc)(void *, NSUInteger)) {
    MaliMTLDevice *mdev = (MaliMTLDevice *)dev;
    mali_texture_params_t p;
    if (!d || !d.width || !d.height || !params_from_descriptor(d, &p)) return nil;
    uint64_t need = 0;
    mali_texture_t *probe = mali_texture_new([mdev maliCtx], &p, &need);
    if (!probe) return nil;
    mali_subresource_t s0;
    const uint32_t row0 = mali_texture_subresource(probe, 0, 0, &s0) ? s0.row_stride_B : 0;
    mali_texture_free(probe);
    mali_new_resource_args_t a;
    args_init(&a, MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE, (uint32_t)d.pixelFormat);
    MaliTexture *t = [[MaliTexture alloc] initWithDevice:dev descriptor:d sysMemPointer:bytes sysMemSize:len sysMemLength:len
                                          sysMemRowBytes:row0 args:&a argsSize:sizeof a deallocator:dealloc];
    if (!t) return nil;
    [t maliSetUsage:d.usage];
    [t maliSetCPU:bytes bytes:len];
    [t maliAttachLayout:&p ctx:[mdev maliCtx] baseGPU:t.gpuAddress available:len];
    return t;
}

uint64_t MaliTextureGPUBase(id<MTLTexture> t) {
    mali_texture_t *mt = [(MaliTexture *)t isKindOfClass:[MaliTexture class]] ? [(MaliTexture *)t maliTexture] : NULL;
    return mt ? mali_texture_base(mt) : 0;
}

id MaliGPUBacking(id resource) {
    if (![resource isKindOfClass:[MaliTexture class]]) return resource;
    MaliTexture *t = resource;
    [t maliPrepareForGPUUse];
    return [t maliBackingResource];
}

/* ====================================================================== *
 * Sampler state
 * ====================================================================== */
@implementation MaliSamplerState {
    mali_sampler_params_t _p;
    uint32_t _desc[2][8];                /* [0] float texture, [1] integer texture (border words differ) */
}
- (instancetype)initWithDevice:(id<MTLDevice>)device samplerDescriptor:(MTLSamplerDescriptor *)d {
    if ((self = [super initWithDevice:device samplerDescriptor:d])) {
        memset(&_p, 0, sizeof _p);
        _p.min_filter = (uint32_t)d.minFilter;
        _p.mag_filter = (uint32_t)d.magFilter;
        _p.mip_filter = (uint32_t)d.mipFilter;
        _p.addr_s = (uint32_t)d.sAddressMode;
        _p.addr_t = (uint32_t)d.tAddressMode;
        _p.addr_r = (uint32_t)d.rAddressMode;
        _p.border = (uint32_t)d.borderColor;
        _p.max_anisotropy = (uint32_t)MAX((NSUInteger)1, MIN((NSUInteger)16, d.maxAnisotropy));
        _p.compare = d.compareFunction == MTLCompareFunctionNever ? 8u : (uint32_t)d.compareFunction;   /* 8 = no comparison */
        _p.normalized_coords = d.normalizedCoordinates;
        _p.lod_min = d.lodMinClamp;
        _p.lod_max = d.lodMaxClamp;
        _p.lod_bias = 0.0f;
        if ([d respondsToSelector:NSSelectorFromString(@"lodBias")]) _p.lod_bias = [[(id)d valueForKey:@"lodBias"] floatValue];   /* newer SDKs only */
        mali_emit_sampler(&_p, false, _desc[0]);
        mali_emit_sampler(&_p, true, _desc[1]);
    }
    return self;
}
- (const mali_sampler_params_t *)maliParams { return &_p; }
- (const uint32_t *)maliDesc:(BOOL)integerTexture { return _desc[integerTexture ? 1 : 0]; }
@end

/* ====================================================================== *
 * Depth-stencil state
 * ====================================================================== */
static void face_from(MTLStencilDescriptor *s, mali_stencil_face_t *f) {
    f->compare = s ? (uint32_t)s.stencilCompareFunction : (uint32_t)MTLCompareFunctionAlways;
    f->fail_op = s ? (uint32_t)s.stencilFailureOperation : 0;
    f->depth_fail_op = s ? (uint32_t)s.depthFailureOperation : 0;
    f->pass_op = s ? (uint32_t)s.depthStencilPassOperation : 0;
    f->read_mask = s ? s.readMask : 0xffffffffu;
    f->write_mask = s ? s.writeMask : 0xffffffffu;
}
@implementation MaliDepthStencilState {
    mali_zs_state_t _zs;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device depthStencilDescriptor:(MTLDepthStencilDescriptor *)d {
    if ((self = [super initWithDevice:device depthStencilDescriptor:d])) {
        memset(&_zs, 0, sizeof _zs);
        _zs.depth_compare = d ? (uint32_t)d.depthCompareFunction : (uint32_t)MTLCompareFunctionAlways;
        _zs.depth_write = d ? d.depthWriteEnabled : NO;
        face_from(d.frontFaceStencil, &_zs.front);
        face_from(d.backFaceStencil, &_zs.back);
        const mali_stencil_face_t *f[2] = {&_zs.front, &_zs.back};
        for (int i = 0; i < 2; i++)
            if (f[i]->compare != (uint32_t)MTLCompareFunctionAlways || f[i]->fail_op || f[i]->depth_fail_op || f[i]->pass_op) _zs.stencil_enable = true;
    }
    return self;
}
- (const mali_zs_state_t *)maliZS { return &_zs; }
@end
