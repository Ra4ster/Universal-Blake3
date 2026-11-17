#pragma once
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <hwy/highway.h>

namespace hn = hwy::HWY_NAMESPACE;

class Blake3
{
public:
  void Update(const void* data, size_t len) noexcept
  {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (len > 0)
    {
      if (buffer_len_ == 1024)
      {
        ProcessChunk();
        ++chunk_counter_;
        buffer_len_ = 0;
      }

      if (buffer_len_ == 0 && len > 1024)
      {
        const size_t complete_chunks = (len - 1) / 1024;

        ProcessChunksSIMD(ptr, complete_chunks);

        const size_t consumed = complete_chunks * 1024;
        ptr += consumed;
        len -= consumed;
        continue;
      }

      const size_t take = std::min(len, 1024 - buffer_len_);

      hwy::CopyBytes(ptr, chunk_buffer_ + buffer_len_, take);

      buffer_len_ += take;
      ptr += take;
      len -= take;
    }
  }

  void UpdateParallel(const void* data, size_t len, unsigned threads = 0)
  {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    if (threads == 0)
      threads = std::thread::hardware_concurrency();

    const size_t prefix = TaskAlignPrefix();

    if (threads > 1 && len > prefix && len - prefix > MIN_PARALLEL_TASKS * TASK_BYTES)
    {
      Update(ptr, prefix);
      ptr += prefix;
      len -= prefix;

      FlushFullBuffer();

      const size_t tasks = (len - 1) / TASK_BYTES;

      HashTasks(tasks, threads, [ptr](size_t i, size_t) noexcept { return ptr + i * TASK_BYTES; });

      ptr += tasks * TASK_BYTES;
      len -= tasks * TASK_BYTES;
    }

    Update(ptr, len);
  }

  // Hashes a whole file in parallel; returns false if it cannot be read
  bool UpdateFile(const char* path, unsigned threads = 0)
  {
    if (threads == 0)
      threads = std::thread::hardware_concurrency();

#if defined(__unix__) || defined(__APPLE__)
    const int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
      return false;

    struct stat st;
    bool ok = false;

    if (fstat(fd, &st) == 0)
    {
      // Size 0 still has content (like under /proc)
      if (S_ISREG(st.st_mode) && st.st_size > 0)
        ok = UpdateRegularFile(fd, static_cast<uint64_t>(st.st_size), threads);
      else
        ok = UpdateStream(
            [fd](uint8_t* dst, size_t n) noexcept -> ptrdiff_t
            {
              for (;;)
              {
                const ssize_t k = read(fd, dst, n);

                if (k >= 0 || errno != EINTR)
                  return static_cast<ptrdiff_t>(k);
              }
            },
            threads);
    }

    close(fd);
    return ok;
#else
    std::FILE* file = nullptr;

#if defined(_WIN32)
    if (fopen_s(&file, path, "rb") != 0)
      file = nullptr;
#else
    file = std::fopen(path, "rb");
#endif

    if (file == nullptr)
      return false;

    const bool ok = UpdateStream(
        [file](uint8_t* dst, size_t n) noexcept -> ptrdiff_t
        {
          const size_t k = std::fread(dst, 1, n, file);
          return (k == 0 && std::ferror(file)) ? -1 : static_cast<ptrdiff_t>(k);
        },
        threads);

    std::fclose(file);
    return ok;
#endif
  }

  std::string Hex() noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;

    V cv[8];
    V block[16];

    for (int j = 0; j < 8; ++j)
      cv[j] = hn::Set(d, default_cv[j]);

    const V counter_low = hn::Set(d, static_cast<uint32_t>(chunk_counter_));
    const V counter_high = hn::Set(d, static_cast<uint32_t>(chunk_counter_ >> 32));
    const size_t num_blocks = buffer_len_ == 0 ? 1 : (buffer_len_ + 63) / 64;

