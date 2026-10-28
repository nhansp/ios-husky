/* SPDX-License-Identifier: MIT */
// vh_frontend.cpp — AIR -> NIR front-end translation unit for libair2valhall.
// Includes the air2ir3 front-end parts in order, using vh_sampler.h instead of a8xx_sampler.h
// (spec 07 section 3.1, 3.3).
#define AIR2VH_FRONTEND_TU 1

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/CFG.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Evaluator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "nir.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"
#include "glsl_types.h"
#include "vh_sampler.h"
#include "air2ir3.h"

using namespace llvm;

#include "air2ir3/abi_metadata.cpp"
#include "air2ir3/fc_ctor_eval.cpp"
#include "air2ir3/unset_body.cpp"
#include "air2ir3/specialize.cpp"
#include "air2ir3/translator.cpp"
#include "air2ir3/translator_rt.cpp"
#include "air2ir3/translator_args.cpp"
#include "air2ir3/translator_calls.cpp"
#include "air2ir3/translator_insts.cpp"
#include "air2ir3/frontend.cpp"
