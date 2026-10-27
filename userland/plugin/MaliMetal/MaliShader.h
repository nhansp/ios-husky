/*
 * MaliShader.h - AIR input parsing and the air2valhall compiler front for the plug-in
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliShader.h
 *
 * Flow (spec 05 section 5.2, spec 04 D8): Metal's own MTLCompiler runs with pluginPath
 * "simulatorDummyPlugin", specialises the function (function constants, vertex-fetch function) and hands
 * back an MTLB container holding AIR. MaliCompilerInput extracts and verifies that AIR module;
 * MaliShaderCompiler runs air2vh_compile on it (contract air2vh_api.h) and keeps the result.
 */
#pragma once
#import <Foundation/Foundation.h>
#include "air2vh_api.h"
#include "mali_backend_ops.h"

NS_ASSUME_NONNULL_BEGIN

/* A constexpr sampler the shader embeds: the compiler's sidecar lists the T3 slot and the finished SAMPLER
 * descriptor words (spec 07 8.3 "constant_samplers"). */
typedef struct MaliConstSampler {
    uint32_t slot;               /* T3 index                                         */
    uint32_t desc[8];            /* mali_sampler[8]                                  */
    uint32_t int_border[4];      /* mali_sampler_int_border[4]: words 4..7 for an integer texture */
} MaliConstSampler;
#define MALI_MAX_CONST_SAMPLERS 16u

/* One unambiguous stage out of the MTLCompiler service output (or one of the plug-in's built-in AIR modules). */
@interface MaliCompilerInput : NSObject
@property (nonatomic, readonly, strong) NSData *airModule;
@property (nonatomic, readonly, strong) NSString *entry;
@property (nonatomic, readonly, strong) NSString *airSHA256;
@property (nonatomic, readonly) NSUInteger stage;        /* MTLB TYPE: 0 vertex, 1 fragment, 2 kernel */
+ (nullable instancetype)inputWithCompilerOutput:(id)output stage:(NSUInteger)stage
                                           entry:(nullable NSString *)entry error:(NSString *_Nullable *_Nullable)error;
/* A raw AIR module (bitcode or wrapper) that is not inside an MTLB container. */
+ (nullable instancetype)inputWithAIR:(NSData *)air entry:(NSString *)entry stage:(NSUInteger)stage
                                error:(NSString *_Nullable *_Nullable)error;
@end

/* The finished shader: Valhall code + flat program struct + (VS) the varying layout it produces. */
@interface MaliShaderProgram : NSObject {
@public
    air2vh_program   prog;                 /* copy of the compiler's flat struct               */
    air2vh_vs_layout layout;               /* VS: layout this program produces; .blob -> layoutBlob */
    MaliConstSampler csamp[MALI_MAX_CONST_SAMPLERS];
    uint32_t         csamp_count;
}
@property (nonatomic, readonly) NSData *code;
@property (nonatomic, readonly) NSData *layoutBlob;
/* The program followed by its extension block (air2vh_program_get_ext), nil when the compiler gave none. */
@property (nonatomic, readonly, nullable) NSData *programWithExt;
@property (nonatomic, readonly) NSString *key;
@property (nonatomic, readonly) NSString *entry;
@end

@interface MaliShaderCompiler : NSObject
- (nullable instancetype)initWithProps:(const mali_dev_props_t *)props error:(NSString *_Nullable *_Nullable)error;
/* flags: enum air2vh_flags. vs: the vertex program whose layout a fragment program links against (tier B). */
- (nullable MaliShaderProgram *)compile:(MaliCompilerInput *)input flags:(uint32_t)flags
                               vsLayout:(nullable MaliShaderProgram *)vs error:(NSString *_Nullable *_Nullable)error;
- (nullable MaliShaderProgram *)compileBlendShaderKey:(NSData *)key error:(NSString *_Nullable *_Nullable)error;
@property (nonatomic, readonly) NSString *compilerID;
@end

NS_ASSUME_NONNULL_END
