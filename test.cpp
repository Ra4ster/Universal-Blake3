#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "blake3.hpp"

int main(int argc, char* argv[])
{
  if (argc > 2)
    return 1;

  if (argc == 2)
  {
    const std::string input = argv[1];

    Deep::Blake3 hasher;
    hasher.Update(input.data(), input.size());

    std::cout << input << " hashed:\n\n" << hasher.Hex() << std::endl;
    return 0;
  }

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

  const auto bench = [&](const char* label, unsigned threads)
  {
    std::vector<double> times;
    times.reserve(ITERATIONS);

    std::string digest;

    const auto hash = [&]
    {
      Deep::Blake3 hasher;
      if (threads == 1)
        hasher.Update(data.data(), data.size());
      else
        hasher.UpdateParallel(data.data(), data.size(), threads);
      digest = hasher.Hex();
    };

    hash();

    for (int i = 0; i < ITERATIONS; ++i)
    {
      const auto start = std::chrono::steady_clock::now();

      hash();

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

    std::cout << "\n=== BLAKE3 Highway Benchmark (" << label << ") ===\n";
    std::cout << "Data size: 256 MiB\n";
    std::cout << "Iterations: " << ITERATIONS << '\n';
    std::cout << "Average time: " << average * 1000.0 << " ms\n";
    std::cout << "Best time: " << best * 1000.0 << " ms\n";
    std::cout << "Average throughput: " << (DATA_SIZE / 1e9) / average << " GB/s\n";
    std::cout << "Best throughput: " << (DATA_SIZE / 1e9) / best << " GB/s\n";
    std::cout << "Average throughput: " << (DATA_SIZE / 1073741824.0) / average << " GiB/s\n";
    std::cout << "Digest: " << digest << "\n\n";
  };

  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());

  bench("1 thread", 1);
  bench((std::to_string(threads) + " threads").c_str(), threads);

  return 0;
}
