/*
 * MaliBindings.h - Metal binding calls -> mali_stage_binds_t (shared by the render, compute and blit encoders)
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliBindings.h
 *
 * Every function updates `b->serial` when it changes something, so the command builder's SRT/FAU caches (mali_cs.c)
 * stay valid across draws that bind nothing new.
 */
#pragma once
#import "MaliMTLObjectsPrivate.h"

NS_ASSUME_NONNULL_BEGIN

void MaliBindsInit(mali_stage_binds_t *b);
/* bound buffer: VA incl. offset, remaining size; also lists the resource for the segment */
void MaliBindBuffer(mali_stage_binds_t *b, MaliCommandBuffer *cb, NSUInteger index, id<MTLBuffer> _Nullable buf, NSUInteger offset);
void MaliBindBufferOffset(mali_stage_binds_t *b, NSUInteger index, NSUInteger offset, id<MTLBuffer> _Nullable bound);
/* setBytes: copied into the command buffer's arena (the app may reuse its memory immediately) */
void MaliBindBytes(mali_stage_binds_t *b, MaliCommandBuffer *cb, NSUInteger index, const void *bytes, NSUInteger length);
/* sampled and (if the texture allows shader writes) storage descriptors for slot `index` */
void MaliBindTexture(mali_stage_binds_t *b, MaliCommandBuffer *cb, NSUInteger index, id<MTLTexture> _Nullable tex);
void MaliBindSampler(mali_stage_binds_t *b, NSUInteger index, id<MTLSamplerState> _Nullable s);
/* constexpr samplers of the bound program: written into their T3 slots */
void MaliApplyConstSamplers(mali_stage_binds_t *b, const MaliConstSampler *cs, uint32_t n);

NS_ASSUME_NONNULL_END
