#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "blake3.hpp"

int main()
{
  constexpr size_t DATA_SIZE = 256ull * 1024 * 1024;
  constexpr int ITERATIONS = 10;

  // Generate 256 MiB of deterministic binary data.
  std::vector<uint8_t> data(DATA_SIZE);

  uint32_t state = 123456789u;
  for (size_t i = 0; i < data.size(); ++i)
  {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    data[i] = static_cast<uint8_t>(state);
  }

  std::vector<double> times;
  times.reserve(ITERATIONS);

  std::string digest;

  // Warm up the implementation.
  {
    Deep::Blake3 hasher;
    hasher.Update(data.data(), data.size());
    digest = hasher.Hex();
  }

  for (int i = 0; i < ITERATIONS; ++i)
  {
    const auto start = std::chrono::steady_clock::now();

    Deep::Blake3 hasher;
    hasher.Update(data.data(), data.size());
    digest = hasher.Hex();

    const auto end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();

    times.push_back(seconds);

    const double gbps = (DATA_SIZE / 1e9) / seconds;

    std::cout << "Run " << i + 1 << ": " << std::fixed << std::setprecision(3) << seconds * 1000.0
              << " ms | " << gbps << " GB/s\n";
  }

  double total = 0.0;
  for (double t : times)
    total += t;

  const double average = total / ITERATIONS;
  const double best = *std::min_element(times.begin(), times.end());

  std::cout << "\n=== BLAKE3 Highway Benchmark ===\n";
  std::cout << "Data size: 256 MiB\n";
  std::cout << "Iterations: " << ITERATIONS << '\n';
  std::cout << "Average time: " << average * 1000.0 << " ms\n";
  std::cout << "Best time: " << best * 1000.0 << " ms\n";
  std::cout << "Average throughput: " << (DATA_SIZE / 1e9) / average << " GB/s\n";
  std::cout << "Best throughput: " << (DATA_SIZE / 1e9) / best << " GB/s\n";
  std::cout << "Average throughput: " << (DATA_SIZE / 1073741824.0) / average << " GiB/s\n";
  std::cout << "Digest: " << digest << '\n';

  return 0;
}
