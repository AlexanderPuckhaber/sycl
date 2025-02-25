//===- LowerMemIntrinsics.cpp ----------------------------------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/LowerMemIntrinsics.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <optional>

using namespace llvm;

namespace {

cl::opt<bool>
    LowerToNonI8Type("lower-mem-intr-to-llvm-type", cl::Hidden,
                     cl::desc("try to lower memory intrinsics to their "
                              "underlying LLVM IR types and not i8"));

cl::opt<bool> RequestedUnrollCount(
    "lower-mem-intr-full-unroll", cl::Hidden,
    cl::desc("add full unroll annotations to generated loops"));

Value *Skip1BitCast(Value *V) {
  assert(V->getType()->isPointerTy());

  if (auto *BC = dyn_cast<BitCastInst>(V))
    return BC->getOperand(0);
  if (auto *BC = dyn_cast<BitCastOperator>(V))
    return BC->getOperand(0);
  return V;
}

void maybeAddUnrollAnnotation(BranchInst *Br) {
  if (RequestedUnrollCount > 0) {
    MDNode *MD =
        MDNode::get(Br->getContext(),
                    {MDString::get(Br->getContext(), "llvm.loop.unroll.full")});

    MDNode *LoopID = MDNode::getDistinct(Br->getContext(), {nullptr, MD});
    LoopID->replaceOperandWith(0, LoopID);
    Br->setMetadata(LLVMContext::MD_loop, LoopID);
  }
}

/// Choose the LLVM type that should be used for a memory operation on Ptr1 and
/// maybe Ptr2
Type *determineUnderlyingType(Type* Default, Value *Ptr1, Value *Ptr2 = nullptr) {
  assert((Ptr1->getType()->isPointerTy() &&
          Ptr1->getType()->getPointerElementType()->isIntegerTy(8)) &&
         (!Ptr2 || (Ptr2->getType()->isPointerTy() &&
                    Ptr2->getType()->getPointerElementType()->isIntegerTy(8))));

  if (!Ptr2 || Skip1BitCast(Ptr1)->getType() == Skip1BitCast(Ptr2)->getType())
    return Skip1BitCast(Ptr1)->getType()->getPointerElementType();
  return Default;
}

/// return a constant of Type Ty as if it was obtained from a memset over the
/// representation of the type.
Constant *emitBytePatternForType(Type *Ty, ConstantInt *Pattern, Module &M) {
  const DataLayout &DL = M.getDataLayout();
  uint64_t IntValue;
  std::memset(&IntValue, Pattern->getZExtValue(), sizeof(IntValue));
  if (Ty->isIntOrIntVectorTy()) {
    unsigned BitWidth =
        cast<llvm::IntegerType>(Ty->getScalarType())->getBitWidth();
    if (BitWidth <= 64)
      return llvm::ConstantInt::get(Ty, IntValue);
    return llvm::ConstantInt::get(
        Ty, llvm::APInt::getSplat(BitWidth, llvm::APInt(64, IntValue)));
  }
  if (Ty->isPtrOrPtrVectorTy()) {
    auto *PtrTy = cast<llvm::PointerType>(Ty->getScalarType());
    unsigned PtrWidth = DL.getPointerSizeInBits();
    llvm::Type *IntTy = llvm::IntegerType::get(M.getContext(), PtrWidth);
    auto *Int = llvm::ConstantInt::get(IntTy, IntValue);
    return llvm::ConstantExpr::getIntToPtr(Int, PtrTy);
  }
  if (Ty->isStructTy()) {
    auto *StructTy = cast<llvm::StructType>(Ty);
    llvm::SmallVector<llvm::Constant *, 8> Struct(StructTy->getNumElements());
    for (unsigned El = 0; El != Struct.size(); ++El)
      Struct[El] =
          emitBytePatternForType(StructTy->getElementType(El), Pattern, M);
    return llvm::ConstantStruct::get(StructTy, Struct);
  }
  if (Ty->isFPOrFPVectorTy()) {
    unsigned BitWidth = llvm::APFloat::semanticsSizeInBits(
        Ty->getScalarType()->getFltSemantics());
    llvm::APInt Payload(BitWidth, IntValue);
    if (BitWidth >= 64)
      Payload = llvm::APInt::getSplat(BitWidth, Payload);
    return ConstantFP::get(
        Ty, APFloat(Ty->getScalarType()->getFltSemantics(), Payload));
  }
  if (Ty->isArrayTy()) {
    auto *ArrTy = cast<llvm::ArrayType>(Ty);
    llvm::SmallVector<llvm::Constant *, 8> Element(
        ArrTy->getNumElements(),
        emitBytePatternForType(ArrTy->getElementType(), Pattern, M));
    return llvm::ConstantArray::get(ArrTy, Element);
  }
  llvm_unreachable("Unhandled type");
}

}

