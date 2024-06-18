///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstdint>

namespace squiz::detail {

  constexpr bool add_with_overflow(std::uint32_t a, std::uint32_t b, std::uint32_t* result) noexcept {
    return __builtin_add_overflow(a, b,  result);
  }

  constexpr bool add_with_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t* result) noexcept {
    return __builtin_add_overflow(a, b,  result);
  }

  constexpr bool add_with_overflow(std::int32_t a, std::int32_t b, std::int32_t* result) noexcept {
    return __builtin_sadd_overflow(a, b, result);
  }

  constexpr bool add_with_overflow(std::int64_t a, std::int64_t b, std::int64_t* result) noexcept {
    return __builtin_add_overflow(a, b, result);
  }

  constexpr bool sub_with_overflow(std::uint32_t a, std::uint32_t b, std::uint32_t* result) noexcept {
    return __builtin_sub_overflow(a, b,  result);
  }

  constexpr bool sub_with_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t* result) noexcept {
    return __builtin_sub_overflow(a, b,  result);
  }

  constexpr bool sub_with_overflow(std::int32_t a, std::int32_t b, std::int32_t* result) noexcept {
    return __builtin_ssub_overflow(a, b, result);
  }

  constexpr bool sub_with_overflow(std::int64_t a, std::int64_t b, std::int64_t* result) noexcept {
    return __builtin_sub_overflow(a, b, result);
  }

  constexpr std::uint32_t add_with_carry(std::uint32_t a,std::uint32_t b, std::uint32_t carry_in, std::uint32_t* carry_out) noexcept  {
    return __builtin_addc(a, b, carry_in, carry_out);
  }

  constexpr std::uint64_t add_with_carry(std::uint64_t a, std::uint64_t b, std::uint64_t carry_in, std::uint64_t* carry_out) noexcept {
    return __builtin_addcl(a, b, carry_in, carry_out);
  }

  constexpr bool mul_with_overflow(std::int64_t a, std::int64_t b, std::int64_t* result) noexcept{
    return __builtin_mul_overflow(a, b, result);
  }

  /// A value with 2 * sizeof(T) * CHAR_BIT bits.
  ///
  /// The high-order bits are stored in \c high and the low-order bits are stored in \c low.
  template<std::integral T>
  struct extended_result {
    T high; // Value of the high-order bits
    T low;  // Value of the low-order bits
  };

  constexpr extended_result<std::uint64_t> mul_64_to_128(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
    unsigned __int128 result = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
    return extended_result<std::uint64_t>{static_cast<std::uint64_t>(result >> 64), static_cast<std::uint64_t>(result)};
#else
    const std::uint64_t a_low = static_cast<std::uint32_t>(a);
    const std::uint64_t a_high = static_cast<std::uint32_t>(a >> 32);
    const std::uint64_t b_low = static_cast<std::uint32_t>(b);
    const std::uint64_t b_high = static_cast<std::uint32_t>(b >> 32);

    const std::uint64_t part_high = a_high * b_high;
    const std::uint64_t part_mid_1 = a_high * b_low;
    const std::uint64_t part_mid_2 = a_low * b_high;
    const std::uint64_t part_low = a_low * b_low;

    std::uint64_t part_mid_sum;
    bool mid_sum_overflow = add_with_overflow(part_mid_1, part_mid_2, &part_mid_sum);

    std::uint64_t result_low;
    bool low_overflow = add_with_overflow(part_low, part_mid_sum << 32, &result_low);

    // Now need to add for the high part
    // - 0x1'0000'0000 - if mid_sum_overflow is true
    // - 0x1           - if low_overflow is true
    // - part_mid_sum >> 32
    // - part_high
    // But we are guaranteed that the result is not going to overflow.

    std::uint64_t result_high = part_high;
    result_high += part_mid_sum >> 32;
    result_high += static_cast<std::uint64_t>(mid_sum_overflow) << 32;
    result_high += static_cast<std::uint64_t>(low_overflow);

    return extended_result<std::uint64_t>{result_high, result_low};
#endif
  }
}
