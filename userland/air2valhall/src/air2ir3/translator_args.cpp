// SPDX-License-Identifier: MIT
// Translator members defined out of class: argument and output binding (setupArgs).
// Part of src/vh_frontend.cpp's translation unit, #included from it in order.
#ifdef AIR2VH_FRONTEND_TU

inline void Translator::setupArgs() {
  // Resolve gating, attribute index expressions and usage first.
  for (ArgInfo &a : outArgs) {
    if (a.gated && a.kind == "air.render_target" && spec.ambiguousPredicates.count(a.predGlobal)) {
      int loc = locValue(a);
      if (loc < 0 || loc >= ABI_MAX_RTS) die(spec.ambiguousPredicates.at(a.predGlobal));
      if (opt->rtClass[loc]) die(spec.ambiguousPredicates.at(a.predGlobal) + " (colour attachment " + std::to_string(loc) + " is an XR attachment of this pipeline)");
      a.active = a.present = false; a.location = loc;
      if (std::find(R.requires_no_attachment.begin(), R.requires_no_attachment.end(), loc) == R.requires_no_attachment.end())
        R.requires_no_attachment.push_back(loc);
      R.warnings.push_back("[[color(" + std::to_string(loc) + (a.dualIndex ? "), index(" + std::to_string(a.dualIndex) : std::string()) + ")]] is gated by " + a.predGlobal + ", which depends on the value of an unset function constant: "
                           "not written; valid only for a pipeline without colour attachment " + std::to_string(loc) + " (BINDING-ABI.md 3.7)");
      continue;
    }
    if (a.gated) a.active = a.present = gatedPresent(a, "output");
    if (a.active) a.location = locValue(a);
  }
  for (ArgInfo &a : inArgs) {
    Argument *A = F->getArg(a.argno);
    a.used = !A->use_empty();
    if (a.gated) a.present = gatedPresent(a, "argument");
    a.active = a.present || a.used;
    if (a.active) a.location = locValue(a);
    if (a.used && !a.present) {
      R.warnings.push_back("argument " + a.argName + " is absent (function-constant predicate false or unset) but the code reads it: bind-through");
    }
  }
  // Varying slots: deterministic, sorted by name (BINDING-ABI.md section 4).  VS and FS are
  // linked by name, never by slot.
  std::set<std::string> names;
  if (stage == MESA_SHADER_VERTEX) { for (auto &a : outArgs) if (a.active && a.kind == "air.vertex_output") names.insert(a.name); }
  if (stage == MESA_SHADER_FRAGMENT) { for (auto &a : inArgs) if (a.active && a.kind == "air.fragment_input") names.insert(a.name); }
  for (auto &n : names) { int sl = varyingSlot.size(); varyingSlot[n] = sl; }
  if (varyingSlot.size() > 32) die("more than 32 varyings");

  // Outputs: return value (vertex outputs / render targets).
  Type *RT = F->getReturnType();
  std::vector<Type *> rts;
  if (auto *ST = dyn_cast<StructType>(RT)) for (Type *e : ST->elements()) rts.push_back(e);
  else if (!RT->isVoidTy()) rts.push_back(RT);
  if (rts.size() != outArgs.size()) die("output metadata does not match the return type");
  for (auto &a : outArgs) if (a.active && a.kind == "air.render_target") writtenRts++;
  for (unsigned i = 0; i < outArgs.size(); i++) {
    ArgInfo &a = outArgs[i];
    if (!a.active) {                                 // function-constant-gated output switched off
      for (unsigned c = 0; c < nflat(rts[i]); c++) outs.push_back({nullptr, c});
      report += "  out  " + a.kind + " " + a.name + " (function constant off)\n";
      continue;
    }
    int loc;
    if (a.kind == "air.position") loc = VARYING_SLOT_POS;
    else if (a.kind == "air.vertex_output") loc = VARYING_SLOT_VAR0 + varyingSlot.at(a.name);
    else if (a.kind == "air.render_target") {
      if (a.location < 0 || a.location >= ABI_MAX_RTS) die("render target index " + std::to_string(a.location) + " out of range");
      if (a.dualIndex) die("[[color(" + std::to_string(a.location) + "), index(" + std::to_string(a.dualIndex) + ")]] (app dual-source blending) is not in VH-ABI 1");
      loc = FRAG_RESULT_DATA0 + a.location;
    }
    else if (a.kind == "air.depth") { loc = FRAG_RESULT_DEPTH; R.writes_depth = true; }
    else if (a.kind == "air.point_size") { loc = VARYING_SLOT_PSIZ; R.writes_point_size = true; }
    else if (a.kind == "air.sample_mask") loc = FRAG_RESULT_SAMPLE_MASK;
    else die("output kind " + a.kind);
    bool rt = a.kind == "air.render_target";
    // Note: for render targets, keep the declared scalar type (half vs float, int vs uint) in
    // AirRenderTarget before mkvar(..., true) forces 32-bit storage so pan_nir_lower_fs_outputs
    // and air2vh_program.fs.rt[].base_type know the logical type.
    std::vector<Type *> flatT; flatten(rts[i], flatT);
    unsigned origBits = bits(flatT[0]);
    std::string origBase = flatT[0]->isFloatingPointTy() ? (origBits == 16 ? "half" : "float")
                         : (origBits == 16 ? "ushort" : "uint");
    nir_variable *v = mkvar(nir_var_shader_out, rts[i], loc, a.kind.c_str(), rt || a.kind == "air.sample_mask");
    if (rt) {
      v->data.index = 0;
      if (opt->rtClass[a.location]) setupRtLowering(a.location, v, true);
      AirRenderTarget t; t.index = a.location; t.comps = glsl_get_vector_elements(v->type);
      t.bits = origBits; t.base = origBase; t.written = true; R.render_targets.push_back(t);
    }
    if (a.kind == "air.vertex_output") {
      v->data.interpolation = a.interp == "flat" ? INTERP_MODE_FLAT : a.interp == "no_perspective" ? INTERP_MODE_NOPERSPECTIVE : INTERP_MODE_SMOOTH;
      v->data.centroid = a.sampling == "centroid"; v->data.sample = a.sampling == "sample";
      AirVarying vy; vy.name = a.name; vy.slot = varyingSlot.at(a.name); vy.comps = glsl_get_vector_elements(v->type);
      vy.bits = glsl_get_bit_size(v->type); vy.base = baseName(glsl_get_base_type(v->type)); vy.interp = a.interp; vy.sampling = a.sampling;
      R.varyings.push_back(vy);
      // Spec 07 3.3 item 1: Valhall packs half varyings at 16-bit natively in
      // pan_varying_collect_formats (pan_nir_collect_varyings.c:218-246) and
      // pan_build_varying_layout_compact (pan_nir_collect_varyings.c:366-468),
      // so do NOT widen 16-bit vertex outputs to 32-bit here.
    }
    for (unsigned c = 0; c < nflat(rts[i]); c++) outs.push_back({v, c});
    report += "  out  " + a.kind + " " + a.name + " -> location " + std::to_string(loc) + "\n";
  }
  // Sampler slots used by API sampler arguments, then constexpr samplers in the LOWEST free slots
  uint32_t apiMask = 0;
  for (auto &a : inArgs) if (a.active && a.kind == "air.sampler") {
    if (a.location < 0 || a.location >= ABI_MAX_SAMPLERS) die("sampler index " + std::to_string(a.location) + " >= 16");
    apiMask |= 1u << a.location;
  }
  {
    std::vector<GlobalVariable *> cs;
    for (GlobalVariable &G : M.globals())
      if (G.getName().starts_with("__air_sampler_state") && !G.use_empty()) cs.push_back(&G);
    std::sort(cs.begin(), cs.end(), [](GlobalVariable *x, GlobalVariable *y) { return x->getName() < y->getName(); });
    int next = 0;
    for (GlobalVariable *G : cs) {
      while (next < ABI_MAX_SAMPLERS && (apiMask >> next) & 1) next++;
      if (next >= ABI_MAX_SAMPLERS) die("more than 16 samplers (API + constexpr) in one function");
      constSamplers[G] = next;
      R.constexpr_sampler_mask |= 1u << next;
      Constant *initial = G->getInitializer();
      uint64_t w0 = 0, w1 = 0;
      if (auto *word = dyn_cast<ConstantInt>(initial)) {
        if (!word->getType()->isIntegerTy(64)) die("constexpr sampler scalar is not i64");
        w0 = word->getZExtValue();
      } else if (auto *array = dyn_cast<ConstantDataArray>(initial)) {
        if (!array->getElementType()->isIntegerTy(64) || !array->getNumElements() || array->getNumElements() > 2)
          die("constexpr sampler array must contain one or two i64 words");
        w0 = array->getElementAsInteger(0);
        if (array->getNumElements() > 1) w1 = array->getElementAsInteger(1);
      } else {
        die("constexpr sampler initialiser is not an i64 state or constant array");
      }
      AirBinding bd; bd.kind = "constexpr_sampler"; bd.slot = next; bd.arg_name = G->getName().str();
      bd.sampler_word = w0;
      SHA256 h; uint8_t buf[24]; memcpy(buf, "AIRSAMP1", 8); memcpy(buf + 8, &bd.sampler_word, 8); memcpy(buf + 16, &w1, 8);
      h.update(ArrayRef<uint8_t>(buf, 24)); auto dg = h.final();
      for (int i = 0; i < 8; i++) bd.unique_id = (bd.unique_id << 8) | dg[i];
      air_sampler as; air_sampler_decode(bd.sampler_word, &as);
      air_sampler_a8xx(&as, bd.a8xx_desc);
      air_sampler_valhall(&as, bd.vh_desc, bd.vh_int_border);
      R.bindings.push_back(bd);
      char buf2[160]; snprintf(buf2, sizeof buf2, "  constexpr sampler %s = 0x%016llx -> S#%d (unique id 0x%016llx)\n",
                               G->getName().str().c_str(), (unsigned long long)bd.sampler_word, next, (unsigned long long)bd.unique_id);
      report += buf2;
      next++;
    }
  }
  // Inputs.
  for (ArgInfo &a : inArgs) {
    Argument *A = F->getArg(a.argno);
    std::string line = "  in   #" + std::to_string(a.argno) + " " + a.kind;
    if (!a.active) { report += line + " (function constant off)\n"; continue; }
    if (a.kind == "air.vertex_input") {
      if (a.location < 0 || a.location >= 16)
        die("vertex attribute index " + std::to_string(a.location) + " out of range (Valhall PAN_MAX_VERTEX_ATTRIBS = 16)");
      if (a.used) {
        nir_variable *v = mkvar(nir_var_shader_in, A->getType(), VERT_ATTRIB_GENERIC0 + a.location, "attr");
        v->data.driver_location = a.location; // T0[a.location] (pan_nir_lower_vs_inputs.c:33)
        vals[A] = coerce(split(nir_load_var(&b, v)), A->getType());
        AirAttribute at; at.index = a.location; at.comps = glsl_get_vector_elements(v->type); at.bits = glsl_get_bit_size(v->type);
        at.base = baseName(glsl_get_base_type(v->type)); R.attributes.push_back(at);
        AirBinding bd; bd.kind = "attribute"; bd.metal_index = a.location; bd.slot = a.location;
        bd.arg_name = a.argName; bd.type_name = a.typeName; bd.present = a.present; bd.used = a.used;
        R.bindings.push_back(bd);
      }
      line += " attribute " + std::to_string(a.location);
    } else if (a.kind == "air.fragment_input") {
      nir_variable *v = mkvar(nir_var_shader_in, A->getType(), VARYING_SLOT_VAR0 + varyingSlot.at(a.name), a.name.c_str());
      v->data.interpolation = a.interp == "flat" ? INTERP_MODE_FLAT : a.interp == "no_perspective" ? INTERP_MODE_NOPERSPECTIVE : INTERP_MODE_SMOOTH;
      v->data.centroid = a.sampling == "centroid"; v->data.sample = a.sampling == "sample";
      if (a.used) vals[A] = split(nir_load_var(&b, v));
      AirVarying vy; vy.name = a.name; vy.slot = varyingSlot.at(a.name); vy.comps = glsl_get_vector_elements(v->type);
      vy.bits = glsl_get_bit_size(v->type); vy.base = baseName(glsl_get_base_type(v->type)); vy.interp = a.interp; vy.sampling = a.sampling;
      R.varyings.push_back(vy);
      line += " " + a.name + " varying slot " + std::to_string(vy.slot) + " " + a.interp;
    } else if (a.kind == "air.position" && stage == MESA_SHADER_FRAGMENT) {
      if (a.used) { vals[A] = coerce(split(nir_load_frag_coord(&b)), A->getType()); R.sysvals.push_back("frag_coord"); }
    } else if (a.kind == "air.render_target") {        // framebuffer fetch: read the colour attachment
      if (a.location < 0 || a.location >= ABI_MAX_RTS) die("framebuffer-fetch colour index " + std::to_string(a.location) + " out of range");
      if (a.used) {
        nir_variable *v = nullptr;
        for (auto &o : outs) if (o.first && o.first->data.location == (int)(FRAG_RESULT_DATA0 + a.location)) v = o.first;
        if (!v) {
          std::vector<Type *> flatT; flatten(A->getType(), flatT);
          unsigned origBits = bits(flatT[0]);
          std::string origBase = flatT[0]->isFloatingPointTy() ? (origBits == 16 ? "half" : "float")
                               : (origBits == 16 ? "ushort" : "uint");
          v = mkvar(nir_var_shader_out, A->getType(), FRAG_RESULT_DATA0 + a.location, "fbfetch", true);
          AirRenderTarget t; t.index = a.location; t.comps = glsl_get_vector_elements(v->type);
          t.bits = origBits; t.base = origBase; t.written = false; R.render_targets.push_back(t);
        }
        for (auto &t : R.render_targets) if (t.index == a.location) t.fb_fetch = true;
        v->data.fb_fetch_output = 1;
        s->info.fs.uses_fbfetch_output = true;
        R.uses_fb_fetch = true;
        if (opt->rtClass[a.location]) {
          if (!rtLow.count(a.location)) setupRtLowering(a.location, v, false);
          unsigned cls = rtLow[a.location].cls;
          std::vector<nir_def *> u = split(nir_load_var(&b, v)), d;
          for (unsigned c = 0; c < u.size(); c++)
            d.push_back(c == 3 && cls == AIR_FMT_XR10_BGR32 ? nir_imm_float(&b, 1.0) : xrDecode(cls, u[c]));
          vals[A] = coerce(d, A->getType());
        } else
        vals[A] = coerce(split(nir_load_var(&b, v)), A->getType());
        AirBinding bd; bd.kind = "fb_fetch"; bd.metal_index = a.location; bd.slot = a.location;
        bd.arg_name = a.argName; bd.type_name = a.typeName; bd.present = a.present; R.bindings.push_back(bd);
      }
      line += " (framebuffer fetch, colour " + std::to_string(a.location) + (a.used ? "" : ", unused") + ")";
    } else if (a.kind == "air.buffer") {
      PtrVal p{};
      AirBinding bd; bd.metal_index = a.location; bd.arg_name = a.argName; bd.type_name = a.typeName; bd.used = a.used;
      bd.present = a.present;
      if (a.location < 0 || a.location > ABI_MAX_UBO_INDEX) die("buffer index " + std::to_string(a.location) + " out of range");
      if (a.addrspace == 2) {
        p.kind = PtrVal::UBO; p.index = a.location; if (a.used) maxUbo = std::max(maxUbo, a.location);
        bd.kind = "ubo"; bd.slot = a.location; bd.minimum_bytes = a.bufferSize;
      } else if (a.addrspace == 1) {   // device buffer: 64-bit GPU VA from FAU buffer_va[n] (via marker UBO driver_table_ubo)
        p.kind = PtrVal::GLOBAL;
        if (a.used) {
          _nir_load_ubo_indices ix = {}; ix.align_mul = 8; ix.range = ~0u;
          p.addr64 = nir_pack_64_2x32(&b, _nir_build_load_ubo(&b, 2, 32, nir_imm_int(&b, driverTableUbo()), nir_imm_int(&b, 8 * a.location), ix));
          maxUbo = std::max(maxUbo, R.driver_table_ubo);
        }
        bd.kind = "device_buffer"; bd.slot = 8 * a.location; bd.minimum_bytes = a.bufferSize;
      } else if (a.addrspace == 3) {
        if (stage != MESA_SHADER_COMPUTE) die("threadgroup buffer argument in a " + std::string(stage == MESA_SHADER_VERTEX ? "vertex" : "fragment") + " function (tile shaders are not in VH-ABI 1)");
        p.kind = PtrVal::SHARED;
        if (a.used) {
          _nir_load_ubo_indices ix = {}; ix.align_mul = 4; ix.range = ~0u;
          tgBase[A] = _nir_build_load_ubo(&b, 1, 32, nir_imm_int(&b, driverTableUbo()), nir_imm_int(&b, ABI_TG_TABLE_OFFSET + 4 * a.location), ix);
          maxUbo = std::max(maxUbo, R.driver_table_ubo);
        }
        bd.kind = "threadgroup_buffer"; bd.slot = ABI_TG_TABLE_OFFSET + 4 * a.location;
        p.off = a.used ? tgBase[A] : nir_imm_int(&b, 0);
        ptrs[A] = p; R.bindings.push_back(bd);
        report += line + " threadgroup(" + std::to_string(a.location) + ") -> shared memory at FAU offset\n";
        continue;
      } else die("buffer in address space " + std::to_string(a.addrspace) + " as an argument");
      p.off = nir_imm_int(&b, 0);
      ptrs[A] = p;
      R.bindings.push_back(bd);
      line += " buffer(" + std::to_string(a.location) + ") addrspace " + std::to_string(a.addrspace) +
              (a.addrspace == 2 ? " -> UBO T1" : " -> global (VA from FAU)");
    } else if (a.kind == "air.texture") {
      if (a.location < 0 || a.location >= ABI_MAX_TEXTURES)
        die("texture index " + std::to_string(a.location) + " >= " + std::to_string(ABI_MAX_TEXTURES));
      tex[A] = a.location; line += " texture(" + std::to_string(a.location) + ")";
      if (a.access == "read_write") die("texture(" + std::to_string(a.location) + ") with access::read_write (coherent image load/store): not in VH-ABI 1");
      AirBinding bd; bd.kind = a.access == "write" ? "storage_texture" : "texture"; bd.metal_index = bd.slot = a.location; bd.arg_name = a.argName;
      bd.type_name = a.typeName; bd.access = a.access; bd.used = a.used; bd.present = a.present; R.bindings.push_back(bd);
    } else if (a.kind == "air.sampler") {
      smp[A] = a.location; line += " sampler(" + std::to_string(a.location) + ")";
      AirBinding bd; bd.kind = "sampler"; bd.metal_index = bd.slot = a.location; bd.arg_name = a.argName;
      bd.used = a.used; bd.present = a.present; R.bindings.push_back(bd);
    } else if (!a.used) { line += " (unused)"; }
    else if (a.kind == "air.vertex_id") { vals[A] = coerce({nir_load_vertex_id(&b)}, A->getType()); R.sysvals.push_back("vertex_id"); }
    else if (a.kind == "air.instance_id") { vals[A] = coerce({nir_load_instance_id(&b)}, A->getType()); R.sysvals.push_back("instance_id"); }
    else if (a.kind == "air.base_vertex") { vals[A] = coerce({nir_load_base_vertex(&b)}, A->getType()); R.sysvals.push_back("base_vertex"); }
    else if (a.kind == "air.base_instance") { vals[A] = coerce({nir_load_base_instance(&b)}, A->getType()); R.sysvals.push_back("base_instance"); }
    else if (a.kind == "air.front_facing") { vals[A] = {nir_load_front_face(&b, 1)}; R.sysvals.push_back("front_facing"); }
    else if (a.kind == "air.sample_id") { vals[A] = coerce({nir_load_sample_id(&b)}, A->getType()); R.sysvals.push_back("sample_id"); }
    else if (a.kind == "air.point_coord") {
      // Valhall (arch >= 6): pan_nir_lower_var_special_pan lowers load_point_coord to
      // LD_VAR_SPECIAL.point (pan_nir_lower_var_special.c:23-24, pan_nir_collect_varyings.c:295).
      vals[A] = coerce(split(nir_load_point_coord(&b)), A->getType()); R.sysvals.push_back("point_coord");
    }
    else if (a.kind == "air.thread_position_in_grid") { vals[A] = coerce(split(nir_load_global_invocation_id(&b, 32)), A->getType()); R.sysvals.push_back("thread_position_in_grid"); }
    else if (a.kind == "air.thread_position_in_threadgroup") { vals[A] = coerce(split(nir_load_local_invocation_id(&b)), A->getType()); R.sysvals.push_back("thread_position_in_threadgroup"); }
    else if (a.kind == "air.threadgroup_position_in_grid") { vals[A] = coerce(split(nir_load_workgroup_id(&b)), A->getType()); R.sysvals.push_back("threadgroup_position_in_grid"); }
    else if (a.kind == "air.threads_per_threadgroup") { vals[A] = coerce(split(nir_load_workgroup_size(&b)), A->getType()); R.sysvals.push_back("threads_per_threadgroup"); }
    else if (a.kind == "air.threadgroups_per_grid") { vals[A] = coerce(split(nir_load_num_workgroups(&b)), A->getType()); R.sysvals.push_back("threadgroups_per_grid"); }
    else if (a.kind == "air.threads_per_grid") {
      if (opt && opt->nonuniformDispatch) {
        _nir_load_ubo_indices ix = {}; ix.align_mul = 4; ix.range = ~0u;
        nir_def *tpg = _nir_build_load_ubo(&b, 3, 32, nir_imm_int(&b, driverTableUbo()), nir_imm_int(&b, ABI_TPG_TABLE_OFFSET), ix);
        vals[A] = coerce(split(tpg), A->getType());
      } else {
        vals[A] = coerce(split(nir_imul(&b, nir_load_num_workgroups(&b), nir_load_workgroup_size(&b))), A->getType());
      }
      R.sysvals.push_back("threads_per_grid");
    }
    else if (a.kind == "air.thread_index_in_threadgroup") {
      nir_def *local_id = nir_load_local_invocation_id(&b);
      nir_def *local_size = nir_load_workgroup_size(&b);
      nir_def *flat_local_id =
         nir_iadd(&b,
            nir_channel(&b, local_id, 0),
            nir_imul(&b,
               nir_channel(&b, local_size, 0),
               nir_iadd(&b,
                  nir_channel(&b, local_id, 1),
                  nir_imul(&b,
                     nir_channel(&b, local_size, 1),
                     nir_channel(&b, local_id, 2)))));
      vals[A] = coerce({flat_local_id}, A->getType());
      R.sysvals.push_back("thread_index_in_threadgroup");
    }
    else if (a.kind == "air.thread_index_in_simdgroup") { vals[A] = coerce({nir_load_subgroup_invocation(&b)}, A->getType()); R.sysvals.push_back("thread_index_in_simdgroup"); }
    else if (a.kind == "air.simdgroup_index_in_threadgroup") { vals[A] = coerce({nir_load_subgroup_id(&b)}, A->getType()); R.sysvals.push_back("simdgroup_index_in_threadgroup"); }
    else if (a.kind == "air.simdgroups_per_threadgroup") {
      // Valhall subgroup size is 16 (pan_subgroup_size(11) = 16, pan_compiler.h:581-592);
      // compute DIV_ROUND_UP(wgs.x * wgs.y * wgs.z, 16) from load_workgroup_size because
      // bifrost_nir.c:488 asserts !workgroup_size_variable on load_num_subgroups.
      nir_def *wgs = nir_load_workgroup_size(&b);
      nir_def *total = nir_imul(&b, nir_channel(&b, wgs, 0),
                                nir_imul(&b, nir_channel(&b, wgs, 1), nir_channel(&b, wgs, 2)));
      nir_def *nsg = nir_ushr_imm(&b, nir_iadd_imm(&b, total, 15), 4);
      vals[A] = coerce({nsg}, A->getType());
      R.sysvals.push_back("simdgroups_per_threadgroup");
    }
    else if (a.kind == "air.thread_index_in_quadgroup") { vals[A] = coerce({nir_iand_imm(&b, nir_load_subgroup_invocation(&b), 3)}, A->getType()); R.sysvals.push_back("thread_index_in_quadgroup"); }
    else die("input kind " + a.kind);
    report += line + "\n";
  }
}
#endif // AIR2VH_FRONTEND_TU
