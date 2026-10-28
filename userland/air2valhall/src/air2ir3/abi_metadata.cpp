// SPDX-License-Identifier: MIT
// ABI constants, shared helpers (die, argOperand, JSON escaping) and the AIR argument/output metadata parser.
// Part of src/vh_frontend.cpp's translation unit, #included from it in order.
#ifdef AIR2VH_FRONTEND_TU

// ---------------------------------------------------------------- ABI constants (spec 07 section 6) --
static const int ABI_VERSION = 1;                // VH-ABI 1
static const int CACHE_FORMAT_VERSION = 1;       // AIR2VH-CACHE-KEY
static const int ABI_MAX_TEXTURES = 128;         // Metal texture(n), n < 128 -> T2[n] / T4[n]
static const int ABI_MAX_SAMPLERS = 16;          // Metal sampler(n) + constexpr samplers share T3[0..15] (T3[16] = dummy)
// On Valhall, T1 holds up to 31 constant buffers (buffer(0..30)).  The front-end's driver_table_ubo
// (declared_ubo_max + 1, <= 31) is only a temporary marker in NIR that air2vh_lower_bindings rewrites
// to load_push_constant (FAU) before pan_postprocess_nir (spec 07 3.3 item 3, 6.4).
static const int ABI_UBO_SLOTS = 32;
static const int ABI_TG_TABLE_OFFSET = 256;      // threadgroup(n) byte offsets at 256 + 4n in driver_table_ubo
static const int ABI_TPG_TABLE_OFFSET = 384;     // threads_per_grid (3 x u32) at 384 in driver_table_ubo (non-uniform dispatch)
static const int ABI_MAX_UBO_INDEX = 30;         // Metal buffer(0..30)
static int uboSlots(const AirOptions &o) { return o.testUboSlots > 0 ? std::min(o.testUboSlots, 32) : ABI_UBO_SLOTS; }
static const int ABI_MAX_RTS = 8;
static const int ABI_FS_BINDLESS_SET = 0;
static const int ABI_FB_READ_SLOT = 0;

[[noreturn]] static void die(const std::string &m) { throw AirError(m); }
static Value *argOperand(const CallInst *CI, unsigned i) {
  if (i >= CI->arg_size()) {
    const Function *F = CI->getCalledFunction();
    die("call to " + (F ? F->getName().str() : std::string("an indirect callee")) + " has " + std::to_string(CI->arg_size()) +
        " operands, operand " + std::to_string(i) + " was expected (an operand layout this compiler does not know)");
  }
  return CI->getArgOperand(i);
}
static bool containsPointer(Type *T) {
  if (T->isPointerTy()) return true;
  if (auto *S = dyn_cast<StructType>(T)) { for (Type *E : S->elements()) if (containsPointer(E)) return true; return false; }
  if (auto *A = dyn_cast<ArrayType>(T)) return containsPointer(A->getElementType());
  if (auto *V = dyn_cast<VectorType>(T)) return containsPointer(V->getElementType());
  return false;
}
static std::string str(const Value *v) {
  std::string s; raw_string_ostream os(s); v->print(os); return s;
}
static std::string jesc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

