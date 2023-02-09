//==--------- xrt_alloc.hpp - SYCL XRT Allocator ---------------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// When using Xilinx/AMD FPGA through the OpenCL backend, if buffer allocation
// are not aligned on 4096 bytes, XRT will perform an extra memcpy. This file
// provides a SYCL allocator with the proper alignment.
//
//===----------------------------------------------------------------------===//

#pragma once
#include <sycl/detail/defines_elementary.hpp>

namespace sycl {
__SYCL_INLINE_VER_NAMESPACE(_V1) {
namespace ext::xilinx {

struct xrt_alloc : public aligned_allocator<char> {
  xrt_alloc() : aligned_allocator<char>(4096) {
  }
};

} // namespace ext::xilinx
}
}  // namespace sycl
