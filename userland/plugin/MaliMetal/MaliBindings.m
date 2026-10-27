/*
 * MaliBindings.m
 * Intended repo path: plugin/MaliMetal/MaliBindings.m
 */
#import "MaliBindings.h"

void MaliBindsInit(mali_stage_binds_t *b) {
    memset(b, 0, sizeof *b);
    /* the dummy sampler (T3[16], VH-ABI 1) is read by every stage: nearest, clamp to edge, no anisotropy */
    mali_sampler_params_t p;
    memset(&p, 0, sizeof p);
    p.max_anisotropy = 1;
    p.compare = 8;
    p.normalized_coords = true;
    p.lod_max = 1000.0f;
    mali_emit_sampler(&p, false, b->samp_desc[MALI_DUMMY_SAMPLER]);
    b->serial = 1;
}

void MaliBindBuffer(mali_stage_binds_t *b, MaliCommandBuffer *cb, NSUInteger i, id<MTLBuffer> buf, NSUInteger offset) {
    if (i >= MALI_MAX_BUFFERS) { MaliLogOnce([NSString stringWithFormat:@"buffer index %lu out of range", (unsigned long)i]); return; }
    b->serial++;
    if (!buf) { b->buf_gpu[i] = 0; b->buf_size[i] = 0; return; }
    const NSUInteger len = buf.length;
    if (offset > len) { MaliLogOnce(@"buffer offset beyond the buffer"); b->buf_gpu[i] = 0; b->buf_size[i] = 0; return; }
    b->buf_gpu[i] = [(IOGPUMetalBuffer *)buf gpuAddress] + offset;
    b->buf_size[i] = len - offset;
    [cb maliUseResource:buf usage:MTLResourceUsageRead | MTLResourceUsageWrite];
}
void MaliBindBufferOffset(mali_stage_binds_t *b, NSUInteger i, NSUInteger offset, id<MTLBuffer> bound) {
    if (i >= MALI_MAX_BUFFERS || !bound) return;
    const NSUInteger len = bound.length;
    if (offset > len) { MaliLogOnce(@"buffer offset beyond the buffer"); return; }
    b->serial++;
    b->buf_gpu[i] = [(IOGPUMetalBuffer *)bound gpuAddress] + offset;
    b->buf_size[i] = len - offset;
}
void MaliBindBytes(mali_stage_binds_t *b, MaliCommandBuffer *cb, NSUInteger i, const void *bytes, NSUInteger length) {
    if (i >= MALI_MAX_BUFFERS) { MaliLogOnce([NSString stringWithFormat:@"bytes index %lu out of range", (unsigned long)i]); return; }
    mali_gpu_ptr_t m = mali_arena_alloc([cb maliArena], (uint32_t)MAX(length, (NSUInteger)16), 64);
    if (!m.cpu) { [cb maliFail:@"descriptor arena exhausted (setBytes)"]; return; }
    memcpy(m.cpu, bytes, length);
    b->serial++;
    b->buf_gpu[i] = m.gpu;
    b->buf_size[i] = length;
}

void MaliBindTexture(mali_stage_binds_t *b, MaliCommandBuffer *cb, NSUInteger i, id<MTLTexture> tex) {
    if (i >= MALI_MAX_TEXTURES) { MaliLogOnce([NSString stringWithFormat:@"texture index %lu out of range", (unsigned long)i]); return; }
    b->serial++;
    b->tex_mask &= ~(1u << i);
    b->stex_mask &= ~(1u << i);
    if (!tex) return;
    MaliTexture *t = [tex isKindOfClass:[MaliTexture class]] ? (MaliTexture *)tex : nil;
    mali_texture_t *mt = [t maliTexture];
    if (!mt) { MaliLogOnce([NSString stringWithFormat:@"texture(%lu) bound without a Mali layout: it reads as null", (unsigned long)i]); return; }
    const MTLTextureUsage usage = tex.usage;
    mali_arena_t *a = [cb maliArena];
    MTLResourceUsage use = 0;
    if (!usage || (usage & MTLTextureUsageShaderRead) || (usage & MTLTextureUsageRenderTarget)) {
        if (mali_emit_sampled_texture(mt, a, b->tex_desc[i])) { b->tex_mask |= 1u << i; use |= MTLResourceUsageRead; }
        else MaliLogOnce([NSString stringWithFormat:@"texture(%lu) fmt %lu: no sampled descriptor", (unsigned long)i, (unsigned long)tex.pixelFormat]);
    }
    if (usage & MTLTextureUsageShaderWrite) {
        mali_fmt_info_t fi;
        if (mali_fmt_lookup((uint32_t)tex.pixelFormat, &fi) && (fi.flags & MALI_FMT_STORAGE) && mali_emit_storage_texture(mt, a, b->stex_desc[i])) {
            b->stex_mask |= 1u << i;
            use |= MTLResourceUsageWrite;
        } else MaliLogOnce([NSString stringWithFormat:@"texture(%lu) fmt %lu: no storage descriptor", (unsigned long)i, (unsigned long)tex.pixelFormat]);
    }
    if (use) [cb maliUseResource:tex usage:use];
}

void MaliBindSampler(mali_stage_binds_t *b, NSUInteger i, id<MTLSamplerState> s) {
    if (i >= MALI_MAX_SAMPLERS) { MaliLogOnce([NSString stringWithFormat:@"sampler index %lu out of range", (unsigned long)i]); return; }
    b->serial++;
    if (![s isKindOfClass:[MaliSamplerState class]]) { b->samp_mask &= ~(1u << i); return; }
    memcpy(b->samp_desc[i], [(MaliSamplerState *)s maliDesc:NO], 32);
    b->samp_mask |= 1u << i;
}

void MaliApplyConstSamplers(mali_stage_binds_t *b, const MaliConstSampler *cs, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        if (cs[k].slot >= MALI_MAX_SAMPLERS) {            /* slot 16 is the dummy sampler: never overwritten */
            MaliLogOnce([NSString stringWithFormat:@"constexpr sampler slot %u out of range", cs[k].slot]);
            continue;
        }
        const uint32_t bit = 1u << cs[k].slot;
        if ((b->samp_mask & bit) && !memcmp(b->samp_desc[cs[k].slot], cs[k].desc, 32)) continue;
        memcpy(b->samp_desc[cs[k].slot], cs[k].desc, 32);
        b->samp_mask |= bit;
        b->serial++;
    }
}