// ---------------------------------------------------------------- metadata --
struct ArgInfo {
  std::string kind;          // air.vertex_input, air.fragment_input, air.position, air.render_target, ...
  std::string name;          // user(...) / generated(...) varying name
  std::string argName, typeName;
  int argno = -1;            // input argument number (-1 for outputs)
  int location = -1;         // air.location_index / render target index
  int dualIndex = 0;         // air.render_target's second positional: [[color(n), index(1)]] = 1
  std::string locGlobal;     // location given by a function-constant expression global
  int addrspace = -1;        // for buffers
  int64_t bufferSize = -1;   // explicit AIR argument buffer extent, if present
  std::string interp = "perspective", sampling = "center";
  bool gated = false;        // [[function_constant(...)]]
  std::string predGlobal;    // predicate global (e.g. __metal_implicit_fc_pred_3 or an FC initialiser)
  bool active = true;        // gets a binding: present, or read by the specialised code
  bool present = true;       // Metal's notion: predicate true (or not gated)
  bool used = true;          // (inputs) argument has uses after specialisation
  std::string access;        // air.read / air.sample / air.write / air.read_write
};
static std::string mdstr(const Metadata *m) {
  if (auto *s = dyn_cast_or_null<MDString>(m)) return s->getString().str();
  return "";
}
static int mdint(const Metadata *m) {
  if (auto *c = dyn_cast_or_null<ConstantAsMetadata>(m))
    if (auto *ci = dyn_cast<ConstantInt>(c->getValue())) return (int)ci->getSExtValue();
  return -1;
}
static std::string mdglobal(const Metadata *m) {
  if (auto *v = dyn_cast_or_null<ValueAsMetadata>(m))
    if (auto *g = dyn_cast<GlobalValue>(v->getValue()->stripPointerCasts())) return g->getName().str();
  return "";
}
static ArgInfo parseArg(const MDNode *n, bool isOutput) {
  ArgInfo a;
  unsigned i = isOutput ? 0 : 1;
  if (!isOutput) a.argno = mdint(n->getOperand(0));
  unsigned k = i;
  if (k < n->getNumOperands()) a.kind = mdstr(n->getOperand(k));
  if (a.kind == "air.function_constant") {
    a.gated = true;
    if (auto *pn = dyn_cast_or_null<MDNode>(n->getOperand(k + 1).get()))
      if (pn->getNumOperands() > 0) a.predGlobal = mdglobal(pn->getOperand(0));
    k += 2;
    a.kind = k < n->getNumOperands() ? mdstr(n->getOperand(k)) : "";
  }
  if (a.kind == "air.render_target" && k + 1 < n->getNumOperands()) a.location = mdint(n->getOperand(k + 1));
  if (a.kind == "air.render_target" && k + 2 < n->getNumOperands()) a.dualIndex = std::max(0, mdint(n->getOperand(k + 2)));
  for (unsigned j = k + 1; j < n->getNumOperands(); j++) {
    std::string s = mdstr(n->getOperand(j));
    if (s == "air.location_index" && j + 1 < n->getNumOperands()) {
      int v = mdint(n->getOperand(j + 1));
      if (v >= 0) a.location = v; else a.locGlobal = mdglobal(n->getOperand(j + 1));
    }
    else if (s == "air.address_space" && j + 1 < n->getNumOperands()) a.addrspace = mdint(n->getOperand(j + 1));
    else if (s == "air.buffer_size" && j + 1 < n->getNumOperands()) {
      int size = mdint(n->getOperand(j + 1));
      if (size < 0) die("invalid air.buffer_size metadata");
      a.bufferSize = size;
    }
    else if (s == "air.perspective" || s == "air.no_perspective" || s == "air.flat") a.interp = s.substr(4);
    else if (s == "air.center" || s == "air.centroid" || s == "air.sample") {
      if (a.kind == "air.texture" && s == "air.sample") a.access = "sample"; else a.sampling = s.substr(4);
    }
    else if (s == "air.read" || s == "air.write" || s == "air.read_write") a.access = s.substr(4);
    else if (s == "air.arg_name" && j + 1 < n->getNumOperands()) a.argName = mdstr(n->getOperand(j + 1));
    else if (s == "air.arg_type_name" && j + 1 < n->getNumOperands()) a.typeName = mdstr(n->getOperand(j + 1));
    else if (s.rfind("user(", 0) == 0 || s.rfind("generated(", 0) == 0) a.name = s;
  }
  return a;
}

struct FcDecl { int index; std::string type, name, global; };
static std::vector<FcDecl> parseFcDecls(Module &M) {
  std::vector<FcDecl> r;
  if (NamedMDNode *N = M.getNamedMetadata("air.function_constants"))
    for (MDNode *E : N->operands()) {
      if (E->getNumOperands() < 4) continue;
      FcDecl d; d.global = mdglobal(E->getOperand(0)); d.type = mdstr(E->getOperand(1));
      d.name = mdstr(E->getOperand(2)); d.index = mdint(E->getOperand(3));
      r.push_back(d);
    }
  std::sort(r.begin(), r.end(), [](const FcDecl &a, const FcDecl &b) { return a.index < b.index; });
  return r;
}
static int fcIndexOfGlobal(StringRef nm) {
  size_t p = nm.find(".MTL_FC_INIT_");
  if (p == StringRef::npos) return -1;
  return std::atoi(nm.substr(p + 13).str().c_str());
}
#endif // AIR2VH_FRONTEND_TU
