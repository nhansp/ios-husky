/*
 * MaliResourceUsage.m
 * Intended repo path: plugin/MaliMetal/MaliResourceUsage.m
 */
#import "MaliResourceUsage.h"
#import <objc/runtime.h>
#import <dlfcn.h>

/* IOGPUResourceInfo as IOGPU.framework lays it out (spec 04 D14, runtime ivar type encoding
 * {IOGPUResourceInfo="iosurface"^{__IOSurface}"resourceSize"b56"iosurfaceField"b8"resourceID"I}). */
struct MaliResourceInfo {
    void    *surface;
    uint64_t sizeAndField;       /* bits 55:0 size, 63:56 iosurface field */
    uint32_t resourceID, padding;
};
_Static_assert(sizeof(struct MaliResourceInfo) == 24, "IOGPUResourceInfo ABI");

struct MaliResourceUse {
    struct MaliResourceInfo info;
    uint32_t usage, reserved;
};

static void (*gRetainResource)(id, id);                                           /* MTLResourceListAddResource */
static uint32_t (*gAddResource)(void *, const struct MaliResourceInfo *, uint32_t);   /* IOGPUResourceListAddResource */
static ptrdiff_t gInfoOffset;

static void MaliLoadResourceABI(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Ivar iv = class_getInstanceVariable([IOGPUMetalResource class], "_res");
        const char *type = iv ? ivar_getTypeEncoding(iv) : NULL;
        if (!type || !strstr(type, "IOGPUResourceInfo")) return;
        /* _res is a struct whose first member is a 32-byte vendor union, then the info block (D14). */
        const ptrdiff_t off = ivar_getOffset(iv) + 32;
        if (off < 32 || (size_t)off + sizeof(struct MaliResourceInfo) > class_getInstanceSize([IOGPUMetalResource class])) return;
        gInfoOffset = off;
        gRetainResource = (void (*)(id, id))dlsym(RTLD_DEFAULT, "MTLResourceListAddResource");
        gAddResource = (uint32_t (*)(void *, const struct MaliResourceInfo *, uint32_t))dlsym(RTLD_DEFAULT, "IOGPUResourceListAddResource");
    });
    if (!gInfoOffset || !gRetainResource || !gAddResource)
        [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: unsupported native resource-list ABI"];
}

@implementation MaliResourceUsage {
    id _device, _retainedList;
    NSMutableData *_records;                         /* struct MaliResourceUse[] */
    NSMutableDictionary<NSNumber *, NSNumber *> *_index;   /* resourceID -> record index */
    BOOL _sealed;
}
- (instancetype)initWithDevice:(id)device retainedList:(id)retainedList {
    if (!device) [NSException raise:NSInvalidArgumentException format:@"MaliMetal: missing resource device"];
    MaliLoadResourceABI();
    if ((self = [super init])) {
        _device = device;
        _retainedList = retainedList;
        _records = [NSMutableData data];
        _index = [NSMutableDictionary dictionary];
    }
    return self;
}
- (NSUInteger)count { return _records.length / sizeof(struct MaliResourceUse); }

- (void)recordResource:(id)resource usage:(MTLResourceUsage)usage {
    if (_sealed) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: resource encoding already ended"];
    const MTLResourceUsage supported = MTLResourceUsageRead | MTLResourceUsageWrite | (1u << 2);
    if (![resource isKindOfClass:[IOGPUMetalResource class]] || !usage || (usage & ~supported))
        [NSException raise:NSInvalidArgumentException format:@"MaliMetal: invalid resource or usage"];
    IOGPUMetalResource *native = resource;
    if ([native device] != _device) [NSException raise:NSInvalidArgumentException format:@"MaliMetal: resource of a different device"];

    const uint32_t rid = [native resourceID];
    NSNumber *at = _index[@(rid)];
    if (at) {                                          /* already recorded: merge the usage */
        struct MaliResourceUse *r = (struct MaliResourceUse *)_records.mutableBytes + at.unsignedIntegerValue;
        r->usage |= (uint32_t)usage;
        return;
    }
    struct MaliResourceUse rec;
    memset(&rec, 0, sizeof rec);
    memcpy(&rec.info, (const char *)(__bridge const void *)native + gInfoOffset, sizeof rec.info);
    const uint64_t size = rec.info.sizeAndField & 0x00ffffffffffffffull;
    if (!size || size != [native resourceSize] || rec.info.resourceID != rid)
        [NSException raise:NSInvalidArgumentException format:@"MaliMetal: resource information empty or inconsistent"];
    rec.usage = (uint32_t)usage;
    if (_retainedList) gRetainResource(_retainedList, resource);   /* retained-reference command buffers keep it alive */
    _index[@(rid)] = @(self.count);
    [_records appendBytes:&rec length:sizeof rec];
}

- (NSData *)resourceIDs {
    const struct MaliResourceUse *r = _records.bytes;
    const NSUInteger n = self.count;
    NSMutableData *ids = [NSMutableData dataWithLength:n * sizeof(uint32_t)];
    uint32_t *out = ids.mutableBytes;
    for (NSUInteger i = 0; i < n; i++) out[i] = r[i].info.resourceID;
    return ids;
}

- (void)encodeIntoIOGPUList:(void *)list {
    if (_sealed) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: resource publication cannot be repeated"];
    _sealed = YES;
    if (!list) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: missing native resource list"];
    const struct MaliResourceUse *r = _records.bytes;
    for (NSUInteger i = 0; i < self.count; i++)
        if (gAddResource(list, &r[i].info, r[i].usage) == UINT32_MAX)
            [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: segment resource threshold exceeded"];
}
@end
