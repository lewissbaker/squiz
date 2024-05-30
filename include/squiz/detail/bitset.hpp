///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <bit>

namespace squiz::detail {

/// Equivalent to std::bitset but with the countl_one/zero and countr_one/zero
/// methods.
template <std::size_t Count>
struct bitset {
private:
  using chunk_type = std::conditional_t<
      (Count <= 8),
      std::uint8_t,
      std::conditional_t<
          (Count <= 16),
          std::uint16_t,
          std::conditional_t<(Count <= 32), std::uint32_t, std::uint64_t>>>;
  static constexpr std::size_t chunk_bits =
      std::numeric_limits<chunk_type>::digits;
  static constexpr std::size_t chunk_count =
      (Count + (chunk_bits - 1)) / chunk_bits;
  static constexpr std::size_t whole_chunk_count = Count / chunk_bits;
  static constexpr std::size_t partial_chunk_bits = Count % chunk_bits;
  static constexpr chunk_type last_chunk_mask =
      (chunk_type(1) << partial_chunk_bits) - 1U;
  static constexpr chunk_type all_ones_chunk = ~chunk_type(0);

public:
  static constexpr std::size_t size() noexcept { return Count; }

  void reset() noexcept {
    for (chunk_type& chunk : chunks_) {
      chunk = 0;
    }
  }

  void reset(std::size_t index) noexcept {
    std::size_t chunk_idx = index / chunk_bits;
    std::size_t bit_idx = index % chunk_bits;
    chunk_type bit_mask = chunk_type(1) << bit_idx;
    chunks_[chunk_idx] &= ~bit_mask;
  }

  void set_all(bool value = true) noexcept {
    if (value) {
      for (std::size_t i = 0; i < whole_chunk_count; ++i) {
        chunks_[i] = all_ones_chunk;
      }
      if constexpr (whole_chunk_count != chunk_count) {
        chunks_.back() = last_chunk_mask;
      }
    } else {
      reset();
    }
  }

  // Prevent accidentally calling set(true) instead of set_all(true).
  void set(std::same_as<bool> auto) = delete;

  void set(std::size_t index, bool value = true) noexcept {
    const std::size_t chunk_idx = index / chunk_bits;
    const std::size_t bit_idx = index % chunk_bits;
    const chunk_type bit_mask = chunk_type(1) << bit_idx;
    const chunk_type bit_value = value ? bit_mask : 0;
    chunks_[chunk_idx] = (chunks_[chunk_idx] & ~bit_mask) | bit_value;
  }

  bool operator[](std::size_t index) const noexcept {
    const std::size_t chunk_idx = index / chunk_bits;
    const std::size_t bit_idx = index % chunk_bits;
    return static_cast<bool>((chunks_[chunk_idx] >> bit_idx) & 0b1);
  }

  bool all() const noexcept {
    for (std::size_t i = 0; i < whole_chunk_count; ++i) {
      if (chunks_[i] != all_ones_chunk) {
        return false;
      }
    }
    if constexpr (whole_chunk_count != chunk_count) {
      if (chunks_.back() != last_chunk_mask) {
        return false;
      }
    }
    return true;
  }

  bool none() const noexcept {
    for (chunk_type chunk : chunks_) {
      if (chunk != 0) {
        return false;
      }
    }
    return true;
  }

  bool any() const noexcept { return !none(); }

  std::size_t count() const noexcept {
    std::size_t count = 0;
    for (chunk_type chunk : chunks_) {
      count += std::popcount(chunk);
    }
    return count;
  }

  std::size_t countr_zero() const noexcept {
    if constexpr (Count == 0) {
      return 0;
    } else {
      for (std::size_t i = 0; (i + 1) < chunk_count; ++i) {
        chunk_type chunk = chunks_[i];
        if (chunk != 0) {
          return (i * chunk_bits) + std::countr_zero(chunk);
        }
      }

      if constexpr (whole_chunk_count != chunk_count) {
        const chunk_type last_chunk_with_unused_bits_set_to_one =
            chunks_.back() | ~last_chunk_mask;
        return (chunk_count - 1) * chunk_bits +
            std::countr_zero(last_chunk_with_unused_bits_set_to_one);
      } else {
        return (chunk_count - 1) * chunk_bits +
            std::countr_zero(chunks_.back());
      }
    }
  }

public:
  [[no_unique_address]] std::array<chunk_type, chunk_count> chunks_{};
};

}  // namespace squiz::detail
