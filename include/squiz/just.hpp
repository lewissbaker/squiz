///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/detail/member_type.hpp>

namespace squiz {

template <typename Receiver, typename... Vs>
class just_op final
  : public inlinable_operation_state<just_op<Receiver, Vs...>, Receiver> {
  using inlinable_base = inlinable_operation_state<just_op, Receiver>;

public:
  template <typename... V2s>
  explicit just_op(Receiver r, std::tuple<V2s...> vs) noexcept(
      (std::is_nothrow_constructible_v<Vs, V2s> && ...))
    : inlinable_base(std::move(r))
    , values_(std::move(vs)) {}

  void start() noexcept {
    std::apply(
        [&](Vs&&... vs) noexcept {
          this->get_receiver().set_value(std::forward<Vs>(vs)...);
        },
        std::move(values_));
  }

private:
  [[no_unique_address]] std::tuple<Vs...> values_;
};

template <typename... Vs>
struct just_sender {
  template <typename Self>
    requires(std::constructible_from<Vs, detail::member_type_t<Self, Vs>> &&
             ...)  //
  auto get_completion_signatures(this Self&&)
      -> completion_signatures<set_value_t(Vs...)>;

  template <typename Self>
  auto is_always_nothrow_connectable(this Self&&)
      -> std::bool_constant<(
          std::
              is_nothrow_constructible_v<Vs, detail::member_type_t<Self, Vs>> &&
          ...)>;

  template <typename Self, typename Receiver>
  just_op<Receiver, Vs...> connect(this Self&& self, Receiver r) noexcept(
      (std::is_nothrow_constructible_v<Vs, detail::member_type_t<Self, Vs>> &&
       ...)) {
    return std::apply(
        [&](detail::member_type_t<Self, Vs>&&... vs) {
          return just_op<Receiver, Vs...>(
              std::move(r), std::forward_as_tuple(std::forward<Vs>(vs)...));
        },
        std::forward<Self>(self).values_);
  }

  template <typename... V2s>
    requires(std::constructible_from<Vs, V2s> && ...)  //
  explicit just_sender(V2s&&... vs)                    //
      noexcept((std::is_nothrow_constructible_v<Vs, V2s> && ...))
    : values_{std::forward<V2s>(vs)...} {}

private:
  std::tuple<Vs...> values_;
};

template <typename... Vs>
just_sender(Vs...) -> just_sender<Vs...>;

}  // namespace squiz
