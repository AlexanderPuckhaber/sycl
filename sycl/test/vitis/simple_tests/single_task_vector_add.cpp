// REQUIRES: vitis

// RUN: rm -rf %t.dir && mkdir %t.dir && cd %t.dir
// RUN: %clangxx %EXTRA_COMPILE_FLAGS-std=c++20 -fsycl -fsycl-targets=%sycl_triple %s -o %t.dir/exec.out
// RUN: %ACC_RUN_PLACEHOLDER %t.dir/exec.out
// Test that we can get a fast reports by adding --vitis-ip-part
// RUN: %clangxx %EXTRA_COMPILE_FLAGS-std=c++20 -fsycl -fsycl-targets=%sycl_triple %s -o %t.dir/exec2.out --vitis-ip-part=xcu200-fsgd2104-2-e

/*
   A simple typical FPGA-like kernel adding 2 vectors
*/
#include <sycl/sycl.hpp>
#include <iostream>
#include <numeric>


using namespace sycl;

constexpr size_t N = 300;
using Type = int;

int main(int argc, char *argv[]) {
  buffer<Type> a { N };
  buffer<Type> b { N };
  buffer<Type> c { N };

  {
    sycl::host_accessor a_b{b, sycl::write_only};
    // Initialize buffer with increasing numbers starting at 0
    std::iota(&a_b[0], &a_b[a_b.size()], 0);
  }

  {
    sycl::host_accessor a_c{c, sycl::write_only};
    // Initialize buffer with increasing numbers starting at 5
    std::iota(&a_c[0], &a_c[a_c.size()], 5);
  }

  queue q;

  std::cout << "Queue Device: " << q.get_device().get_info<info::device::name>() << std::endl;
  std::cout << "Queue Device Vendor: " << q.get_device().get_info<info::device::vendor>() << std::endl;

  // Launch a kernel to do the summation
  q.submit([&] (handler &cgh) {
      // Get access to the data
      sycl::accessor a_a{a, cgh, sycl::write_only};
      sycl::accessor a_b{b, cgh, sycl::read_only};
      sycl::accessor a_c{c, cgh, sycl::read_only};

      // A typical FPGA-style pipelined kernel
      cgh.single_task<class add>([=] () {
          // Use an intermediate automatic array
          int array[N];
          for (unsigned int i = 0 ; i < N; ++i)
            array[i] = a_b[i];
          for (unsigned int i = 0 ; i < N; ++i)
            a_a[i] = array[i] + a_c[i];
        });
    });

  // Verify the result
  sycl::host_accessor a_a{a, sycl::read_only};
  for (unsigned int i = 0 ; i < a.size(); ++i) {
    assert(a_a[i] == 5 + 2*i && "invalid result from kernel");
  }

  return 0;
}