void llvm::createMemCpyLoopKnownSize(
    Instruction *InsertBefore, Value *SrcAddr, Value *DstAddr,
    ConstantInt *CopyLen, Align SrcAlign, Align DstAlign, bool SrcIsVolatile,
    bool DstIsVolatile, bool CanOverlap, const TargetTransformInfo &TTI,
    std::optional<uint32_t> AtomicElementSize) {
  // No need to expand zero length copies.
  if (CopyLen->isZero())
    return;

  BasicBlock *PreLoopBB = InsertBefore->getParent();
  BasicBlock *PostLoopBB = nullptr;
  Function *ParentFunc = PreLoopBB->getParent();
  LLVMContext &Ctx = PreLoopBB->getContext();
  const DataLayout &DL = ParentFunc->getParent()->getDataLayout();
  MDBuilder MDB(Ctx);
  MDNode *NewDomain = MDB.createAnonymousAliasScopeDomain("MemCopyDomain");
  StringRef Name = "MemCopyAliasScope";
  MDNode *NewScope = MDB.createAnonymousAliasScope(NewDomain, Name);

  unsigned SrcAS = cast<PointerType>(SrcAddr->getType())->getAddressSpace();
  unsigned DstAS = cast<PointerType>(DstAddr->getType())->getAddressSpace();

  Type *TypeOfCopyLen = CopyLen->getType();
  Type *LoopOpType = TTI.getMemcpyLoopLoweringType(
      Ctx, CopyLen, SrcAS, DstAS, SrcAlign.value(), DstAlign.value(),
      AtomicElementSize);
  if (LowerToNonI8Type)
    LoopOpType = determineUnderlyingType(LoopOpType, SrcAddr, DstAddr);
  assert((!AtomicElementSize || !LoopOpType->isVectorTy()) &&
         "Atomic memcpy lowering is not supported for vector operand type");

  unsigned LoopOpSize = DL.getTypeStoreSize(LoopOpType);
  assert((!AtomicElementSize || LoopOpSize % *AtomicElementSize == 0) &&
      "Atomic memcpy lowering is not supported for selected operand size");

  uint64_t LoopEndCount = CopyLen->getZExtValue() / LoopOpSize;

  if (LoopEndCount != 0) {
    // Split
    PostLoopBB = PreLoopBB->splitBasicBlock(InsertBefore, "memcpy-split");
    BasicBlock *LoopBB =
        BasicBlock::Create(Ctx, "load-store-loop", ParentFunc, PostLoopBB);
    PreLoopBB->getTerminator()->setSuccessor(0, LoopBB);

    IRBuilder<> PLBuilder(PreLoopBB->getTerminator());

    // Cast the Src and Dst pointers to pointers to the loop operand type (if
    // needed).
    PointerType *SrcOpType = PointerType::get(LoopOpType, SrcAS);
    PointerType *DstOpType = PointerType::get(LoopOpType, DstAS);
    if (SrcAddr->getType() != SrcOpType) {
      SrcAddr = PLBuilder.CreateBitCast(SrcAddr, SrcOpType);
    }
    if (DstAddr->getType() != DstOpType) {
      DstAddr = PLBuilder.CreateBitCast(DstAddr, DstOpType);
    }

    Align PartDstAlign(commonAlignment(DstAlign, LoopOpSize));
    Align PartSrcAlign(commonAlignment(SrcAlign, LoopOpSize));

    IRBuilder<> LoopBuilder(LoopBB);
    PHINode *LoopIndex = LoopBuilder.CreatePHI(TypeOfCopyLen, 2, "loop-index");
    LoopIndex->addIncoming(ConstantInt::get(TypeOfCopyLen, 0U), PreLoopBB);
    // Loop Body
    Value *SrcGEP =
        LoopBuilder.CreateInBoundsGEP(LoopOpType, SrcAddr, LoopIndex);
    LoadInst *Load = LoopBuilder.CreateAlignedLoad(LoopOpType, SrcGEP,
                                                   PartSrcAlign, SrcIsVolatile);
    if (!CanOverlap) {
      // Set alias scope for loads.
      Load->setMetadata(LLVMContext::MD_alias_scope,
                        MDNode::get(Ctx, NewScope));
    }
    Value *DstGEP =
        LoopBuilder.CreateInBoundsGEP(LoopOpType, DstAddr, LoopIndex);
    StoreInst *Store = LoopBuilder.CreateAlignedStore(
        Load, DstGEP, PartDstAlign, DstIsVolatile);
    if (!CanOverlap) {
      // Indicate that stores don't overlap loads.
      Store->setMetadata(LLVMContext::MD_noalias, MDNode::get(Ctx, NewScope));
    }
    if (AtomicElementSize) {
      Load->setAtomic(AtomicOrdering::Unordered);
      Store->setAtomic(AtomicOrdering::Unordered);
    }
    Value *NewIndex =
        LoopBuilder.CreateAdd(LoopIndex, ConstantInt::get(TypeOfCopyLen, 1U));
    LoopIndex->addIncoming(NewIndex, LoopBB);

    // Create the loop branch condition.
    Constant *LoopEndCI = ConstantInt::get(TypeOfCopyLen, LoopEndCount);
    BranchInst *Br = LoopBuilder.CreateCondBr(
        LoopBuilder.CreateICmpULT(NewIndex, LoopEndCI), LoopBB, PostLoopBB);
    maybeAddUnrollAnnotation(Br);
  }

  uint64_t BytesCopied = LoopEndCount * LoopOpSize;
  uint64_t RemainingBytes = CopyLen->getZExtValue() - BytesCopied;
  if (RemainingBytes) {
    IRBuilder<> RBuilder(PostLoopBB ? PostLoopBB->getFirstNonPHI()
                                    : InsertBefore);

    SmallVector<Type *, 5> RemainingOps;
    TTI.getMemcpyLoopResidualLoweringType(RemainingOps, Ctx, RemainingBytes,
                                          SrcAS, DstAS, SrcAlign.value(),
                                          DstAlign.value(), AtomicElementSize);

    for (auto *OpTy : RemainingOps) {
      Align PartSrcAlign(commonAlignment(SrcAlign, BytesCopied));
      Align PartDstAlign(commonAlignment(DstAlign, BytesCopied));

      // Calculate the new index
      unsigned OperandSize = DL.getTypeStoreSize(OpTy);
      assert(
          (!AtomicElementSize || OperandSize % *AtomicElementSize == 0) &&
          "Atomic memcpy lowering is not supported for selected operand size");

      uint64_t GepIndex = BytesCopied / OperandSize;
      assert(GepIndex * OperandSize == BytesCopied &&
             "Division should have no Remainder!");
      // Cast source to operand type and load
      PointerType *SrcPtrType = PointerType::get(OpTy, SrcAS);
      Value *CastedSrc = SrcAddr->getType() == SrcPtrType
                             ? SrcAddr
                             : RBuilder.CreateBitCast(SrcAddr, SrcPtrType);
      Value *SrcGEP = RBuilder.CreateInBoundsGEP(
          OpTy, CastedSrc, ConstantInt::get(TypeOfCopyLen, GepIndex));
      LoadInst *Load =
          RBuilder.CreateAlignedLoad(OpTy, SrcGEP, PartSrcAlign, SrcIsVolatile);
      if (!CanOverlap) {
        // Set alias scope for loads.
        Load->setMetadata(LLVMContext::MD_alias_scope,
                          MDNode::get(Ctx, NewScope));
      }
      // Cast destination to operand type and store.
      PointerType *DstPtrType = PointerType::get(OpTy, DstAS);
      Value *CastedDst = DstAddr->getType() == DstPtrType
                             ? DstAddr
                             : RBuilder.CreateBitCast(DstAddr, DstPtrType);
      Value *DstGEP = RBuilder.CreateInBoundsGEP(
          OpTy, CastedDst, ConstantInt::get(TypeOfCopyLen, GepIndex));
      StoreInst *Store = RBuilder.CreateAlignedStore(Load, DstGEP, PartDstAlign,
                                                     DstIsVolatile);
      if (!CanOverlap) {
        // Indicate that stores don't overlap loads.
        Store->setMetadata(LLVMContext::MD_noalias, MDNode::get(Ctx, NewScope));
      }
      if (AtomicElementSize) {
        Load->setAtomic(AtomicOrdering::Unordered);
        Store->setAtomic(AtomicOrdering::Unordered);
      }
      BytesCopied += OperandSize;
    }
  }
  assert(BytesCopied == CopyLen->getZExtValue() &&
         "Bytes copied should match size in the call!");
}

