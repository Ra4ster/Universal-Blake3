# Universal Blake3

<div align="center">
    <img width="500" src="https://itzmeanjan.in/images/blake3_on_gpgpu_0.jpg" alt="Blake3 Algorithm Visualied" />
    <hr />
</div>

This is a **header-only** implementation of [BLAKE3](<https://en.wikipedia.org/wiki/BLAKE_(hash_function)#BLAKE3>) with cross-platform, cross-CPU architecture compatibility. Whereas many implementations are well-optimized for certain flags (AVX-256, FMA, etc.) this implementation is optimized using [Google Highway](https://github.com/google/highway), an SIMD runtime which can dispatch to quite a lot of specialized CPU flags such as RISC-V (RVV), Diamond Rapids (AVX10_2), WebAssembly (WASM, WASM_EMU256) and more.

You can simply download `blake3-headers.zip` from the Releases page and drop it into your project.

## Usage Example

```cpp
#include "blake3.hpp"
#include <iostream>

int main(void) {
    blake3 b3;
    std::string hash;

   // Hashes a string, routes to SIMD automatically:
   b3.Update("Hello, World!");

   // Same as Update, but runs multithreaded:
   b3.UpdateParallel("Hello, World!");

   // Compute the final tree and get the hash:
   hash = b3.Hex();

   std::cout << hash << std::endl;
   return 0;
}
```

There are only 3 public methods to this class; it is extremely easy to use!

## Performance Results

I'll let these results speak for themselves.

|            | Universal Blake3        | Reference `blake3` 1.0.11 |
| ---------- | ----------------------- | ------------------------- |
| 1 thread   | 61–68 ms (3.9–4.4 GB/s) | 60–64 ms (4.2–4.5 GB/s)   |
| 20 threads | 9.2–9.9 ms (27–29 GB/s) | 8.1 ms (33 GB/s)          |

Single-threaded, this implementation matches the official BLAKE3 library, which uses hand-written AVX2 assembly on this CPU. Multithreaded, it comes within about 5%.

### Thread scaling

| Threads | Time    | Throughput |
| ------- | ------- | ---------- |
| 1       | 66.9 ms | 4.0 GB/s   |
| 2       | 34.1 ms | 7.9 GB/s   |
| 4       | 19.2 ms | 14.0 GB/s  |
| 6       | 13.7 ms | 19.6 GB/s  |
| 8       | 12.5 ms | 21.5 GB/s  |
| 12      | 10.9 ms | 24.6 GB/s  |
| 20      | 9.7 ms  | 27.7 GB/s  |

Scaling is close to linear across the first 6 performance cores.

### Test setup

- **CPU:** Intel Core i7-12700H (6 P-cores + 8 E-cores, 20 threads), AVX2
- **Compiler:** `icpx -O3 -DNDEBUG -march=native -std=c++20`
- **Single-threaded:** `Blake3::Update`
- **Multithreaded:** `Blake3::UpdateParallel`, which splits the input into 256 KiB subtrees hashed on a persistent thread pool
- **Reference:** the official BLAKE3 Rust implementation via the Python `blake3` package, timed on a buffer of the same size.

Run `make bench` to reproduce the benchmark on your own machine.

## Building

The library is extremely simple. The only dependency is Google Highway, so make sure to pull with submodules. After, the only thing left to do is run CMake!

```bash
git clone --recurse-submodules https://github.com/Ra4ster/Universal-Blake3

cd Universal-Blake3

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Or if you have make:
make
```

### Showcase

You can also test out the program and use it easily by running `./Blake3 "content to hash"`. Alternatively, you can enter a file path to hash its contents, using `./Blake3 --file file.name`.

Here are some useful hashes to test out:

- "": af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262
- "abc": 6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85
- "Hello, World": 21c71df4d665d75c3f3c4c42894ce40c33453d4d4d204df175686ab5c15dc4f1
- --file include/blake3.hpp: cea44b0f6c781c28c5c1b82cc9f95099eccf88d0b2d5a35c3e0994caada0578f _(The header library's hash of itself!)_

<div align="center">
    <img width="500" src="blake3_cli.png" alt="Blake3 running in the CLI"/>
</div>

---

If you are interested in knowing more about the developer, visit [Ra4ster](https://ra4ster.github.io).

_Thank you for the support!_ ❤️