    for (size_t i = 0; i < num_blocks; ++i)
    {
      const size_t offset = i * 64;
      const size_t remaining = buffer_len_ > offset ? buffer_len_ - offset : 0;
      const size_t block_len = std::min<size_t>(remaining, 64);

      uint8_t bytes[64] = {};

      if (block_len > 0)
        hwy::CopyBytes(chunk_buffer_ + offset, bytes, block_len);

      for (int j = 0; j < 16; ++j)
      {
        uint32_t val;
        hwy::CopyBytes(bytes + j * 4, &val, sizeof(val));
        block[j] = hn::Set(d, val);
      }

      const bool first = (i == 0);
      const bool last = (i + 1 == num_blocks);

      uint32_t flags = (first ? CHUNK_START : 0u) | (last ? CHUNK_END : 0u);

      if (last && cv_stack_len_ == 0)
        flags |= ROOT;

      Compress<false>(d,
                      cv,
                      block,
                      counter_low,
                      counter_high,
                      hn::Set(d, static_cast<uint32_t>(block_len)),
                      hn::Set(d, flags));
    }

    size_t stack_len = cv_stack_len_;
    while (stack_len > 0)
    {
      --stack_len;

      const uint32_t* left = &cv_stack_[stack_len * 8];

      for (int j = 0; j < 8; ++j)
      {
        block[j] = hn::Set(d, left[j]);
        block[j + 8] = cv[j];
        cv[j] = hn::Set(d, default_cv[j]);
      }

      const uint32_t flags = PARENT | (stack_len == 0 ? ROOT : 0u);
      Compress<false>(d, cv, block, hn::Zero(d), hn::Zero(d), hn::Set(d, 64u), hn::Set(d, flags));
    }

