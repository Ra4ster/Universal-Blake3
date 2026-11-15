#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include <hwy/highway.h>
namespace hn = hwy::HWY_NAMESPACE;

namespace Deep
{

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

inline constexpr uint32_t default_cv[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
constexpr uint32_t default_m[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};

class Blake3
{
public:
  Blake3() noexcept
      : chunk_counter_(0)
      , buffer_len_(0)
      , cv_stack_len_(0)
  {
  }

  void Update(const void* data, size_t len) noexcept
  {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    const hn::ScalableTag<uint32_t> d;
    const size_t lanes = hn::Lanes(d);

    while (len > 0)
    {
      if (buffer_len_ == 1024)
      {
        ProcessChunk();
        ++chunk_counter_;
        buffer_len_ = 0;
      }

      if (buffer_len_ == 0 && lanes > 1 && len > lanes * 1024)
      {
        const size_t complete_chunks = (len - 1) / 1024;
        const size_t simd_chunks = (complete_chunks / lanes) * lanes;

        if (simd_chunks > 0)
        {
          ProcessChunksSIMD(ptr, simd_chunks);

          const size_t consumed = simd_chunks * 1024;
          ptr += consumed;
          len -= consumed;
          continue;
        }
      }

      const size_t take = MIN(len, 1024 - buffer_len_);

      hwy::CopyBytes(ptr, chunk_buffer_ + buffer_len_, take);

      buffer_len_ += take;
      ptr += take;
      len -= take;
    }
  }

  std::string Hex() noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;

    constexpr uint32_t CHUNK_START = 1u;
    constexpr uint32_t CHUNK_END = 2u;
    constexpr uint32_t PARENT = 4u;
    constexpr uint32_t ROOT = 8u;

    V cv[8];
    V block[16];
    V output[16];

    for (int j = 0; j < 8; ++j)
      cv[j] = hn::Set(d, default_cv[j]);

    const V counter_low = hn::Set(d, static_cast<uint32_t>(chunk_counter_));
    const V counter_high = hn::Set(d, static_cast<uint32_t>(chunk_counter_ >> 32));
    const size_t num_blocks = buffer_len_ == 0 ? 1 : (buffer_len_ + 63) / 64;

    for (size_t i = 0; i < num_blocks; ++i)
    {
      const size_t offset = i * 64;
      const size_t remaining = buffer_len_ > offset ? buffer_len_ - offset : 0;
      const size_t block_len = MIN(remaining, 64);

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

      Compress(d,
               cv,
               block,
               counter_low,
               counter_high,
               hn::Set(d, static_cast<uint32_t>(block_len)),
               hn::Set(d, flags),
               output);

      for (int j = 0; j < 8; ++j)
        cv[j] = output[j];
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

      Compress(d, cv, block, hn::Zero(d), hn::Zero(d), hn::Set(d, 64u), hn::Set(d, flags), output);

      for (int j = 0; j < 8; ++j)
        cv[j] = output[j];
    }

    constexpr char hex_digits[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(64);

    for (int j = 0; j < 8; ++j)
    {
      const uint32_t word = hn::GetLane(output[j]);

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
  alignas(64) uint32_t cv_stack_[54 * 8];
  alignas(64) uint8_t chunk_buffer_[1024];

  uint64_t chunk_counter_ = 0;
  size_t buffer_len_ = 0;
  uint8_t cv_stack_len_ = 0;

  template <int N, class D> static HWY_INLINE hn::Vec<D> Rotr(hn::Vec<D> x) noexcept
  {
    return hn::Or(hn::ShiftRight<N>(x), hn::ShiftLeft<32 - N>(x));
  }

  template <class D>
  static HWY_INLINE void QRound(D, hn::Vec<D>* words, int a, int b, int c, int d, hn::Vec<D> m0,
                                hn::Vec<D> m1) noexcept
  {
    words[a] = hn::Add(hn::Add(words[a], words[b]), m0);
    words[d] = Rotr<16, D>(hn::Xor(words[d], words[a]));
    words[c] = hn::Add(words[c], words[d]);
    words[b] = Rotr<12, D>(hn::Xor(words[b], words[c]));

    words[a] = hn::Add(hn::Add(words[a], words[b]), m1);
    words[d] = Rotr<8, D>(hn::Xor(words[d], words[a]));
    words[c] = hn::Add(words[c], words[d]);
    words[b] = Rotr<7, D>(hn::Xor(words[b], words[c]));
  }

  template <class D>
  static HWY_INLINE void Compress(D d, const hn::Vec<D> (&cv)[8], const hn::Vec<D> (&block)[16],
                                  hn::Vec<D> counter_low, hn::Vec<D> counter_high,
                                  hn::Vec<D> block_len, hn::Vec<D> flags,
                                  hn::Vec<D> (&out)[16]) noexcept
  {
    using V = hn::Vec<D>;

    V words[16];
    V m[16];

    for (int i = 0; i < 8; ++i)
      words[i] = cv[i];

    for (int i = 0; i < 4; ++i)
      words[i + 8] = hn::Set(d, default_cv[i]);

    words[12] = counter_low;
    words[13] = counter_high;
    words[14] = block_len;
    words[15] = flags;

    for (int i = 0; i < 16; ++i)
      m[i] = block[i];

    for (int j = 0; j < 7; ++j)
    {
      QRound(d, words, 0, 4, 8, 12, m[0], m[1]);
      QRound(d, words, 1, 5, 9, 13, m[2], m[3]);
      QRound(d, words, 2, 6, 10, 14, m[4], m[5]);
      QRound(d, words, 3, 7, 11, 15, m[6], m[7]);

      QRound(d, words, 0, 5, 10, 15, m[8], m[9]);
      QRound(d, words, 1, 6, 11, 12, m[10], m[11]);
      QRound(d, words, 2, 7, 8, 13, m[12], m[13]);
      QRound(d, words, 3, 4, 9, 14, m[14], m[15]);

      if (j != 6)
      {
        const V tmp[16] = {m[2],
                           m[6],
                           m[3],
                           m[10],
                           m[7],
                           m[0],
                           m[4],
                           m[13],
                           m[1],
                           m[11],
                           m[12],
                           m[5],
                           m[9],
                           m[14],
                           m[15],
                           m[8]};

        for (int i = 0; i < 16; ++i)
          m[i] = tmp[i];
      }
    }

    for (int i = 0; i < 8; ++i)
    {
      out[i] = hn::Xor(words[i], words[i + 8]);
      out[i + 8] = hn::Xor(words[i + 8], cv[i]);
    }
  }

  void ProcessChunk() noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;

    V cv[8];
    V block[16];
    V output[16];

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

      const uint32_t flags = (i == 0 ? 1u : 0u) | (i == 960 ? 2u : 0u);

      Compress(d, cv, block, counter_low, counter_high, hn::Set(d, 64u), hn::Set(d, flags), output);

      for (int j = 0; j < 8; ++j)
        cv[j] = output[j];
    }

    uint32_t result[8];

    for (int j = 0; j < 8; ++j)
      result[j] = hn::GetLane(cv[j]);

    PushChainingValue(result);
  }

  void ProcessChunksSIMD(const uint8_t* data, size_t num_chunks) noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;
    const size_t lanes = hn::Lanes(d);

    HWY_ALIGN uint32_t lane_words[16];
    HWY_ALIGN uint32_t lane_results[16][8];

    for (size_t base = 0; base < num_chunks; base += lanes)
    {
      const uint64_t batch_counter = chunk_counter_;

      V cv[8];
      V block[16];
      V output[16];

      for (int j = 0; j < 8; ++j)
        cv[j] = hn::Set(d, default_cv[j]);

      for (size_t lane = 0; lane < lanes; ++lane)
        lane_words[lane] = static_cast<uint32_t>(batch_counter + lane);

      const V counter_low = hn::LoadU(d, lane_words);

      for (size_t lane = 0; lane < lanes; ++lane)
        lane_words[lane] = static_cast<uint32_t>((batch_counter + lane) >> 32);

      const V counter_high = hn::LoadU(d, lane_words);

      for (size_t offset = 0; offset < 1024; offset += 64)
      {
        for (int j = 0; j < 16; ++j)
        {
          for (size_t lane = 0; lane < lanes; ++lane)
          {
            const uint8_t* src = data + (base + lane) * 1024 + offset + j * 4;

            hwy::CopyBytes(src, &lane_words[lane], sizeof(uint32_t));
          }
          block[j] = hn::LoadU(d, lane_words);
        }

        const uint32_t flags = (offset == 0 ? 1u : 0u) | (offset == 960 ? 2u : 0u);

        Compress(
            d, cv, block, counter_low, counter_high, hn::Set(d, 64u), hn::Set(d, flags), output);

        for (int j = 0; j < 8; ++j)
          cv[j] = output[j];
      }

      for (int j = 0; j < 8; ++j)
      {
        hn::StoreU(cv[j], d, lane_words);

        for (size_t lane = 0; lane < lanes; ++lane)
          lane_results[lane][j] = lane_words[lane];
      }

      for (size_t lane = 0; lane < lanes; ++lane)
      {
        PushChainingValue(lane_results[lane]);
        ++chunk_counter_;
      }
    }
  }

  void PushChainingValue(const uint32_t cv[8]) noexcept
  {
    const hn::ScalableTag<uint32_t> d;
    using V = hn::Vec<decltype(d)>;

    uint32_t current[8];
    hwy::CopyBytes(cv, current, sizeof(current));

    uint64_t total_chunks = chunk_counter_ + 1;
    while ((total_chunks & 1u) == 0)
    {
      --cv_stack_len_;

      const uint32_t* left = &cv_stack_[cv_stack_len_ * 8];

      V cvHWY[8];
      V blockHWY[16];
      V outputHWY[16];

      for (int j = 0; j < 8; ++j)
      {
        cvHWY[j] = hn::Set(d, default_cv[j]);
        blockHWY[j] = hn::Set(d, left[j]);
        blockHWY[j + 8] = hn::Set(d, current[j]);
      }

      Compress(
          d, cvHWY, blockHWY, hn::Zero(d), hn::Zero(d), hn::Set(d, 64u), hn::Set(d, 4u), outputHWY);

      for (int j = 0; j < 8; ++j)
        current[j] = hn::GetLane(outputHWY[j]);

      total_chunks >>= 1;
    }

    hwy::CopyBytes(current, &cv_stack_[cv_stack_len_ * 8], sizeof(current));

    ++cv_stack_len_;
  }
};
} // namespace Deep
