// REQUIRES: vitis

// RUN: %clangxx %EXTRA_COMPILE_FLAGS-fsycl -fsycl-targets=%sycl_triple %s -o %t.out
// RUN: %ACC_RUN_PLACEHOLDER %t.out

/*
  Test showcases a blocking event related to host accessor synchronization
  in sections 3.6.5.1 and 3.5, the attempt to get a second accessor will force
  a deadlock.

  To make this test work, uncomment the braces {} around the kernel invocations.
  This means there is only ever one host side to the accessor at the same time
  bypassing the problem.

  Note: This test probably shouldn't be ran as a test case as it will deadlock,
  but is useful to check regressive behaviour in the scheduler (e.g. scheduler
  1.0 this was legal and in scheduler 2.0 it's no longer legal) and check for
  correct interactions with the scheduler for various runtimes
*/

#include <sycl/sycl.hpp>
#include <iostream>


using namespace sycl;
class k1;
class k2;

int main() {
  queue q;

  std::cout << "Queue Device: " << q.get_device().get_info<info::device::name>() << std::endl;
  std::cout << "Queue Device Vendor: " << q.get_device().get_info<info::device::vendor>() << std::endl;
  std::cout << "Device is accelerator: " << q.get_device().is_accelerator() << std::endl;
  std::cout << "Device is GPU: " << q.get_device().is_gpu() << std::endl;
  std::cout << "Device is CPU: " << q.get_device().is_cpu() << std::endl;

  int arr[1]{0};
  sycl::buffer<int, 1> ob(arr, 1);
//{
  q.submit([&](handler &cgh) {
    sycl::accessor wb{ob, cgh, sycl::read_write};
    cgh.single_task<k1>([=] {
      wb[0] += 1;
    });
  });

  sycl::host_accessor rb{ob, sycl::read_only};
  std::cout << rb[0] << "\n";
//}

//{
  q.submit([&](handler &cgh) {
    sycl::accessor wb{ob, cgh, sycl::read_write};
    cgh.single_task<k2>([=] {
      wb[0] += 1;
    });
  });

  sycl::host_accessor rb2{ob, sycl::read_only};
  std::cout << rb2[0] << "\n";
//}

  return 0;
}
