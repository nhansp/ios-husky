// SPDX-License-Identifier: MIT
// Front-end entry points: entry lookup, function-constant resolution, air_frontend_mem, reflection JSON.
// Part of src/vh_frontend.cpp's translation unit, #included from it in order.
#ifdef AIR2VH_FRONTEND_TU

// ---------------------------------------------------------------------- front end --
const char *air_format_class_name(unsigned c) {
  static const char *n[] = {"native", "xr10_bgr32", "xr10_bgra64", "xr10_rgb10a8_2p", "ycbcr10_packed", "xr10_srgb"};
  return c < AIR_FMT_CLASS_COUNT ? n[c] : "invalid";
}

AirFrontendResult air_frontend(const char *path, const AirOptions &o, const nir_shader_compiler_options *nopts) {
  auto buf = MemoryBuffer::getFile(path);
  if (!buf) die(std::string("cannot read input ") + path);
  return air_frontend_mem((*buf)->getBufferStart(), (*buf)->getBufferSize(), o, nopts);
}

struct AirEntry { Function *F = nullptr; mesa_shader_stage stage = MESA_SHADER_NONE; const MDNode *ins = nullptr, *outs = nullptr; };
static AirEntry findEntry(Module *M, const AirOptions &o) {
  AirEntry e;
  for (auto [md, st] : {std::pair{"air.vertex", MESA_SHADER_VERTEX}, {"air.fragment", MESA_SHADER_FRAGMENT}, {"air.kernel", MESA_SHADER_COMPUTE}}) {
    if (NamedMDNode *N = M->getNamedMetadata(md)) {
      for (MDNode *E : N->operands()) {
        Function *C = cast<Function>(cast<ValueAsMetadata>(E->getOperand(0))->getValue());
        if (!o.entry.empty() ? C->getName() != o.entry : (e.F != nullptr || (o.expectStage >= 0 && (int)st != o.expectStage))) continue;
        e.F = C; e.stage = st;
        if (st == MESA_SHADER_COMPUTE) { e.ins = cast<MDNode>(E->getOperand(2)); e.outs = nullptr; }
        else { e.outs = cast<MDNode>(E->getOperand(1)); e.ins = cast<MDNode>(E->getOperand(2)); }
      }
    }
  }
  if (!e.F && !o.entry.empty()) die("entry point " + o.entry + " not found");
  if (!e.F) {
    if (M->getNamedMetadata("air.visible")) die("no AIR entry point (visible/stitchable function: needs linking into a caller)");
    die("no AIR entry point");
  }
  if (o.expectStage >= 0 && o.expectStage != (int)e.stage)
    die("entry point " + e.F->getName().str() + " is a " + (e.stage == MESA_SHADER_VERTEX ? "vertex" : e.stage == MESA_SHADER_FRAGMENT ? "fragment" : "kernel") +
        " function, not the requested stage");
  return e;
}

static std::vector<AirFc> resolveFcs(const std::vector<FcDecl> &decls, const AirOptions &o, const std::string &fn,
                                     std::map<int, std::vector<double>> &fc) {
  fc = o.fc;
  for (auto &kv : o.fcByName) {
    bool found = false;
    for (auto &d : decls) if (d.name == kv.first) { fc[d.index] = kv.second; found = true; }
    if (!found) fprintf(stderr, "air2valhall: warning: function constant '%s' not declared by %s (ignored)\n", kv.first.c_str(), fn.c_str());
  }
  std::vector<AirFc> r;
  for (auto &d : decls) {
    AirFc f; f.index = d.index; f.type = d.type; f.name = d.name; f.defined = fc.count(d.index) != 0;
    if (f.defined) fc[d.index] = fcCanon(d.type, fc.at(d.index));
    f.value = f.defined ? fc.at(d.index) : std::vector<double>{};
    r.push_back(f);
  }
  return r;
}

static uint16_t declaredTextures(const std::vector<ArgInfo> &inArgs) {
  uint16_t m = 0;
  for (auto &a : inArgs) if (a.kind == "air.texture" && a.location >= 0 && a.location < 16) m |= 1u << a.location;
  return m;
}
static int declaredUboMax(const std::vector<ArgInfo> &inArgs) {
  int m = -1;
  for (auto &a : inArgs) if (a.kind == "air.buffer" && a.addrspace == 2 && a.location >= 0 && a.location <= 30) m = std::max(m, a.location);
  return m;
}

