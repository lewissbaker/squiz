///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <numeric>

#include <squiz/detail/integer_ops.hpp>

namespace squiz::linuxos {
/// A std::chrono-like clock type that provides access to the CLOCK_BOOTTIME
/// clock.
///
/// This is a monotonic clock, like CLOCK_MONOTONIC, but also takes into account
/// time when the system is suspended. This allows timers to continue to elapse
/// when the system goes to sleep.
class monotonic_clock {
public:
  // A duration of signed 64-bit nanoseconds can represent spans of approx +/-
  // 292 years. This should be sufficient for representing any reasonable
  // difference in times used for scheduling purposes over the lifetime of an
  // application.
  using rep = std::int64_t;
  using ratio = std::nano;
  using duration = std::chrono::duration<rep, ratio>;  // nanoseconds

  class time_point {
  public:
    constexpr time_point() noexcept : seconds_(0), nanoseconds_(0) {}

    constexpr time_point(const time_point& a) noexcept = default;
    constexpr time_point& operator=(const time_point& a) noexcept = default;

    static constexpr time_point max() noexcept {
      time_point tp;
      tp.seconds_ = std::numeric_limits<std::int64_t>::max();
      tp.nanoseconds_ = 999'999'999;
      return tp;
    }

    static constexpr time_point min() noexcept {
      time_point tp;
      tp.seconds_ = std::numeric_limits<std::int64_t>::min();
      tp.nanoseconds_ = 0;
      return tp;
    }

    constexpr std::int64_t seconds_part() const noexcept { return seconds_; }

    // nanoseconds_part() is always in the range [0, 999'999'999]
    constexpr std::int32_t nanoseconds_part() const noexcept {
      return nanoseconds_;
    }

