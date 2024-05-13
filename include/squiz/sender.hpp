///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include <squiz/completion_signatures.hpp>
#include <squiz/concepts.hpp>
#include <squiz/operation_state.hpp>
#include <squiz/receiver.hpp>

namespace squiz {

/// Concept for types that are senders.
///
/// \note
/// Nothing syntactic that distinguishes senders from other move-constructible
/// types. Unless you have an environment, in which case you can use sender_in
/// instead.
template <typename T>
concept sender = std::is_class_v<T> && std::move_constructible<T>;

template <typename T>
concept copyable_sender = sender<T> && std::copy_constructible<T>;

template <typename Sender, typename... Env>
struct completion_signatures_for {};

template <typename Sender, typename Env>
requires requires(Sender&& sender, Env env) {
  std::forward<Sender>(sender).get_completion_signatures(std::move(env));
}
struct completion_signatures_for<Sender, Env> {
  using type = decltype(std::declval<Sender>().get_completion_signatures(
      std::declval<Env>()));
};

template <typename Sender, typename Env>
struct completion_signatures_for<Sender, Env>
  : completion_signatures_for<Sender> {};

template <typename Sender>
requires requires(Sender&& sender) {
  std::forward<Sender>(sender).get_completion_signatures();
}
struct completion_signatures_for<Sender> {
  using type = decltype(std::declval<Sender>().get_completion_signatures());
};

template <typename Sender, typename... Envs>
using completion_signatures_for_t =
    typename completion_signatures_for<Sender, Envs...>::type;

namespace detail {

template <typename T, typename... Env>
concept valid_completion_signatures_for = requires {
  typename completion_signatures_for_t<T, Env...>;
  requires instance_of<
      completion_signatures_for_t<T, Env...>,
      completion_signatures>;
};

}  // namespace detail

template <typename T, typename... Env>
concept sender_in = sender<std::remove_cvref_t<T>> &&
    detail::valid_completion_signatures_for<T, Env...>;

template <typename T, typename Receiver>
concept connectable_to = sender_in<T, receiver_env_t<Receiver>> &&
    requires(T&& sender, Receiver r) {
  { std::forward<T>(sender).connect(std::move(r)) } -> operation_state;
};

template <typename T, typename Receiver>
concept nothrow_connectable_to = connectable_to<T, Receiver> &&
    requires(T&& sender, Receiver r) {
  { std::forward<T>(sender).connect(std::move(r)) }
  noexcept;
};

template <typename T, typename Receiver>
requires connectable_to<T, Receiver>
using connect_result_t =
    decltype(std::declval<T>().connect(std::declval<Receiver>()));

}  // namespace squiz
