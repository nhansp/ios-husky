/*
 * MaliResourceUsage.h - the resources a command buffer touches, published to IOGPU's segment resource list
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliResourceUsage.h
 *
 * -[IOGPUMetalCommandBuffer beginSegment:] resets IOGPU's resource list, so uses are recorded here while
 * encoding and written into the list after beginSegment and before endCurrentSegment at commit
 * (spec 04 D14: IOGPUResourceListAddResource, resource info read from the `_res` ivar at +32).
 * The kernel cleans/invalidates every listed resource around the submission (spec 03 section 5.4).
 */
#pragma once
#import "MaliMetalSPI.h"

NS_ASSUME_NONNULL_BEGIN

@interface MaliResourceUsage : NSObject
- (instancetype)initWithDevice:(id)device retainedList:(nullable id)retainedList;
/* usage: MTLResourceUsageRead / Write (bit 2 legacy Sample is accepted). Repeated records of one resource merge. */
- (void)recordResource:(id)resource usage:(MTLResourceUsage)usage;
/* Only after beginSegment and before endCurrentSegment. Raises if IOGPU's per-segment byte budget is exceeded. */
- (void)encodeIntoIOGPUList:(void *)list;
- (NSUInteger)count;
/* IOGPU resource ids in record order (the kernel finds queue-state mappings by id). */
- (NSData *)resourceIDs;
@end

NS_ASSUME_NONNULL_END