void llvm::createMemCpyLoopUnknownSize(
    Instruction *InsertBefore, Value *SrcAddr, Value *DstAddr, Value *CopyLen,
    Align SrcAlign, Align DstAlign, bool SrcIsVolatile, bool DstIsVolatile,
    bool CanOverlap, const TargetTransformInfo &TTI,
    std::optional<uint32_t> AtomicElementSize) {
  BasicBlock *PreLoopBB = InsertBefore->getParent();
  BasicBlock *PostLoopBB =
      PreLoopBB->splitBasicBlock(InsertBefore, "post-loop-memcpy-expansion");

  Function *ParentFunc = PreLoopBB->getParent();
  const DataLayout &DL = ParentFunc->getParent()->getDataLayout();
  LLVMContext &Ctx = PreLoopBB->getContext();
  MDBuilder MDB(Ctx);
  MDNode *NewDomain = MDB.createAnonymousAliasScopeDomain("MemCopyDomain");
  StringRef Name = "MemCopyAliasScope";
  MDNode *NewScope = MDB.createAnonymousAliasScope(NewDomain, Name);

  unsigned SrcAS = cast<PointerType>(SrcAddr->getType())->getAddressSpace();
  unsigned DstAS = cast<PointerType>(DstAddr->getType())->getAddressSpace();

  Type *LoopOpType = TTI.getMemcpyLoopLoweringType(
      Ctx, CopyLen, SrcAS, DstAS, SrcAlign.value(), DstAlign.value(),
      AtomicElementSize);
  if (LowerToNonI8Type)
    LoopOpType = determineUnderlyingType(LoopOpType, SrcAddr, DstAddr);
  assert((!AtomicElementSize || !LoopOpType->isVectorTy()) &&
         "Atomic memcpy lowering is not supported for vector operand type");
  unsigned LoopOpSize = DL.getTypeStoreSize(LoopOpType);
  assert((!AtomicElementSize || LoopOpSize % *AtomicElementSize == 0) &&
         "Atomic memcpy lowering is not supported for selected operand size");

  IRBuilder<> PLBuilder(PreLoopBB->getTerminator());

  PointerType *SrcOpType = PointerType::get(LoopOpType, SrcAS);
  PointerType *DstOpType = PointerType::get(LoopOpType, DstAS);
  if (SrcAddr->getType() != SrcOpType) {
    SrcAddr = PLBuilder.CreateBitCast(SrcAddr, SrcOpType);
  }
  if (DstAddr->getType() != DstOpType) {
    DstAddr = PLBuilder.CreateBitCast(DstAddr, DstOpType);
  }

  // Calculate the loop trip count, and remaining bytes to copy after the loop.
  Type *CopyLenType = CopyLen->getType();
  IntegerType *ILengthType = dyn_cast<IntegerType>(CopyLenType);
  assert(ILengthType &&
         "expected size argument to memcpy to be an integer type!");
  Type *Int8Type = Type::getInt8Ty(Ctx);
  bool LoopOpIsInt8 = LoopOpType == Int8Type;
  ConstantInt *CILoopOpSize = ConstantInt::get(ILengthType, LoopOpSize);
  Value *RuntimeLoopCount = LoopOpIsInt8 ?
                            CopyLen :
                            PLBuilder.CreateUDiv(CopyLen, CILoopOpSize);
  BasicBlock *LoopBB =
      BasicBlock::Create(Ctx, "loop-memcpy-expansion", ParentFunc, PostLoopBB);
  IRBuilder<> LoopBuilder(LoopBB);

  Align PartSrcAlign(commonAlignment(SrcAlign, LoopOpSize));
  Align PartDstAlign(commonAlignment(DstAlign, LoopOpSize));

  PHINode *LoopIndex = LoopBuilder.CreatePHI(CopyLenType, 2, "loop-index");
  LoopIndex->addIncoming(ConstantInt::get(CopyLenType, 0U), PreLoopBB);

  Value *SrcGEP = LoopBuilder.CreateInBoundsGEP(LoopOpType, SrcAddr, LoopIndex);
  LoadInst *Load = LoopBuilder.CreateAlignedLoad(LoopOpType, SrcGEP,
                                                 PartSrcAlign, SrcIsVolatile);
  if (!CanOverlap) {
    // Set alias scope for loads.
    Load->setMetadata(LLVMContext::MD_alias_scope, MDNode::get(Ctx, NewScope));
  }
  Value *DstGEP = LoopBuilder.CreateInBoundsGEP(LoopOpType, DstAddr, LoopIndex);
  StoreInst *Store =
      LoopBuilder.CreateAlignedStore(Load, DstGEP, PartDstAlign, DstIsVolatile);
  if (!CanOverlap) {
    // Indicate that stores don't overlap loads.
    Store->setMetadata(LLVMContext::MD_noalias, MDNode::get(Ctx, NewScope));
  }
  if (AtomicElementSize) {
    Load->setAtomic(AtomicOrdering::Unordered);
    Store->setAtomic(AtomicOrdering::Unordered);
  }
  Value *NewIndex =
      LoopBuilder.CreateAdd(LoopIndex, ConstantInt::get(CopyLenType, 1U));
  LoopIndex->addIncoming(NewIndex, LoopBB);

  bool requiresResidual =
      !LoopOpIsInt8 && !(AtomicElementSize && LoopOpSize == AtomicElementSize);
  if (requiresResidual) {
    Type *ResLoopOpType = AtomicElementSize
                              ? Type::getIntNTy(Ctx, *AtomicElementSize * 8)
                              : Int8Type;
    unsigned ResLoopOpSize = DL.getTypeStoreSize(ResLoopOpType);
    assert((ResLoopOpSize == AtomicElementSize ? *AtomicElementSize : 1) &&
           "Store size is expected to match type size");

    // Add in the
    Value *RuntimeResidual = PLBuilder.CreateURem(CopyLen, CILoopOpSize);
    Value *RuntimeBytesCopied = PLBuilder.CreateSub(CopyLen, RuntimeResidual);

    // Loop body for the residual copy.
    BasicBlock *ResLoopBB = BasicBlock::Create(Ctx, "loop-memcpy-residual",
                                               PreLoopBB->getParent(),
                                               PostLoopBB);
    // Residual loop header.
    BasicBlock *ResHeaderBB = BasicBlock::Create(
        Ctx, "loop-memcpy-residual-header", PreLoopBB->getParent(), nullptr);

    // Need to update the pre-loop basic block to branch to the correct place.
    // branch to the main loop if the count is non-zero, branch to the residual
    // loop if the copy size is smaller then 1 iteration of the main loop but
    // non-zero and finally branch to after the residual loop if the memcpy
    //  size is zero.
    ConstantInt *Zero = ConstantInt::get(ILengthType, 0U);
    PLBuilder.CreateCondBr(PLBuilder.CreateICmpNE(RuntimeLoopCount, Zero),
                           LoopBB, ResHeaderBB);
    PreLoopBB->getTerminator()->eraseFromParent();

    LoopBuilder.CreateCondBr(
        LoopBuilder.CreateICmpULT(NewIndex, RuntimeLoopCount), LoopBB,
        ResHeaderBB);

    // Determine if we need to branch to the residual loop or bypass it.
    IRBuilder<> RHBuilder(ResHeaderBB);
    RHBuilder.CreateCondBr(RHBuilder.CreateICmpNE(RuntimeResidual, Zero),
                           ResLoopBB, PostLoopBB);

    // Copy the residual with single byte load/store loop.
    IRBuilder<> ResBuilder(ResLoopBB);
    PHINode *ResidualIndex =
        ResBuilder.CreatePHI(CopyLenType, 2, "residual-loop-index");
    ResidualIndex->addIncoming(Zero, ResHeaderBB);

    Value *SrcAsResLoopOpType = ResBuilder.CreateBitCast(
        SrcAddr, PointerType::get(ResLoopOpType, SrcAS));
    Value *DstAsResLoopOpType = ResBuilder.CreateBitCast(
        DstAddr, PointerType::get(ResLoopOpType, DstAS));
    Value *FullOffset = ResBuilder.CreateAdd(RuntimeBytesCopied, ResidualIndex);
    Value *SrcGEP = ResBuilder.CreateInBoundsGEP(
        ResLoopOpType, SrcAsResLoopOpType, FullOffset);
    LoadInst *Load = ResBuilder.CreateAlignedLoad(ResLoopOpType, SrcGEP,
                                                  PartSrcAlign, SrcIsVolatile);
    if (!CanOverlap) {
      // Set alias scope for loads.
      Load->setMetadata(LLVMContext::MD_alias_scope,
                        MDNode::get(Ctx, NewScope));
    }
    Value *DstGEP = ResBuilder.CreateInBoundsGEP(
        ResLoopOpType, DstAsResLoopOpType, FullOffset);
    StoreInst *Store = ResBuilder.CreateAlignedStore(Load, DstGEP, PartDstAlign,
                                                     DstIsVolatile);
    if (!CanOverlap) {
      // Indicate that stores don't overlap loads.
      Store->setMetadata(LLVMContext::MD_noalias, MDNode::get(Ctx, NewScope));
    }
    if (AtomicElementSize) {
      Load->setAtomic(AtomicOrdering::Unordered);
      Store->setAtomic(AtomicOrdering::Unordered);
    }
    Value *ResNewIndex = ResBuilder.CreateAdd(
        ResidualIndex, ConstantInt::get(CopyLenType, ResLoopOpSize));
    ResidualIndex->addIncoming(ResNewIndex, ResLoopBB);

    // Create the loop branch condition.
    ResBuilder.CreateCondBr(
        ResBuilder.CreateICmpULT(ResNewIndex, RuntimeResidual), ResLoopBB,
        PostLoopBB);
  } else {
    // In this case the loop operand type was a byte, and there is no need for a
    // residual loop to copy the remaining memory after the main loop.
    // We do however need to patch up the control flow by creating the
    // terminators for the preloop block and the memcpy loop.
    ConstantInt *Zero = ConstantInt::get(ILengthType, 0U);
    PLBuilder.CreateCondBr(PLBuilder.CreateICmpNE(RuntimeLoopCount, Zero),
                           LoopBB, PostLoopBB);
    PreLoopBB->getTerminator()->eraseFromParent();
    LoopBuilder.CreateCondBr(
        LoopBuilder.CreateICmpULT(NewIndex, RuntimeLoopCount), LoopBB,
        PostLoopBB);
  }
}

