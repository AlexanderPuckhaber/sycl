// REQUIRES: vitis

// RUN: rm -rf %t.dir && mkdir %t.dir && cd %t.dir
// RUN: %clangxx %EXTRA_COMPILE_FLAGS-std=c++20 -fsycl -fsycl-targets=%sycl_triple %s -o %t.dir/exec.out
// RUN: %ACC_RUN_PLACEHOLDER %t.dir/exec.out

/*
  Testing if constexpr values carry across as expected from the host to the
  device e.g. as a compile time hardcoded value and not by value copy
  (trivially testable at the moment as by value capture and transfer appears
  to be broken).
*/

#include <sycl/sycl.hpp>


using namespace sycl;

int add(int v1, int v2) {
  return v1 + v2;
}

int main() {
  queue q;

  constexpr int host_to_device = 20;
  int try_capture = 30;

  printf("host host_to_device before: %d \n", host_to_device);
  printf("host try_capture before: %d \n", try_capture);
  printf("host add before: %d \n", add(host_to_device, try_capture));

  buffer<int> ob(range<1>{3});

  q.submit([&](handler &cgh) {
      sycl::accessor wb{ob, cgh, sycl::write_only};

      cgh.single_task<class constexpr_carryover>([=] {
        wb[0] = host_to_device;
        wb[1] = try_capture;
        wb[2] = add(host_to_device, try_capture);
      });
  });

  q.wait();

  sycl::host_accessor rb{ob, sycl::read_only};

  printf("host host_to_device after: %d \n", rb[0]);
  printf("host try_capture after: %d \n", rb[1]);
  printf("host add after: %d \n", rb[2]);

  return 0;
}
