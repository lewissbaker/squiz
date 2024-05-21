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
#include <squiz/receiver.hpp>
#include <squiz/detail/member_type.hpp>

namespace squiz {

template <typename Receiver, typename Signal, typename... Datums>
class just_op final
  : public inlinable_operation_state<
        just_op<Receiver, Signal, Datums...>,
        Receiver> {
  using inlinable_base = inlinable_operation_state<just_op, Receiver>;

public:
  template <typename... Datums2>
  explicit just_op(Receiver r, std::tuple<Datums2...> datums) noexcept(
      (std::is_nothrow_constructible_v<Datums, Datums2> && ...))
    : inlinable_base(std::move(r))
    , datums_(std::move(datums)) {}

  void start() noexcept {
    std::apply(
        [&](Datums&... datums) noexcept {
          squiz::set_result(
              this->get_receiver(),
              squiz::result<Signal, Datums...>,
              squiz::forward_parameter<Datums>(datums)...);
        },
        datums_);
  }

private:
  [[no_unique_address]] std::tuple<Datums...> datums_;
};

//
// just_value_sender
//

template <typename... Vs>
struct just_value_sender {
  template <typename Self>
    requires(std::constructible_from<Vs, detail::member_type_t<Self, Vs>> &&
             ...)  //
  auto get_completion_signatures(this Self&&)
      -> completion_signatures<value_t<Vs...>>;

  template <typename Self>
  auto is_always_nothrow_connectable(this Self&&)
      -> std::bool_constant<(
          std::
              is_nothrow_constructible_v<Vs, detail::member_type_t<Self, Vs>> &&
          ...)>;

  template <typename Self, typename Receiver>
  just_op<Receiver, value_tag, Vs...>
  connect(this Self&& self, Receiver r) noexcept(
      (std::is_nothrow_constructible_v<Vs, detail::member_type_t<Self, Vs>> &&
       ...)) {
    return std::apply(
        [&](detail::member_type_t<Self, Vs>&&... vs) {
          return just_op<Receiver, value_tag, Vs...>(
              std::move(r),
              std::forward_as_tuple(static_cast<decltype(vs)>(vs)...));
        },
        std::forward<Self>(self).values_);
  }

  template <typename... V2s>
    requires(std::constructible_from<Vs, V2s> && ...)  //
  explicit just_value_sender(V2s&&... vs)              //
      noexcept((std::is_nothrow_constructible_v<Vs, V2s> && ...))
    : values_{std::forward<V2s>(vs)...} {}

private:
  [[no_unique_address]] std::tuple<Vs...> values_;
};

template <typename... Vs>
just_value_sender(Vs...) -> just_value_sender<Vs...>;

//
// just_error_sender
//

template <typename E>
struct just_error_sender {
  template <typename Self>
    requires std::constructible_from<E, detail::member_type_t<Self, E>>
  auto
  get_completion_signatures(this Self&&) -> completion_signatures<error_t<E>>;

  template <typename Self>
  auto is_always_nothrow_connectable(this Self&&)
      -> std::bool_constant<
          std::is_nothrow_constructible_v<E, detail::member_type_t<Self, E>>>;

  template <typename Self, typename Receiver>
  just_op<Receiver, error_tag, E>
  connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<E, detail::member_type_t<Self, E>>) {
    return just_op<Receiver, error_tag, E>{
        std::move(r), std::forward_as_tuple(std::forward<Self>(self).error)};
  }

  [[no_unique_address]] E error;
};

template <typename E>
just_error_sender(E) -> just_error_sender<E>;

//
// just_stopped_sender
//

template <typename Receiver>
class just_stopped_op final
  : public inlinable_operation_state<just_stopped_op<Receiver>, Receiver> {
  using inlinable_base = inlinable_operation_state<just_stopped_op, Receiver>;

public:
  explicit just_stopped_op(Receiver r) noexcept
    : inlinable_base(std::move(r)) {}

  void start() noexcept { squiz::set_stopped(this->get_receiver()); }
};

struct just_stopped_sender {
  static auto get_completion_signatures() -> completion_signatures_t<stopped>;
  static auto is_always_nothrow_connectable() -> std::true_type;

  template <typename Receiver>
  just_stopped_op<Receiver> connect(Receiver r) noexcept {
    return just_stopped_op<Receiver>(std::move(r));
  }
};

}  // namespace squiz