// Lower memmove to IR. memmove is required to correctly copy overlapping memory
// regions; therefore, it has to check the relative positions of the source and
// destination pointers and choose the copy direction accordingly.
//
// The code below is an IR rendition of this C function:
//
// void* memmove(void* dst, const void* src, size_t n) {
//   unsigned char* d = dst;
//   const unsigned char* s = src;
//   if (s < d) {
//     // copy backwards
//     while (n--) {
//       d[n] = s[n];
//     }
//   } else {
//     // copy forward
//     for (size_t i = 0; i < n; ++i) {
//       d[i] = s[i];
//     }
//   }
//   return dst;
// }
static void createMemMoveLoop(Instruction *InsertBefore, Value *SrcAddr,
                              Value *DstAddr, Value *CopyLen, Align SrcAlign,
                              Align DstAlign, bool SrcIsVolatile,
                              bool DstIsVolatile) {
  Type *TypeOfCopyLen = CopyLen->getType();
  BasicBlock *OrigBB = InsertBefore->getParent();
  Function *F = OrigBB->getParent();
  const DataLayout &DL = F->getParent()->getDataLayout();

  // TODO: Use different element type if possible?
  IRBuilder<> CastBuilder(InsertBefore);
  Type *EltTy = CastBuilder.getInt8Ty();
  Type *PtrTy =
      CastBuilder.getInt8PtrTy(SrcAddr->getType()->getPointerAddressSpace());
  SrcAddr = CastBuilder.CreateBitCast(SrcAddr, PtrTy);
  DstAddr = CastBuilder.CreateBitCast(DstAddr, PtrTy);
  if (LowerToNonI8Type) {
    Type* NewTy = determineUnderlyingType(EltTy, SrcAddr, DstAddr);
    if (EltTy != NewTy) {
      SrcAddr = Skip1BitCast(SrcAddr);
      DstAddr = Skip1BitCast(DstAddr);
    }
    EltTy = NewTy;
  }

  // Create the a comparison of src and dst, based on which we jump to either
  // the forward-copy part of the function (if src >= dst) or the backwards-copy
  // part (if src < dst).
  // SplitBlockAndInsertIfThenElse conveniently creates the basic if-then-else
  // structure. Its block terminators (unconditional branches) are replaced by
  // the appropriate conditional branches when the loop is built.
  ICmpInst *PtrCompare = new ICmpInst(InsertBefore, ICmpInst::ICMP_ULT,
                                      SrcAddr, DstAddr, "compare_src_dst");
  Instruction *ThenTerm, *ElseTerm;
  SplitBlockAndInsertIfThenElse(PtrCompare, InsertBefore, &ThenTerm,
                                &ElseTerm);

  // Each part of the function consists of two blocks:
  //   copy_backwards:        used to skip the loop when n == 0
  //   copy_backwards_loop:   the actual backwards loop BB
  //   copy_forward:          used to skip the loop when n == 0
  //   copy_forward_loop:     the actual forward loop BB
  BasicBlock *CopyBackwardsBB = ThenTerm->getParent();
  CopyBackwardsBB->setName("copy_backwards");
  BasicBlock *CopyForwardBB = ElseTerm->getParent();
  CopyForwardBB->setName("copy_forward");
  BasicBlock *ExitBB = InsertBefore->getParent();
  ExitBB->setName("memmove_done");

  unsigned PartSize = DL.getTypeStoreSize(EltTy);
  Align PartSrcAlign(commonAlignment(SrcAlign, PartSize));
  Align PartDstAlign(commonAlignment(DstAlign, PartSize));

  // Initial comparison of n == 0 that lets us skip the loops altogether. Shared
  // between both backwards and forward copy clauses.
  ICmpInst *CompareN =
      new ICmpInst(OrigBB->getTerminator(), ICmpInst::ICMP_EQ, CopyLen,
                   ConstantInt::get(TypeOfCopyLen, 0), "compare_n_to_0");

  // Copying backwards.
  BasicBlock *LoopBB =
    BasicBlock::Create(F->getContext(), "copy_backwards_loop", F, CopyForwardBB);
  IRBuilder<> LoopBuilder(LoopBB);
  PHINode *LoopPhi = LoopBuilder.CreatePHI(TypeOfCopyLen, 0);
  Value *IndexPtr = LoopBuilder.CreateSub(
      LoopPhi, ConstantInt::get(TypeOfCopyLen, 1), "index_ptr");
  Value *Element = LoopBuilder.CreateAlignedLoad(
      EltTy, LoopBuilder.CreateInBoundsGEP(EltTy, SrcAddr, IndexPtr),
      PartSrcAlign, "element");
  LoopBuilder.CreateAlignedStore(
      Element, LoopBuilder.CreateInBoundsGEP(EltTy, DstAddr, IndexPtr),
      PartDstAlign);
  LoopBuilder.CreateCondBr(
      LoopBuilder.CreateICmpEQ(IndexPtr, ConstantInt::get(TypeOfCopyLen, 0)),
      ExitBB, LoopBB);
  LoopPhi->addIncoming(IndexPtr, LoopBB);
  LoopPhi->addIncoming(CopyLen, CopyBackwardsBB);
  BranchInst *Br = BranchInst::Create(ExitBB, LoopBB, CompareN, ThenTerm);
  maybeAddUnrollAnnotation(Br);
  ThenTerm->eraseFromParent();

  // Copying forward.
  BasicBlock *FwdLoopBB =
    BasicBlock::Create(F->getContext(), "copy_forward_loop", F, ExitBB);
  IRBuilder<> FwdLoopBuilder(FwdLoopBB);
  PHINode *FwdCopyPhi = FwdLoopBuilder.CreatePHI(TypeOfCopyLen, 0, "index_ptr");
  Value *SrcGEP = FwdLoopBuilder.CreateInBoundsGEP(EltTy, SrcAddr, FwdCopyPhi);
  Value *FwdElement =
      FwdLoopBuilder.CreateAlignedLoad(EltTy, SrcGEP, PartSrcAlign, "element");
  Value *DstGEP = FwdLoopBuilder.CreateInBoundsGEP(EltTy, DstAddr, FwdCopyPhi);
  FwdLoopBuilder.CreateAlignedStore(FwdElement, DstGEP, PartDstAlign);
  Value *FwdIndexPtr = FwdLoopBuilder.CreateAdd(
      FwdCopyPhi, ConstantInt::get(TypeOfCopyLen, 1), "index_increment");
  FwdLoopBuilder.CreateCondBr(FwdLoopBuilder.CreateICmpEQ(FwdIndexPtr, CopyLen),
                              ExitBB, FwdLoopBB);
  FwdCopyPhi->addIncoming(FwdIndexPtr, FwdLoopBB);
  FwdCopyPhi->addIncoming(ConstantInt::get(TypeOfCopyLen, 0), CopyForwardBB);

  Br = BranchInst::Create(ExitBB, FwdLoopBB, CompareN, ElseTerm);
  maybeAddUnrollAnnotation(Br);
  ElseTerm->eraseFromParent();
}

