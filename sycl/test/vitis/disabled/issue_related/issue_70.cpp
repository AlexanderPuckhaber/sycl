// RUN: true

/*

  Test Case/Issue supplied by J-Stephan in issue:
    https://github.com/triSYCL/sycl/issues/70

   Crashes in hw_emu synthesis when trying to comprehend C-style arrays.

*/

#include <sycl/sycl.hpp>

using namespace sycl;

class burst_test;

auto loop(
    sycl::accessor<std::size_t, 3, sycl::access::mode::read_write> s) {
  for (std::size_t z = 0; z < 42; ++z) {
    for (std::size_t y = 0; y < 42; ++y) {
      std::size_t in_row[42];
      std::size_t out_row[42];

      for (std::size_t x = 0; x < 42; ++x) {
        const auto id = sycl::id<3>{x, y, z};
        in_row[x] = s[id];
      }

      for (std::size_t x = 0; x < 42; ++x)
        out_row[x] = in_row[x] + 42;

      for (std::size_t x = 0; x < 42; ++x) {
        const auto id = sycl::id<3>{x, y, z};
        s[id] = out_row[x];
      }
    }
  }
}

auto main() -> int {
  queue q;

  auto s_buf = sycl::buffer<std::size_t, 3>{sycl::range<3>{42, 42, 42}};

  q.submit([&](sycl::handler &cgh) {
    sycl::accessor s{s_buf, cgh, sycl::read_write};

    cgh.single_task<burst_test>([=] { loop(s); });
  });
  q.wait();

  return 0;
}
