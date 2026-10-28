/* SPDX-License-Identifier: MIT */
// air2ir3.h - C++ front-end API shared by the air2valhall library (air2vh_api.h).
// Adapted from air2ir3/src/air2ir3.h with Valhall edits (spec 07 section 3.3).
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "nir.h"

// Every UNSUPPORTED/ERROR is reported by throwing this (the library catches it; the CLI prints
// "air2valhall: UNSUPPORTED/ERROR: <what>" and exits 2).  Nothing in the front end calls exit().
struct AirError : std::runtime_error { using std::runtime_error::runtime_error; };

// Colour-attachment FORMAT CLASS (CACHE-FORMAT.md 2.2, FORMATS.md).  0 = native.
enum AirFormatClass : uint8_t {
  AIR_FMT_NATIVE = 0,
  AIR_FMT_XR10_BGR32 = 1,      // MTLPixelFormatBGR10_XR (554): view B10G10R10A2_UNORM, affine encode/decode
  AIR_FMT_XR10_BGRA64 = 2,     // MTLPixelFormatBGRA10_XR (552): view R16G16B16A16_UNORM (BGRA swap), affine
  AIR_FMT_XR10_RGB10A8_2P = 3, // MTLPixelFormat 550/551 (two planes): conversion blit, refused by the compiler
  AIR_FMT_YCBCR10_PACKED = 4,  // 508/510: conversion blit (sampling) / refused (render target)
  AIR_FMT_XR10_SRGB = 5,       // 553/555: conversion blit, refused by the compiler
  AIR_FMT_CLASS_COUNT
};
const char *air_format_class_name(unsigned c);

struct AirBlend {
  bool enabled = false;
  uint8_t rgbOp = 0, alphaOp = 0;               // MTLBlendOperationAdd = 0
  uint8_t srcRGB = 1, dstRGB = 0, srcA = 1, dstA = 0;   // MTLBlendFactorOne = 1, Zero = 0
};

struct AirOptions {
  std::map<int, std::vector<double>> fc;               // function constants by index
  std::map<std::string, std::vector<double>> fcByName; // function constants by name
  std::string entry;                                   // entry point name (multi-function .air files)
  int expectStage = -1;                                // MESA_SHADER_* the caller expects, or -1
  uint8_t rtClass[8] = {0, 0, 0, 0, 0, 0, 0, 0};      // per colour attachment (fragment only)
  AirBlend blend[8];                                   // per colour attachment (used for lowered classes)
  uint8_t texClass[128] = {0};                         // draw-time format class of each T# (0 = native)
  int testMaxUboIndex = -1;
  bool safeConstlen = false;
  int testUboSlots = -1;
  int testPvtmemBudgetMiB = -1;
  bool nonuniformDispatch = false;                     // AIR2VH_F_NONUNIFORM_DISPATCH (spec 07 6.12)
};

struct AirBinding {            // one resource binding (spec 07 section 6 / 8.3)
  std::string kind;            // "ubo" | "device_buffer" | "texture" | "storage_texture" | "sampler" | "constexpr_sampler" | "fb_fetch" | "threadgroup_buffer"
  int metal_index = -1;        // Metal [[buffer/texture/sampler/threadgroup(n)]] or render-target index for fb_fetch
  int slot = -1;               // UBO index / T# / S# / byte offset in the driver table
  std::string arg_name, type_name, access;
  bool present = true;         // Metal's notion: the argument exists (its function-constant predicate is true)
  bool used = true;            // the specialised code reads it
  int64_t minimum_bytes = -1;  // AIR air.buffer_size; -1 means no declared bound
  uint64_t sampler_word = 0;   // constexpr samplers: AIR sampler word 0
  uint64_t unique_id = 0;      // constexpr samplers: stable id (BINDING-ABI.md 3.5)
  uint32_t a8xx_desc[4] = {0, 0, 0, 0};
  uint32_t vh_desc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t vh_int_border[4] = {0, 0, 0, 0};
};
struct AirVarying { std::string name; int slot = -1; unsigned comps = 0, bits = 32; std::string interp, sampling, base; };
struct AirAttribute { int index = -1; unsigned comps = 0, bits = 32; std::string base; };
struct AirRenderTarget { int index = -1; unsigned comps = 0, bits = 32; std::string base; bool fb_fetch = false, written = false; };
struct AirFc { int index; std::string type, name; std::vector<double> value; bool defined; bool tainted_predicate = false; };
struct AirRtLowering {
  int index = -1; unsigned cls = 0;
  AirBlend api, hw;
  bool dual_source = false;
  bool clear_alpha_write = false;
  bool blend_in_shader = false;
};

struct AirFrontendResult {
  nir_shader *nir = nullptr;
  mesa_shader_stage stage = MESA_SHADER_NONE;
  std::string function, triple, report;
  std::vector<AirBinding> bindings;
  std::vector<AirVarying> varyings;       // VS outputs / FS inputs, by name
  std::vector<AirAttribute> attributes;   // VS stage_in attributes
  std::vector<AirRenderTarget> render_targets;
  std::vector<AirRtLowering> rt_lowering;
  std::vector<AirFc> fcs;
  std::vector<std::string> sysvals;       // position/vertex_id/instance_id/front_facing/point_coord/...
  std::vector<std::string> warnings;
  std::vector<int> requires_no_attachment;
  uint8_t tex_class[128] = {0};
  uint16_t declared_textures = 0;         // bit n (n < 16): function declares texture(n)
  int declared_ubo_max = -1;              // highest constant-buffer index the function declares
  int driver_table_ubo = 0;               // marker UBO index = declared_ubo_max + 1 (rewritten to FAU by vh_lower_bindings)
  uint32_t constexpr_sampler_mask = 0;
  unsigned constant_data_bytes = 0, shared_bytes = 0;
  bool writes_depth = false, writes_point_size = false, uses_discard = false, uses_fb_fetch = false;
};

// Parse, specialise and translate one AIR function to NIR.  The NIR still has IO variables (not
// yet lowered for any backend).  Throws AirError.  Each call uses its own LLVMContext; the NIR
// (ralloc'd) is owned by the caller.
AirFrontendResult air_frontend(const char *path, const AirOptions &o, const nir_shader_compiler_options *nopts);
AirFrontendResult air_frontend_mem(const void *bitcode, size_t size, const AirOptions &o, const nir_shader_compiler_options *nopts);
std::string air_reflection_json(const AirFrontendResult &r);