static void nirValidationPolicy() {
#if defined(MESA_DEBUG) && MESA_DEBUG
  static std::once_flag once;
  std::call_once(once, [] {
    nir_process_debug_variable();
    const char *v = getenv("AIR2VH_NIR_VALIDATE");
    if (!v) v = getenv("AIR2IR3_NIR_VALIDATE");
    if (!getenv("NIR_DEBUG") && !(v && atoi(v))) nir_debug |= NIR_DEBUG_NOVALIDATE;
  });
#endif
}

AirFrontendResult air_frontend_mem(const void *bitcode, size_t size, const AirOptions &o0, const nir_shader_compiler_options *nopts) {
  nirValidationPolicy();
  LLVMContext Ctx;
  AirFrontendResult R;
  MemoryBufferRef mref(StringRef((const char *)bitcode, size), "air");
  auto modOr = parseBitcodeFile(mref, Ctx);
  if (!modOr) die("bitcode parse failed: " + toString(modOr.takeError()));
  std::unique_ptr<Module> MOwn = std::move(*modOr);
  Module *M = MOwn.get();

  AirEntry E = findEntry(M, o0);
  Function *F = E.F; mesa_shader_stage stage = E.stage; const MDNode *ins = E.ins, *outsMd = E.outs;
  R.function = F->getName().str(); R.stage = stage; R.triple = Triple(M->getTargetTriple()).str();

  std::vector<ArgInfo> inArgs, outArgs;
  for (unsigned i = 0; outsMd && i < outsMd->getNumOperands(); i++) outArgs.push_back(parseArg(cast<MDNode>(outsMd->getOperand(i)), true));
  for (unsigned i = 0; ins && i < ins->getNumOperands(); i++) inArgs.push_back(parseArg(cast<MDNode>(ins->getOperand(i)), false));
  for (auto &a : inArgs) {
    if (a.argno < 0 || (unsigned)a.argno >= F->arg_size()) die("input argument index out of range");
    if (a.kind == "air.buffer") {
      auto *PT = dyn_cast<PointerType>(F->getArg(a.argno)->getType());
      if (!PT) die("buffer argument is not a pointer");
      int actual = (int)PT->getAddressSpace();
      if (a.addrspace >= 0 && a.addrspace != actual) die("buffer address-space metadata disagrees with pointer type");
      a.addrspace = actual;
    }
    if (a.kind == "air.imageblock")
      die("input kind air.imageblock (imageblocks/tile memory are Apple-family-4+ features; not in VH-ABI 1)");
    if (a.kind == "air.visible_function_table" || a.kind == "air.indirect_buffer") die("input kind " + a.kind);
  }
  R.declared_textures = declaredTextures(inArgs);
  R.declared_ubo_max = declaredUboMax(inArgs);
  if (R.declared_ubo_max > ABI_MAX_UBO_INDEX)
    die("constant buffer(" + std::to_string(R.declared_ubo_max) + "): Metal buffer index > 30");
  R.driver_table_ubo = R.declared_ubo_max + 1;
  AirOptions o = o0;
  for (int i = 0; i < 16; i++) if (!(R.declared_textures & (1u << i))) o.texClass[i] = 0;

  std::vector<FcDecl> decls = parseFcDecls(*M);
  std::map<int, std::vector<double>> fc;
  R.fcs = resolveFcs(decls, o, R.function, fc);
  SpecResult spec = specialize_and_inline(*M, F, decls, fc);
  if (getenv("AIR2VH_DUMP_LL") || getenv("AIR2IR3_DUMP_LL")) {
    const char *p = getenv("AIR2VH_DUMP_LL") ? getenv("AIR2VH_DUMP_LL") : getenv("AIR2IR3_DUMP_LL");
    std::error_code ec; raw_fd_ostream os(p, ec); M->print(os, nullptr);
  }

  glsl_type_singleton_init_or_ref();
  std::unique_ptr<Translator> T(new Translator(*M, F, stage, R, spec));
  T->inArgs = inArgs; T->outArgs = outArgs; T->opt = &o;
  for (int i = 0; i < 128; i++) if (o.texClass[i]) Translator::checkFormatClass(o.texClass[i], "texture(" + std::to_string(i) + ")");
  try { R.nir = T->run(nopts); }
  catch (...) { if (T->s) ralloc_free(T->s); glsl_type_singleton_decref(); throw; }
  glsl_type_singleton_decref();
  nir_foreach_function_impl(impl, R.nir) nir_foreach_block(blk, impl) nir_foreach_instr(ins2, blk)
    if (ins2->type == nir_instr_type_intrinsic && nir_instr_as_intrinsic(ins2)->intrinsic == nir_intrinsic_demote) R.uses_discard = true;
  std::string impl;
  if (M->getGlobalVariable("llvm.global_ctors") || !spec.implicitVals.empty())
    impl += std::string("  static initialiser evaluated: ") + (spec.ctorEvaluated ? "yes" : "no") + "\n";
  for (auto &kv : spec.implicitVals) impl += "  implicit " + kv.first + " = " + std::to_string(kv.second) + "\n";
  if (spec.taintedPredicates) impl += "  unset function constants: " + std::to_string(spec.taintedPredicates) + " predicate(s) forced false (Metal semantics)\n";
  if (spec.unsetMarkers)
    impl += "  unset function constants in the body: " + std::to_string(spec.unsetMarkers) + " load(s), " + std::to_string(spec.unsetIndependent) +
            " use(s) independent of the value (folded); nothing decided by them\n";
  R.report = impl + T->report;
  return R;
}