static void createMemSetLoop(Instruction *InsertBefore, Value *DstAddr,
                             Value *CopyLen, Value *SetValue, Align DstAlign,
                             bool IsVolatile) {
  if (LowerToNonI8Type) {
    Type *OpTy = determineUnderlyingType(SetValue->getType(), DstAddr);
    if (OpTy != SetValue->getType())
      if (auto *CI = dyn_cast<ConstantInt>(SetValue)) {
        SetValue = emitBytePatternForType(OpTy, CI, *InsertBefore->getModule());
      }
  }

  Type *TypeOfCopyLen = CopyLen->getType();
  BasicBlock *OrigBB = InsertBefore->getParent();
  Function *F = OrigBB->getParent();
  const DataLayout &DL = F->getParent()->getDataLayout();
  BasicBlock *NewBB =
      OrigBB->splitBasicBlock(InsertBefore, "split");
  BasicBlock *LoopBB
    = BasicBlock::Create(F->getContext(), "loadstoreloop", F, NewBB);

  IRBuilder<> Builder(OrigBB->getTerminator());

  // Cast pointer to the type of value getting stored
  unsigned dstAS = cast<PointerType>(DstAddr->getType())->getAddressSpace();
  DstAddr = Builder.CreateBitCast(DstAddr,
                                  PointerType::get(SetValue->getType(), dstAS));

  Builder.CreateCondBr(
      Builder.CreateICmpEQ(ConstantInt::get(TypeOfCopyLen, 0), CopyLen), NewBB,
      LoopBB);
  OrigBB->getTerminator()->eraseFromParent();

  unsigned PartSize = DL.getTypeStoreSize(SetValue->getType());
  Align PartAlign(commonAlignment(DstAlign, PartSize));

  IRBuilder<> LoopBuilder(LoopBB);
  PHINode *LoopIndex = LoopBuilder.CreatePHI(TypeOfCopyLen, 0);
  LoopIndex->addIncoming(ConstantInt::get(TypeOfCopyLen, 0), OrigBB);

  LoopBuilder.CreateAlignedStore(
      SetValue,
      LoopBuilder.CreateInBoundsGEP(SetValue->getType(), DstAddr, LoopIndex),
      PartAlign, IsVolatile);

  Value *NewIndex =
      LoopBuilder.CreateAdd(LoopIndex, ConstantInt::get(TypeOfCopyLen, 1));
  LoopIndex->addIncoming(NewIndex, LoopBB);

  BranchInst *Br = LoopBuilder.CreateCondBr(
      LoopBuilder.CreateICmpULT(NewIndex, CopyLen), LoopBB, NewBB);
  maybeAddUnrollAnnotation(Br);
}

