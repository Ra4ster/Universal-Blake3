#include "hwy/highway.h"
#include <iostream>

namespace hn = hwy::HWY_NAMESPACE;

int main(void)
{
  const hn::ScalableTag<uint32_t> d;

  const auto a = hn::Set(d, 10);
  const auto b = hn::Set(d, 20);
  const auto c = hn::Add(a, b);

  alignas(64) uint32_t result[64] = {};
  hn::StoreU(c, d, result);

  std::cout << "Result: " << result[0] << std::endl;
  std::cout << "SIMD lanes: " << hn::Lanes(d) << std::endl;
  return 0;
}