static std::string fcValueJson(const AirFc &f) {
  unsigned n = 1; std::string base = fcScalarBase(f.type, n);
  bool fp = base == "float" || base == "half";
  std::string v = "[", bits = "[";
  for (double x : f.value) {
    if (v.size() > 1) { v += ","; bits += ","; }
    char b[64];
    if (!fp) {
      if (base == "ulong" || base == "uint" || base == "ushort" || base == "uchar" || base == "bool") snprintf(b, sizeof b, "%llu", (unsigned long long)fcIntBits(x));
      else snprintf(b, sizeof b, "%lld", (long long)(int64_t)fcIntBits(x));
      v += b; continue;
    }
    if (std::isnan(x)) v += std::signbit(x) ? "\"-nan\"" : "\"nan\"";
    else if (std::isinf(x)) v += x < 0 ? "\"-inf\"" : "\"inf\"";
    else if (x == 0) v += std::signbit(x) ? "-0.0" : "0.0";
    else {
      for (int prec = 1; prec <= 17; prec++) {
        snprintf(b, sizeof b, "%.*g", prec, x);
        if ((float)strtod(b, nullptr) == (float)x) break;
      }
      std::string s = b;
      if (s.find_first_of(".en") == std::string::npos) s += ".0";
      v += s;
    }
    if (base == "float") { float y = (float)x; uint32_t u; memcpy(&u, &y, 4); snprintf(b, sizeof b, "\"0x%08x\"", u); }
    else { _Float16 y = (_Float16)x; uint16_t u; memcpy(&u, &y, 2); snprintf(b, sizeof b, "\"0x%04x\"", u); }
    bits += b;
  }
  return "\"value\":" + v + "]" + (fp ? ",\"bits\":" + bits + "]" : std::string());
}

