//===- SYCLUtils.h --------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Shared utilities between the various SYCL passes.
//
// ===---------------------------------------------------------------------===//

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace llvm {

/// Remove a list of attributes from an IR module.
void removeAttributes(Module &M, ArrayRef<Attribute::AttrKind> Kinds);

/// Remove a global metadata from a module.
void removeMetadata(Module &M, StringRef MetadataName);

/// Replace the function named OldN by the function named NewN then delete the
/// function named OldN.
void replaceFunction(Module &M, StringRef OldN, StringRef NewN);

/// Test if a function is a kernel
bool isKernelFunc(const Function* F);

} // namespace llvm