    constexpr char hex_digits[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(64);

    for (int j = 0; j < 8; ++j)
    {
      const uint32_t word = hn::GetLane(cv[j]);

      for (int k = 0; k < 4; ++k)
      {
        const uint8_t byte = static_cast<uint8_t>(word >> (8 * k));

        hex.push_back(hex_digits[byte >> 4]);
        hex.push_back(hex_digits[byte & 0x0F]);
      }
    }

    return hex;
  }

private:
  static constexpr uint32_t default_cv[8] = {0x6A09E667,
                                             0xBB67AE85,
                                             0x3C6EF372,
                                             0xA54FF53A,
                                             0x510E527F,
                                             0x9B05688C,
                                             0x1F83D9AB,
                                             0x5BE0CD19};
  static constexpr uint8_t msg_schedule[7][16] = {
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
      {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
      {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
      {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
      {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
      {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
      {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}};
  static constexpr uint8_t cols[4][4] = {
      {0, 4, 8, 12}, {1, 5, 9, 13}, {2, 6, 10, 14}, {3, 7, 11, 15}};
  static constexpr uint8_t dia[4][4] = {
      {0, 5, 10, 15}, {1, 6, 11, 12}, {2, 7, 8, 13}, {3, 4, 9, 14}};

  static constexpr uint32_t CHUNK_START = 1u;
  static constexpr uint32_t CHUNK_END = 2u;
  static constexpr uint32_t PARENT = 4u;
  static constexpr uint32_t ROOT = 8u;
  static constexpr size_t MAX_LANES = 16;
  static constexpr size_t MAX_BATCHES = 16;
  static constexpr size_t TASK_CHUNKS = 256;
  static constexpr size_t TASK_BYTES = TASK_CHUNKS * 1024;
  static constexpr size_t MIN_PARALLEL_TASKS = 4;
  static constexpr size_t STREAM_BYTES = 64 * TASK_BYTES;
  static constexpr size_t MAX_BATCH_TASKS = 16384;

  // Bytes until the next parallel task boundary
  size_t TaskAlignPrefix() const noexcept
  {
    const size_t fill = buffer_len_ == 0 ? 0 : 1024 - buffer_len_;
    const uint64_t chunks_after = chunk_counter_ + (buffer_len_ == 0 ? 0 : 1);
    const size_t pad =
        static_cast<size_t>((TASK_CHUNKS - (chunks_after & (TASK_CHUNKS - 1))) & (TASK_CHUNKS - 1));

    return fill + pad * 1024;
  }

  void FlushFullBuffer() noexcept
  {
    if (buffer_len_ == 1024)
    {
      ProcessChunk();
      ++chunk_counter_;
      buffer_len_ = 0;
    }
  }

  // fetch(i, slot) returns task i's bytes, or nullptr on error; batched to bound memory
  template <class Fetch> void HashTasks(size_t tasks, unsigned threads, const Fetch& fetch)
  {
    const unsigned log2_task = static_cast<unsigned>(std::countr_zero(TASK_CHUNKS));

    std::vector<uint32_t> cvs(std::min(tasks, MAX_BATCH_TASKS) * 8);

    for (size_t first = 0; first < tasks; first += MAX_BATCH_TASKS)
    {
      const size_t count = std::min(tasks - first, MAX_BATCH_TASKS);
      const size_t workers = std::min<size_t>(threads, count);
      const uint64_t base = chunk_counter_;

      std::atomic<size_t> next{0};
      std::atomic<size_t> slots{0};

      const auto work = [&]() noexcept
      {
        const size_t slot = slots.fetch_add(1, std::memory_order_relaxed);

        for (;;)
        {
          const size_t i = next.fetch_add(1, std::memory_order_relaxed);

          if (i >= count)
            break;

          const uint8_t* const src = fetch(first + i, slot);

          if (src == nullptr)
            continue;

          if (OnEfficiencyCore())
            HashSubtree<true>(src, TASK_CHUNKS, base + i * TASK_CHUNKS, &cvs[i * 8]);
          else
            HashSubtree<false>(src, TASK_CHUNKS, base + i * TASK_CHUNKS, &cvs[i * 8]);
        }
      };

      ThreadPool::Instance().Run(workers - 1, work);

      for (size_t i = 0; i < count; ++i)
      {
        PushChainingValue(&cvs[i * 8], log2_task);
        chunk_counter_ += TASK_CHUNKS;
      }
    }
  }

  // For pipes: read(dst, n) returns bytes read, 0 at end, negative on error
  template <class Read> bool UpdateStream(const Read& read, unsigned threads)
  {
    const std::unique_ptr<uint8_t[]> buf(new uint8_t[STREAM_BYTES]);

    for (;;)
    {
      size_t got = 0;
      bool eof = false;

      while (got < STREAM_BYTES)
      {
        const ptrdiff_t k = read(buf.get() + got, STREAM_BYTES - got);

        if (k < 0)
          return false;

        if (k == 0)
        {
          eof = true;
          break;
        }

        got += static_cast<size_t>(k);
      }

      UpdateParallel(buf.get(), got, threads);

      if (eof)
        return true;
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  // Returns bytes read (fewer than n only at end of file), or -1 on error
  static ptrdiff_t ReadUpTo(int fd, uint8_t* dst, size_t n, uint64_t offset) noexcept
  {
    size_t got = 0;

    while (got < n)
    {
      const ssize_t k = pread(fd, dst + got, n - got, static_cast<off_t>(offset + got));

      if (k < 0 && errno == EINTR)
        continue;

      if (k < 0)
        return -1;

      if (k == 0)
        break;

      got += static_cast<size_t>(k);
    }

    return static_cast<ptrdiff_t>(got);
  }

  static bool ReadAt(int fd, uint8_t* dst, size_t n, uint64_t offset) noexcept
  {
    return ReadUpTo(fd, dst, n, offset) == static_cast<ptrdiff_t>(n);
  }

  // len is the reported size, which can be wrong, so the tail is read to end of file
  bool UpdateRegularFile(int fd, uint64_t len, unsigned threads)
  {
    const std::unique_ptr<uint8_t[]> scratch(new uint8_t[TASK_BYTES]);
    const size_t prefix = TaskAlignPrefix();
    uint64_t offset = 0;

    if (threads > 1 && len > prefix && len - prefix > MIN_PARALLEL_TASKS * TASK_BYTES)
    {
      if (!ReadAt(fd, scratch.get(), prefix, offset))
        return false;

      Update(scratch.get(), prefix);
      offset += prefix;
      len -= prefix;

      FlushFullBuffer();

      const size_t tasks = static_cast<size_t>((len - 1) / TASK_BYTES);
      const size_t workers = std::min<size_t>(threads, tasks);
      const std::unique_ptr<uint8_t[]> bufs(new uint8_t[workers * TASK_BYTES]);
      std::atomic<bool> failed{false};

      HashTasks(tasks,
                threads,
                [&](size_t i, size_t slot) noexcept -> const uint8_t*
                {
                  uint8_t* const dst = bufs.get() + slot * TASK_BYTES;

                  if (!ReadAt(fd, dst, TASK_BYTES, offset + i * TASK_BYTES))
                  {
                    failed.store(true, std::memory_order_relaxed);
                    return nullptr;
                  }

                  return dst;
                });

      if (failed.load(std::memory_order_relaxed))
        return false;

      offset += static_cast<uint64_t>(tasks) * TASK_BYTES;
    }

    for (;;)
    {
      const ptrdiff_t n = ReadUpTo(fd, scratch.get(), TASK_BYTES, offset);

      if (n < 0)
        return false;

      Update(scratch.get(), static_cast<size_t>(n));
      offset += static_cast<uint64_t>(n);

      if (static_cast<size_t>(n) < TASK_BYTES)
        return true;
    }
  }
#endif

  class ThreadPool
  {
  public:
    static ThreadPool& Instance()
    {
      static ThreadPool pool;
      return pool;
    }

    template <class F> void Run(size_t helpers, const F& work)
    {
      std::lock_guard<std::mutex> run_lock(run_mutex_);

      while (threads_.size() < helpers)
        threads_.emplace_back([this] { Worker(); });

      {
        std::lock_guard<std::mutex> lock(mutex_);
        fn_ = [](const void* ctx) { (*static_cast<const F*>(ctx))(); };
        ctx_ = &work;
        pending_ = helpers;
      }
      start_.notify_all();

      work();

      std::unique_lock<std::mutex> lock(mutex_);
      pending_ = 0;
      done_.wait(lock, [this] { return active_ == 0; });
    }

    ~ThreadPool()
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
      }
      start_.notify_all();
    }

  private:
    void Worker()
    {
      std::unique_lock<std::mutex> lock(mutex_);

      for (;;)
      {
        start_.wait(lock, [this] { return stop_ || pending_ > 0; });

        if (stop_)
          return;

        --pending_;
        ++active_;

        void (*const fn)(const void*) = fn_;
        const void* const ctx = ctx_;

        lock.unlock();
        fn(ctx);
        lock.lock();

        if (--active_ == 0)
          done_.notify_one();
      }
    }

    std::mutex run_mutex_;
    std::mutex mutex_;
    std::condition_variable start_;
    std::condition_variable done_;
    void (*fn_)(const void*) = nullptr;
    const void* ctx_ = nullptr;
    size_t pending_ = 0;
    size_t active_ = 0;
    bool stop_ = false;
    std::vector<std::jthread> threads_;
  };

  alignas(64) uint32_t cv_stack_[54 * 8];
  alignas(64) uint8_t chunk_buffer_[1024];

  uint64_t chunk_counter_ = 0;
  size_t buffer_len_ = 0;
  uint8_t cv_stack_len_ = 0;

  template <int N, class D> static HWY_INLINE hn::Vec<D> Rotr(D d, hn::Vec<D> x) noexcept
  {
#if HWY_TARGET <= HWY_AVX3
    (void)d;
    return hn::RotateRight<N>(x);
#else
    if constexpr (N == 16)
    {
      const hn::Repartition<uint8_t, D> d8;
      const auto idx =
          hn::Dup128VecFromValues(d8, 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
      return hn::BitCast(d, hn::TableLookupBytes(hn::BitCast(d8, x), idx));
    }
    else if constexpr (N == 8)
    {
      const hn::Repartition<uint8_t, D> d8;
      const auto idx =
          hn::Dup128VecFromValues(d8, 1, 2, 3, 0, 5, 6, 7, 4, 9, 10, 11, 8, 13, 14, 15, 12);
      return hn::BitCast(d, hn::TableLookupBytes(hn::BitCast(d8, x), idx));
    }
    else
    {
      return hn::RotateRight<N>(x);
    }
#endif
  }

  template <class D>
  static HWY_INLINE void QRound(D d, hn::Vec<D>* words, int a, int b, int c, int e, hn::Vec<D> m0,
                                hn::Vec<D> m1) noexcept
  {
    words[a] = hn::Add(hn::Add(words[a], words[b]), m0);
    words[e] = Rotr<16, D>(d, hn::Xor(words[e], words[a]));
    words[c] = hn::Add(words[c], words[e]);
    words[b] = Rotr<12, D>(d, hn::Xor(words[b], words[c]));

    words[a] = hn::Add(hn::Add(words[a], words[b]), m1);
    words[e] = Rotr<8, D>(d, hn::Xor(words[e], words[a]));
    words[c] = hn::Add(words[c], words[e]);
    words[b] = Rotr<7, D>(d, hn::Xor(words[b], words[c]));
  }

  template <class D>
  static HWY_INLINE void Half(D d, hn::Vec<D>* words, const uint8_t (&x)[4][4], const hn::Vec<D>* m,
                              const uint8_t* s) noexcept
  {
    int i = 0;
    for (; i < 4; ++i)
      words[x[i][0]] = hn::Add(hn::Add(words[x[i][0]], words[x[i][1]]), m[s[2 * i]]);
    for (i = 0; i < 4; ++i)
      words[x[i][3]] = Rotr<16, D>(d, hn::Xor(words[x[i][3]], words[x[i][0]]));
    for (i = 0; i < 4; ++i)
      words[x[i][2]] = hn::Add(words[x[i][2]], words[x[i][3]]);
    for (i = 0; i < 4; ++i)
      words[x[i][1]] = Rotr<12, D>(d, hn::Xor(words[x[i][1]], words[x[i][2]]));
    for (i = 0; i < 4; ++i)
      words[x[i][0]] = hn::Add(hn::Add(words[x[i][0]], words[x[i][1]]), m[s[2 * i + 1]]);
    for (i = 0; i < 4; ++i)
      words[x[i][3]] = Rotr<8, D>(d, hn::Xor(words[x[i][3]], words[x[i][0]]));
    for (i = 0; i < 4; ++i)
      words[x[i][2]] = hn::Add(words[x[i][2]], words[x[i][3]]);
    for (i = 0; i < 4; ++i)
      words[x[i][1]] = Rotr<7, D>(d, hn::Xor(words[x[i][1]], words[x[i][2]]));
  }

  template <bool IL, class D>
  static HWY_INLINE void Compress(D d, hn::Vec<D>* cv, const hn::Vec<D>* m, hn::Vec<D> counter_low,
                                  hn::Vec<D> counter_high, hn::Vec<D> block_len,
                                  hn::Vec<D> flags) noexcept
  {
    using V = hn::Vec<D>;

    V words[16];

    for (int i = 0; i < 8; ++i)
      words[i] = cv[i];
    for (int i = 0; i < 4; ++i)
      words[i + 8] = hn::Set(d, default_cv[i]);

    words[12] = counter_low;
    words[13] = counter_high;
    words[14] = block_len;
    words[15] = flags;

    for (int r = 0; r < 7; ++r)
    {
      const uint8_t* s = msg_schedule[r];

      if constexpr (IL)
      {
        Half(d, words, cols, m, s);
        Half(d, words, dia, m, s + 8);
      }
      else
      {
        QRound(d, words, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        QRound(d, words, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        QRound(d, words, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        QRound(d, words, 3, 7, 11, 15, m[s[6]], m[s[7]]);

        QRound(d, words, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        QRound(d, words, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        QRound(d, words, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        QRound(d, words, 3, 4, 9, 14, m[s[14]], m[s[15]]);
      }
    }

    for (int i = 0; i < 8; ++i)
      cv[i] = hn::Xor(words[i], words[i + 8]);
  }

  template <class D>
  static HWY_INLINE void LoadBlock(D d, const uint8_t* const* ptrs, size_t offset,
                                   hn::Vec<D>* m) noexcept
  {
    using V = hn::Vec<D>;
    const hn::Repartition<uint8_t, D> d8;
    const hn::Repartition<uint64_t, D> d64;
    const size_t lanes = hn::Lanes(d);

    if (lanes == 8)
    {
      for (size_t half = 0; half < 2; ++half)
      {
        const size_t o = offset + half * 32;

        const V r0 = hn::BitCast(d, hn::LoadU(d8, ptrs[0] + o));
        const V r1 = hn::BitCast(d, hn::LoadU(d8, ptrs[1] + o));
        const V r2 = hn::BitCast(d, hn::LoadU(d8, ptrs[2] + o));
        const V r3 = hn::BitCast(d, hn::LoadU(d8, ptrs[3] + o));
        const V r4 = hn::BitCast(d, hn::LoadU(d8, ptrs[4] + o));
        const V r5 = hn::BitCast(d, hn::LoadU(d8, ptrs[5] + o));
        const V r6 = hn::BitCast(d, hn::LoadU(d8, ptrs[6] + o));
        const V r7 = hn::BitCast(d, hn::LoadU(d8, ptrs[7] + o));

        const auto t0 = hn::BitCast(d64, hn::InterleaveLower(d, r0, r1));
        const auto t1 = hn::BitCast(d64, hn::InterleaveUpper(d, r0, r1));
        const auto t2 = hn::BitCast(d64, hn::InterleaveLower(d, r2, r3));
        const auto t3 = hn::BitCast(d64, hn::InterleaveUpper(d, r2, r3));
        const auto t4 = hn::BitCast(d64, hn::InterleaveLower(d, r4, r5));
        const auto t5 = hn::BitCast(d64, hn::InterleaveUpper(d, r4, r5));
        const auto t6 = hn::BitCast(d64, hn::InterleaveLower(d, r6, r7));
        const auto t7 = hn::BitCast(d64, hn::InterleaveUpper(d, r6, r7));

        const V u0 = hn::BitCast(d, hn::InterleaveLower(d64, t0, t2));
        const V u1 = hn::BitCast(d, hn::InterleaveUpper(d64, t0, t2));
        const V u2 = hn::BitCast(d, hn::InterleaveLower(d64, t1, t3));
        const V u3 = hn::BitCast(d, hn::InterleaveUpper(d64, t1, t3));
        const V u4 = hn::BitCast(d, hn::InterleaveLower(d64, t4, t6));
        const V u5 = hn::BitCast(d, hn::InterleaveUpper(d64, t4, t6));
        const V u6 = hn::BitCast(d, hn::InterleaveLower(d64, t5, t7));
        const V u7 = hn::BitCast(d, hn::InterleaveUpper(d64, t5, t7));

        V* out = m + half * 8;
        out[0] = hn::ConcatLowerLower(d, u4, u0);
        out[1] = hn::ConcatLowerLower(d, u5, u1);
        out[2] = hn::ConcatLowerLower(d, u6, u2);
        out[3] = hn::ConcatLowerLower(d, u7, u3);
        out[4] = hn::ConcatUpperUpper(d, u4, u0);
        out[5] = hn::ConcatUpperUpper(d, u5, u1);
        out[6] = hn::ConcatUpperUpper(d, u6, u2);
        out[7] = hn::ConcatUpperUpper(d, u7, u3);
      }
      return;
    }

    if (lanes == 4)
    {
      for (size_t quarter = 0; quarter < 4; ++quarter)
      {
        const size_t o = offset + quarter * 16;

        const V r0 = hn::BitCast(d, hn::LoadU(d8, ptrs[0] + o));
        const V r1 = hn::BitCast(d, hn::LoadU(d8, ptrs[1] + o));
        const V r2 = hn::BitCast(d, hn::LoadU(d8, ptrs[2] + o));
        const V r3 = hn::BitCast(d, hn::LoadU(d8, ptrs[3] + o));

        const auto t0 = hn::BitCast(d64, hn::InterleaveLower(d, r0, r1));
        const auto t1 = hn::BitCast(d64, hn::InterleaveUpper(d, r0, r1));
        const auto t2 = hn::BitCast(d64, hn::InterleaveLower(d, r2, r3));
        const auto t3 = hn::BitCast(d64, hn::InterleaveUpper(d, r2, r3));

        V* out = m + quarter * 4;
        out[0] = hn::BitCast(d, hn::InterleaveLower(d64, t0, t2));
        out[1] = hn::BitCast(d, hn::InterleaveUpper(d64, t0, t2));
        out[2] = hn::BitCast(d, hn::InterleaveLower(d64, t1, t3));
        out[3] = hn::BitCast(d, hn::InterleaveUpper(d64, t1, t3));
      }
      return;
    }

    HWY_ALIGN uint32_t lane_words[MAX_LANES];

    for (int j = 0; j < 16; ++j)
    {
      for (size_t lane = 0; lane < lanes; ++lane)
        hwy::CopyBytes(ptrs[lane] + offset + j * 4, &lane_words[lane], sizeof(uint32_t));

      m[j] = hn::Load(d, lane_words);
    }
  }

  template <bool IL, class D>
  static HWY_INLINE void HashChunks(D d, const uint8_t* data, size_t valid, uint64_t counter,
                                    hn::Vec<D>* cv) noexcept
  {
    using V = hn::Vec<D>;
    const size_t lanes = hn::Lanes(d);

    HWY_ALIGN uint32_t counter_lo[MAX_LANES];
    HWY_ALIGN uint32_t counter_hi[MAX_LANES];
    const uint8_t* ptrs[MAX_LANES];

    for (size_t lane = 0; lane < lanes; ++lane)
    {
      counter_lo[lane] = static_cast<uint32_t>(counter + lane);
      counter_hi[lane] = static_cast<uint32_t>((counter + lane) >> 32);
      ptrs[lane] = data + (lane < valid ? lane * 1024 : 0);
    }

    const V counter_low = hn::Load(d, counter_lo);
    const V counter_high = hn::Load(d, counter_hi);
    const V block_len = hn::Set(d, 64u);

    for (int j = 0; j < 8; ++j)
      cv[j] = hn::Set(d, default_cv[j]);

    V block[16];

    for (size_t offset = 0; offset < 1024; offset += 64)
    {
      LoadBlock(d, ptrs, offset, block);

      const uint32_t flags = (offset == 0 ? CHUNK_START : 0u) | (offset == 960 ? CHUNK_END : 0u);

      Compress<IL>(d, cv, block, counter_low, counter_high, block_len, hn::Set(d, flags));
    }
  }

  template <bool IL, class D>
  static HWY_INLINE void MergeParents(D d, const hn::Vec<D>* lo, const hn::Vec<D>* hi,
                                      hn::Vec<D>* out) noexcept
  {
    using V = hn::Vec<D>;

    V block[16];
    V cv[8];

    for (int j = 0; j < 8; ++j)
    {
      block[j] = hn::ConcatEven(d, hi[j], lo[j]);
      block[j + 8] = hn::ConcatOdd(d, hi[j], lo[j]);
      cv[j] = hn::Set(d, default_cv[j]);
    }

    Compress<IL>(d, cv, block, hn::Zero(d), hn::Zero(d), hn::Set(d, 64u), hn::Set(d, PARENT));

    for (int j = 0; j < 8; ++j)
      out[j] = cv[j];
  }

  template <bool IL, class D>
  static HWY_INLINE void HashGroup(D d, const uint8_t* data, size_t batches, uint64_t counter,
                                   uint32_t* out) noexcept
  {
    using V = hn::Vec<D>;
    const size_t lanes = hn::Lanes(d);

    V cvs[MAX_BATCHES][8];

    for (size_t b = 0; b < batches; ++b)
      HashChunks<IL>(d, data + b * lanes * 1024, lanes, counter + b * lanes, cvs[b]);

    for (size_t n = batches; n > 1; n /= 2)
      for (size_t i = 0; i < n / 2; ++i)
        MergeParents<IL>(d, cvs[2 * i], cvs[2 * i + 1], cvs[i]);

    for (size_t width = lanes; width > 1; width /= 2)
      MergeParents<IL>(d, cvs[0], cvs[0], cvs[0]);

    for (int j = 0; j < 8; ++j)
      out[j] = hn::GetLane(cvs[0][j]);
  }

  static HWY_INLINE void MergePair(const uint32_t* left, const uint32_t* right,
                                   uint32_t* out) noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;

    V cv[8];
    V block[16];

    for (int j = 0; j < 8; ++j)
    {
      cv[j] = hn::Set(d, default_cv[j]);
      block[j] = hn::Set(d, left[j]);
      block[j + 8] = hn::Set(d, right[j]);
    }

    Compress<false>(d, cv, block, hn::Zero(d), hn::Zero(d), hn::Set(d, 64u), hn::Set(d, PARENT));

    for (int j = 0; j < 8; ++j)
      out[j] = hn::GetLane(cv[j]);
  }

  static bool OnEfficiencyCore() noexcept
  {
#if defined(_M_X64) || defined(_M_IX86)
    int regs[4];

    __cpuid(regs, 0);
    if (static_cast<uint32_t>(regs[0]) < 0x1A)
      return false;

    __cpuidex(regs, 0x1A, 0);

    return (static_cast<uint32_t>(regs[0]) >> 24) == 0x20;

#elif defined(__i386__) || defined(__x86_64__)
    unsigned eax, ebx, ecx, edx;

    if (!__get_cpuid_count(0x1A, 0, &eax, &ebx, &ecx, &edx))
      return false;
    return (eax >> 24) == 0x20;

#else
    return false;
#endif
  }

  template <bool IL>
  static void HashSubtree(const uint8_t* data, size_t chunks, uint64_t counter,
                          uint32_t* out) noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    const size_t lanes = hn::Lanes(d);

    if (chunks <= MAX_BATCHES * lanes)
    {
      HashGroup<IL>(d, data, chunks / lanes, counter, out);
      return;
    }

    const size_t half = chunks / 2;

    uint32_t left[8];
    uint32_t right[8];

    HashSubtree<IL>(data, half, counter, left);
    HashSubtree<IL>(data + half * 1024, half, counter + half, right);

    MergePair(left, right, out);
  }

  void ProcessChunk() noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;

    V cv[8];
    V block[16];

    for (int j = 0; j < 8; ++j)
      cv[j] = hn::Set(d, default_cv[j]);

    const V counter_low = hn::Set(d, static_cast<uint32_t>(chunk_counter_));
    const V counter_high = hn::Set(d, static_cast<uint32_t>(chunk_counter_ >> 32));

    for (int i = 0; i < 1024; i += 64)
    {
      for (int j = 0; j < 16; ++j)
      {
        uint32_t val;
        hwy::CopyBytes(chunk_buffer_ + i + j * 4, &val, sizeof(val));

        block[j] = hn::Set(d, val);
      }

      const uint32_t flags = (i == 0 ? CHUNK_START : 0u) | (i == 960 ? CHUNK_END : 0u);

      Compress<false>(d, cv, block, counter_low, counter_high, hn::Set(d, 64u), hn::Set(d, flags));
    }

    uint32_t result[8];

    for (int j = 0; j < 8; ++j)
      result[j] = hn::GetLane(cv[j]);

    PushChainingValue(result, 0);
  }

  void ProcessChunksSIMD(const uint8_t* data, size_t num_chunks) noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;
    const size_t lanes = hn::Lanes(d);

    while (num_chunks > 0)
    {
      if (num_chunks >= lanes && (chunk_counter_ & (lanes - 1)) == 0)
      {
        size_t batches = 1;
        while (batches < MAX_BATCHES && num_chunks >= 2 * batches * lanes &&
               (chunk_counter_ & (2 * batches * lanes - 1)) == 0)
          batches *= 2;

        uint32_t root[8];
        HashGroup<false>(d, data, batches, chunk_counter_, root);

        const size_t total = batches * lanes;

        PushChainingValue(root, static_cast<unsigned>(std::countr_zero(total)));

        chunk_counter_ += total;
        data += total * 1024;
        num_chunks -= total;
      }
      else
      {
        const size_t to_boundary = lanes - (chunk_counter_ & (lanes - 1));
        const size_t count = std::min(num_chunks, to_boundary);

        V cv[8];
        HashChunks<false>(d, data, count, chunk_counter_, cv);

        HWY_ALIGN uint32_t lane_words[MAX_LANES];
        uint32_t lane_results[MAX_LANES][8];

        for (int j = 0; j < 8; ++j)
        {
          hn::Store(cv[j], d, lane_words);

          for (size_t lane = 0; lane < count; ++lane)
            lane_results[lane][j] = lane_words[lane];
        }

        for (size_t lane = 0; lane < count; ++lane)
        {
          PushChainingValue(lane_results[lane], 0);
          ++chunk_counter_;
        }

        data += count * 1024;
        num_chunks -= count;
      }
    }
  }

  void PushChainingValue(const uint32_t cv[8], unsigned log2_chunks) noexcept
  {
    uint32_t current[8];
    hwy::CopyBytes(cv, current, sizeof(current));

    uint64_t total = (chunk_counter_ >> log2_chunks) + 1;
    while ((total & 1u) == 0)
    {
      --cv_stack_len_;

      uint32_t merged[8];
      MergePair(&cv_stack_[cv_stack_len_ * 8], current, merged);
      hwy::CopyBytes(merged, current, sizeof(current));

      total >>= 1;
    }

    hwy::CopyBytes(current, &cv_stack_[cv_stack_len_ * 8], sizeof(current));

    ++cv_stack_len_;
  }
};