template <typename T>
static bool canOverlap(MemTransferBase<T> *Memcpy, ScalarEvolution *SE) {
  if (SE) {
    auto *SrcSCEV = SE->getSCEV(Memcpy->getRawSource());
    auto *DestSCEV = SE->getSCEV(Memcpy->getRawDest());
    if (SE->isKnownPredicateAt(CmpInst::ICMP_NE, SrcSCEV, DestSCEV, Memcpy))
      return false;
  }
  return true;
}

void llvm::expandMemCpyAsLoop(MemCpyInst *Memcpy,
                              const TargetTransformInfo &TTI,
                              ScalarEvolution *SE) {
  bool CanOverlap = canOverlap(Memcpy, SE);
  if (ConstantInt *CI = dyn_cast<ConstantInt>(Memcpy->getLength())) {
    createMemCpyLoopKnownSize(
        /* InsertBefore */ Memcpy,
        /* SrcAddr */ Memcpy->getRawSource(),
        /* DstAddr */ Memcpy->getRawDest(),
        /* CopyLen */ CI,
        /* SrcAlign */ Memcpy->getSourceAlign().valueOrOne(),
        /* DestAlign */ Memcpy->getDestAlign().valueOrOne(),
        /* SrcIsVolatile */ Memcpy->isVolatile(),
        /* DstIsVolatile */ Memcpy->isVolatile(),
        /* CanOverlap */ CanOverlap,
        /* TargetTransformInfo */ TTI);
  } else {
    createMemCpyLoopUnknownSize(
        /* InsertBefore */ Memcpy,
        /* SrcAddr */ Memcpy->getRawSource(),
        /* DstAddr */ Memcpy->getRawDest(),
        /* CopyLen */ Memcpy->getLength(),
        /* SrcAlign */ Memcpy->getSourceAlign().valueOrOne(),
        /* DestAlign */ Memcpy->getDestAlign().valueOrOne(),
        /* SrcIsVolatile */ Memcpy->isVolatile(),
        /* DstIsVolatile */ Memcpy->isVolatile(),
        /* CanOverlap */ CanOverlap,
        /* TargetTransformInfo */ TTI);
  }
}

