// REQUIRES: vitis

// RUN: %clangxx -fsycl -fsycl-targets=%sycl_triple -std=c++20 %s -S -emit-llvm -o %t.bundled.ll
// RUN: %clang_offload_bundler --unbundle --type=ll --targets=sycl-%sycl_triple --input %t.bundled.ll --output %t.ll
// RUN: %run_if_not_cpu FileCheck --input-file=%t.ll %s

#include <sycl/sycl.hpp>
#include <sycl/ext/xilinx/fpga.hpp>
#include <array>
#include <iostream>
#include <tuple>
#include <utility>

int main() {
  constexpr std::size_t len = 120;
  sycl::buffer<sycl::cl_int, 1> Buffer(len);
  sycl::queue Queue;
  Queue.submit([&](sycl::handler &cgh) {
    sycl::accessor Accessor{Buffer, cgh, sycl::write_only};
    cgh.single_task<class FirstKernel>(sycl::ext::xilinx::pipeline_kernel([=] {
      // CHECK-DAG: xilinx_kernel_property
      // CHECK-DAG: kernel_pipeline
      for (size_t i = 0; i < len; ++i)
        Accessor[i] = i;
    }));
  });
}
