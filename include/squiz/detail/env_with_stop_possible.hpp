///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/queryable.hpp>
#include <squiz/stop_possible.hpp>

namespace squiz::detail {

template <queryable Env, bool StopPossible = true>
struct env_with_stop_possible {
  using property_keys =
      key_list_add_t<typename Env::property_keys, stop_possible_t>;

  env_with_stop_possible() = default;

  explicit env_with_stop_possible(Env env) noexcept(
      std::is_nothrow_move_constructible_v<Env>)
    : env(std::move(env)) {}

  env_with_stop_possible(const env_with_stop_possible& other) = default;
  env_with_stop_possible(env_with_stop_possible&& other) = default;

  explicit env_with_stop_possible(
      const env_with_stop_possible<Env, !StopPossible>&
          other) noexcept(std::is_nothrow_copy_constructible_v<Env>)
    requires std::copy_constructible<Env>
    : env(other.env) {}

  explicit env_with_stop_possible(
      env_with_stop_possible<Env, !StopPossible>&&
          other) noexcept(std::is_nothrow_move_constructible_v<Env>)
    requires std::move_constructible<Env>
    : env(std::move(other.env)) {}

  constexpr std::bool_constant<StopPossible>
  query(stop_possible_t) const noexcept {
    return {};
  }

  template <typename Key>
    requires queryable_for<Env, Key>
  decltype(auto) query(Key key) const noexcept(noexcept(env.query(key))) {
    return env.query(key);
  }

  [[no_unique_address]] Env env;
};

template <typename Env>
env_with_stop_possible(Env) -> env_with_stop_possible<Env>;

template <typename Env, bool StopPossible = true>
struct make_env_with_stop_possible {
  using type = env_with_stop_possible<Env, StopPossible>;
};

template <typename Env, bool StopPossible>
  requires(is_stop_possible_v<Env> == StopPossible)
struct make_env_with_stop_possible<Env, StopPossible> {
  using type = Env;
};

template <typename Env, bool StopPossible, bool OtherStopPossible>
  requires(is_stop_possible_v<Env> != StopPossible)
struct make_env_with_stop_possible<
    env_with_stop_possible<Env, OtherStopPossible>,
    StopPossible> {
  using type = env_with_stop_possible<Env, StopPossible>;
};

template <typename Env, bool StopPossible = true>
using make_env_with_stop_possible_t =
    typename make_env_with_stop_possible<Env, StopPossible>::type;

}  // namespace squiz::detail