void llvm::expandMemMoveAsLoop(MemMoveInst *Memmove) {
  createMemMoveLoop(/* InsertBefore */ Memmove,
                    /* SrcAddr */ Memmove->getRawSource(),
                    /* DstAddr */ Memmove->getRawDest(),
                    /* CopyLen */ Memmove->getLength(),
                    /* SrcAlign */ Memmove->getSourceAlign().valueOrOne(),
                    /* DestAlign */ Memmove->getDestAlign().valueOrOne(),
                    /* SrcIsVolatile */ Memmove->isVolatile(),
                    /* DstIsVolatile */ Memmove->isVolatile());
}

void llvm::expandMemSetAsLoop(MemSetInst *Memset) {
  createMemSetLoop(/* InsertBefore */ Memset,
                   /* DstAddr */ Memset->getRawDest(),
                   /* CopyLen */ Memset->getLength(),
                   /* SetValue */ Memset->getValue(),
                   /* Alignment */ Memset->getDestAlign().valueOrOne(),
                   Memset->isVolatile());
}

void llvm::expandAtomicMemCpyAsLoop(AtomicMemCpyInst *AtomicMemcpy,
                                    const TargetTransformInfo &TTI,
                                    ScalarEvolution *SE) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(AtomicMemcpy->getLength())) {
    createMemCpyLoopKnownSize(
        /* InsertBefore */ AtomicMemcpy,
        /* SrcAddr */ AtomicMemcpy->getRawSource(),
        /* DstAddr */ AtomicMemcpy->getRawDest(),
        /* CopyLen */ CI,
        /* SrcAlign */ AtomicMemcpy->getSourceAlign().valueOrOne(),
        /* DestAlign */ AtomicMemcpy->getDestAlign().valueOrOne(),
        /* SrcIsVolatile */ AtomicMemcpy->isVolatile(),
        /* DstIsVolatile */ AtomicMemcpy->isVolatile(),
        /* CanOverlap */ false, // SrcAddr & DstAddr may not overlap by spec.
        /* TargetTransformInfo */ TTI,
        /* AtomicCpySize */ AtomicMemcpy->getElementSizeInBytes());
  } else {
    createMemCpyLoopUnknownSize(
        /* InsertBefore */ AtomicMemcpy,
        /* SrcAddr */ AtomicMemcpy->getRawSource(),
        /* DstAddr */ AtomicMemcpy->getRawDest(),
        /* CopyLen */ AtomicMemcpy->getLength(),
        /* SrcAlign */ AtomicMemcpy->getSourceAlign().valueOrOne(),
        /* DestAlign */ AtomicMemcpy->getDestAlign().valueOrOne(),
        /* SrcIsVolatile */ AtomicMemcpy->isVolatile(),
        /* DstIsVolatile */ AtomicMemcpy->isVolatile(),
        /* CanOverlap */ false, // SrcAddr & DstAddr may not overlap by spec.
        /* TargetTransformInfo */ TTI,
        /* AtomicCpySize */ AtomicMemcpy->getElementSizeInBytes());
  }
}
