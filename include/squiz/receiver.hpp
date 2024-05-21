///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/completion_signatures.hpp>
#include <squiz/concepts.hpp>

namespace squiz {

template <typename T>
concept receiver = nothrow_move_constructible<T> && requires(const T& t) {
  { t.get_env() } noexcept;
};

template <typename R>
using receiver_env_t = decltype(std::declval<const R&>().get_env());

struct set_result_t {
  template <typename Receiver, typename Tag, typename... Datums>
  static void operator()(
      Receiver&& r,
      result_t<Tag, Datums...>,
      parameter_type<Datums>... datums) noexcept {
    using sig_t = result_t<Tag, Datums...>;
    static_assert(
        noexcept(
            r.set_result(sig_t{}, squiz::forward_parameter<Datums>(datums)...)),
        "set_result() member-function invocation must be noexcept");
    r.set_result(sig_t{}, squiz::forward_parameter<Datums>(datums)...);
  }
};

inline constexpr set_result_t set_result;

template <typename... Vs>
struct set_value_t {
  template <typename Receiver>
  static void operator()(Receiver&& r, parameter_type<Vs>... vs) noexcept {
    using sig_t = value_t<Vs...>;
    static_assert(
        noexcept(r.set_result(sig_t{}, squiz::forward_parameter<Vs>(vs)...)),
        "set_result() member-function invocation must be noexcept");
    r.set_result(sig_t{}, squiz::forward_parameter<Vs>(vs)...);
  }
};

template <typename... Vs>
inline constexpr set_value_t<Vs...> set_value{};

template <typename E>
struct set_error_t {
  template <typename Receiver>
  static void operator()(Receiver&& r, parameter_type<E> e) noexcept {
    using sig_t = error_t<E>;
    static_assert(
        noexcept(r.set_result(sig_t{}, squiz::forward_parameter<E>(e))),
        "set_result() member-function invocation must be noexcept");
    r.set_result(sig_t{}, squiz::forward_parameter<E>(e));
  }
};

template <typename E>
inline constexpr set_error_t<E> set_error{};

struct set_stopped_t {
  template <typename Receiver>
  static void operator()(Receiver&& r) noexcept {
    static_assert(
        noexcept(r.set_result(stopped_t{})),
        "set_result() member-function invocation must be noexcept");
    r.set_result(stopped_t{});
  }
};

inline constexpr set_stopped_t set_stopped{};

}  // namespace squiz
