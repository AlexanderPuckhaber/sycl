//===- PrepareSYCLOpt.cpp                                    ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Prepare device code for Optimizations.
//
// ===---------------------------------------------------------------------===//

#include <cstddef>
#include <regex>
#include <string>

#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/SYCL/PrepareSYCLOpt.h"
#include "llvm/Support/Casting.h"

using namespace llvm;

namespace {

struct PrepareSYCLOpt : public ModulePass {

  static char ID; // Pass identification, replacement for typeid

  PrepareSYCLOpt() : ModulePass(ID) {}

  void turnNonKernelsIntoPrivate(Module &M) {
    for (GlobalObject &G : M.global_objects()) {
      if (auto *F = dyn_cast<Function>(&G))
        if (F->getCallingConv() == CallingConv::SPIR_KERNEL)
          continue;
      if (G.isDeclaration())
        continue;
      G.setLinkage(llvm::GlobalValue::PrivateLinkage);
    }
  }

  void setCallingConventions(Module& M) {
    for (Function &F : M.functions()) {
      if (F.getCallingConv() == CallingConv::SPIR_KERNEL) {
        assert(F.use_empty());
        continue;
      }
      F.setCallingConv(CallingConv::SPIR_FUNC);
      for (Value* V : F.users()) {
        if (auto* Call = dyn_cast<CallBase>(V))
          Call->setCallingConv(CallingConv::SPIR_FUNC);
      }
    }
  }

  /// At this point in the pipeline Annotations intrinsic have all been
  /// converted into what they need to be. But they can still be present and
  /// have pointer on pointer as arguments which v++ can't deal with.
  void removeAnnotationsIntrisic(Module &M) {
    SmallVector<Instruction *, 16> ToRemove;
    for (Function &F : M.functions())
      if (F.getIntrinsicID() == Intrinsic::annotation ||
          F.getIntrinsicID() == Intrinsic::ptr_annotation ||
          F.getIntrinsicID() == Intrinsic::var_annotation)
        for (User *U : F.users())
          if (auto *I = dyn_cast<Instruction>(U))
            ToRemove.push_back(I);
    for (Instruction *I : ToRemove)
      I->eraseFromParent();
  }

  bool runOnModule(Module &M) override {
    turnNonKernelsIntoPrivate(M);
    setCallingConventions(M);
    removeAnnotationsIntrisic(M);
    return true;
  }
};

}

namespace llvm {
void initializePrepareSYCLOptPass(PassRegistry &Registry);
}

INITIALIZE_PASS(PrepareSYCLOpt, "preparesycl",
  "prepare SYCL device code to optimizations", false, false)
ModulePass *llvm::createPrepareSYCLOptPass() {return new PrepareSYCLOpt();}

char PrepareSYCLOpt::ID = 0;