std::string air_reflection_json(const AirFrontendResult &r) {
  std::string j = "{";
  auto kv = [&](const std::string &k, const std::string &v, bool q = true) { if (j.size() > 1) j += ","; j += "\"" + k + "\":" + (q ? "\"" + jesc(v) + "\"" : v); };
  kv("abi_version", std::to_string(ABI_VERSION), false);
  kv("function", r.function);
  kv("stage", r.stage == MESA_SHADER_VERTEX ? "vertex" : r.stage == MESA_SHADER_FRAGMENT ? "fragment" : "kernel");
  kv("air_triple", r.triple);
  std::string a = "[";
  for (auto &f : r.fcs) {
    if (a.size() > 1) a += ",";
    a += "{\"index\":" + std::to_string(f.index) + ",\"name\":\"" + jesc(f.name) + "\",\"type\":\"" + jesc(f.type) +
         "\",\"defined\":" + (f.defined ? "true" : "false") + "," + fcValueJson(f) + "}";
  }
  kv("function_constants", a + "]", false);
  a = "[";
  for (auto &b : r.bindings) {
    if (a.size() > 1) a += ",";
    a += "{\"kind\":\"" + b.kind + "\",\"metal_index\":" + std::to_string(b.metal_index) + ",\"slot\":" + std::to_string(b.slot);
    if (!b.arg_name.empty()) a += ",\"arg\":\"" + jesc(b.arg_name) + "\"";
    if (!b.type_name.empty()) a += ",\"type\":\"" + jesc(b.type_name) + "\"";
    if (!b.access.empty()) a += ",\"access\":\"" + b.access + "\"";
    if ((b.kind == "ubo" || b.kind == "device_buffer") && b.minimum_bytes >= 0) a += ",\"minimum_bytes\":" + std::to_string(b.minimum_bytes);
    a += std::string(",\"present\":") + (b.present ? "true" : "false");
    a += std::string(",\"used\":") + (b.used ? "true" : "false");
    if (b.kind == "constexpr_sampler") {
      char buf[768]; air_sampler as; air_sampler_decode(b.sampler_word, &as); char dec[640]; air_sampler_json(&as, dec, sizeof dec);
      snprintf(buf, sizeof buf,
               ",\"air_word\":\"0x%016llx\",\"unique_id\":\"0x%016llx\","
               "\"mali_sampler\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],"
               "\"mali_sampler_int_border\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"state\":",
               (unsigned long long)b.sampler_word, (unsigned long long)b.unique_id,
               b.vh_desc[0], b.vh_desc[1], b.vh_desc[2], b.vh_desc[3],
               b.vh_desc[4], b.vh_desc[5], b.vh_desc[6], b.vh_desc[7],
               b.vh_int_border[0], b.vh_int_border[1], b.vh_int_border[2], b.vh_int_border[3]);
      a += buf; a += dec;
    }
    a += "}";
  }
  kv("bindings", a + "]", false);
  char m[32]; snprintf(m, sizeof m, "0x%04x", r.constexpr_sampler_mask); kv("constexpr_sampler_mask", m);
  a = "[";
  for (auto &v : r.varyings) {
    if (a.size() > 1) a += ",";
    a += "{\"name\":\"" + jesc(v.name) + "\",\"slot\":" + std::to_string(v.slot) + ",\"components\":" + std::to_string(v.comps) +
         ",\"bits\":" + std::to_string(v.bits) + ",\"type\":\"" + v.base + "\",\"interp\":\"" + v.interp + "\",\"sampling\":\"" + v.sampling + "\"}";
  }
  kv("varyings", a + "]", false);
  a = "[";
  for (auto &v : r.attributes) {
    if (a.size() > 1) a += ",";
    a += "{\"attribute\":" + std::to_string(v.index) + ",\"components\":" + std::to_string(v.comps) + ",\"bits\":" + std::to_string(v.bits) + ",\"type\":\"" + v.base + "\"}";
  }
  kv("vertex_attributes", a + "]", false);
  a = "[";
  for (auto &v : r.render_targets) {
    if (a.size() > 1) a += ",";
    a += "{\"index\":" + std::to_string(v.index) + ",\"components\":" + std::to_string(v.comps) + ",\"type\":\"" + v.base +
         "\",\"written\":" + (v.written ? "true" : "false") + ",\"fb_fetch\":" + (v.fb_fetch ? "true" : "false") + "}";
  }
  kv("render_targets", a + "]", false);
  a = "["; for (auto &s : r.sysvals) { if (a.size() > 1) a += ","; a += "\"" + s + "\""; } kv("system_values", a + "]", false);
  kv("writes_depth", r.writes_depth ? "true" : "false", false);
  kv("writes_point_size", r.writes_point_size ? "true" : "false", false);
  kv("uses_discard", r.uses_discard ? "true" : "false", false);
  kv("uses_fb_fetch", r.uses_fb_fetch ? "true" : "false", false);
  kv("constant_data_bytes", std::to_string(r.constant_data_bytes), false);
  kv("threadgroup_bytes", std::to_string(r.shared_bytes), false);
  a = "["; for (int x : r.requires_no_attachment) { if (a.size() > 1) a += ","; a += std::to_string(x); } kv("requires_no_attachment", a + "]", false);
  a = "["; for (auto &w : r.warnings) { if (a.size() > 1) a += ","; a += "\"" + jesc(w) + "\""; } kv("warnings", a + "]", false);
  return j + "}";
}
#endif // AIR2VH_FRONTEND_TU