    static constexpr time_point from_seconds_and_nanoseconds(
        std::int64_t seconds, std::int32_t nanoseconds) noexcept {
      std::int32_t extra_seconds = nanoseconds / std::nano::den;
      nanoseconds -= static_cast<std::int32_t>(extra_seconds * std::nano::den);
      if (nanoseconds < 0) {
        extra_seconds -= 1;
        nanoseconds += std::nano::den;
      }
      assert(nanoseconds >= 0 && nanoseconds < 1'000'000'000);

      std::int64_t result_seconds;
      bool overflow = squiz::detail::add_with_overflow(
          seconds, extra_seconds, &result_seconds);
      if (overflow) {
        return (seconds < 0) ? min() : max();
      }

      time_point result;
      result.seconds_ = result_seconds;
      result.nanoseconds_ = nanoseconds;
      return result;
    }

    // Add a number that is a whole number of seconds.
    template <typename Rep, typename Ratio>
      requires(std::integral<Rep> && Ratio::den == 1)
    constexpr time_point&
    operator+=(const std::chrono::duration<Rep, Ratio>& d) noexcept {
      std::int64_t delta_seconds;
      [[maybe_unused]] bool overflow =
          squiz::detail::mul_with_overflow(d.count(), Ratio::num, &delta_seconds);
      assert(!overflow);
      std::int64_t result_seconds;
      overflow = squiz::detail::add_with_overflow(
          seconds_, delta_seconds, &result_seconds);
      assert(!overflow);
      seconds_ = result_seconds;
      return *this;
    }

    // Add a number that is not a whole number of seconds
    template <typename Rep, typename Ratio>
      requires(std::integral<Rep> && Ratio::den != 1)
    constexpr time_point&
    operator+=(const std::chrono::duration<Rep, Ratio>& d) noexcept {
      const auto seconds_to_add = std::chrono::floor<std::chrono::seconds>(d);
      const auto nanoseconds_to_add =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              d - seconds_to_add);

      assert(nanoseconds_to_add.count() >= 0);
      assert(nanoseconds_to_add.count() < 1'000'000'000);

      // TODO: There is potential for overflow in some of the computations here.
      std::int32_t extra_seconds = 0;
      nanoseconds_ += nanoseconds_to_add.count();
      if (nanoseconds_ >= 1'000'000'000) {
        nanoseconds_ -= 1'000'000'000;
        extra_seconds += 1;
      }

      if (seconds_to_add.count() > 0 &&
          seconds_ > (std::numeric_limits<std::int64_t>::max() -
                      seconds_to_add.count() - extra_seconds)) {
        *this = max();
      } else if (
          seconds_to_add.count() < 0 &&
          seconds_ < (std::numeric_limits<std::int64_t>::min() -
                      seconds_to_add.count() - extra_seconds)) {
        *this = min();
      } else {
        seconds_ += seconds_to_add.count();
        seconds_ += extra_seconds;
      }
      return *this;
    }

    template <typename Rep, typename Ratio>
      requires(std::integral<Rep> && Ratio::den == 1)
    constexpr time_point&
    operator-=(const std::chrono::duration<Rep, Ratio>& d) noexcept {
      std::int64_t delta_seconds;
      [[maybe_unused]] bool overflow =
          squiz::detail::mul_with_overflow(d.count(), Ratio::num, &delta_seconds);
      assert(!overflow);
      std::int64_t result_seconds;
      overflow = squiz::detail::sub_with_overflow(
          seconds_, delta_seconds, &result_seconds);
      assert(!overflow);
      seconds_ = result_seconds;
      return *this;
    }

    template <typename Rep, typename Ratio>
      requires(std::integral<Rep> && Ratio::den != 1)
    constexpr time_point&
    operator-=(const std::chrono::duration<Rep, Ratio>& d) noexcept {
      const auto seconds_to_subtract =
          std::chrono::floor<std::chrono::seconds>(d);
      const auto nanoseconds_to_add =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              d - seconds_to_subtract);

      assert(nanoseconds_to_add.count() >= 0);
      assert(nanoseconds_to_add.count() < 1'000'000'000);

      // TODO: There is potential for overflow in some of the computations here.
      std::int32_t extra_seconds = 0;
      nanoseconds_ += nanoseconds_to_add.count();
      if (nanoseconds_ >= 1'000'000'000) {
        nanoseconds_ -= 1'000'000'000;
        extra_seconds += 1;
      }

      if (seconds_to_subtract.count() > 0 &&
          seconds_ < (std::numeric_limits<std::int64_t>::min() +
                      seconds_to_subtract.count() - extra_seconds)) {
        *this = min();
      } else if (
          (seconds_to_subtract.count() < 0 || extra_seconds > 0) &&
          seconds_ > (std::numeric_limits<std::int64_t>::max() +
                      seconds_to_subtract.count() - extra_seconds)) {
        *this = max();
      } else {
        seconds_ -= seconds_to_subtract.count();
        seconds_ += extra_seconds;
      }
      return *this;
    }

    friend constexpr duration
    operator-(const time_point& a, const time_point& b) noexcept {
      std::int64_t seconds_diff = a.seconds_ - b.seconds_;
      std::int32_t nanoseconds_diff = a.nanoseconds_ - b.nanoseconds_;
      return duration{seconds_diff * 1'000'000'000 + nanoseconds_diff};
    }

    template <typename Rep, typename Ratio>
    friend constexpr time_point operator+(
        const time_point& tp,
        const std::chrono::duration<Rep, Ratio>& d) noexcept {
      time_point tp_copy = tp;
      tp_copy += d;
      return tp_copy;
    }

    template <typename Rep, typename Ratio>
    friend constexpr time_point operator-(
        const time_point& tp,
        const std::chrono::duration<Rep, Ratio>& d) noexcept {
      time_point tp_copy = tp;
      tp_copy -= d;
      return tp_copy;
    }

    friend bool
    operator==(const time_point& a, const time_point& b) noexcept = default;

    friend std::strong_ordering
    operator<=>(const time_point& a, const time_point& b) noexcept {
      return (a.seconds_ == b.seconds_) ? a.nanoseconds_ <=> b.nanoseconds_
                                        : a.seconds_ <=> b.seconds_;
    }

  private:
    std::int64_t seconds_;
    std::int32_t nanoseconds_;
  };

  static time_point now();
};

}  // namespace squiz::linuxos
