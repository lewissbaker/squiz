///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/queryable.hpp>
#include <squiz/stop_possible.hpp>

namespace squiz::detail {

template <queryable Env>
struct env_with_stop_possible {
  using property_keys =
      key_list_add_t<typename Env::property_keys, stop_possible_t>;

  constexpr std::true_type query(stop_possible_t) const noexcept { return {}; }

  template <typename Key>
    requires queryable_for<Env, Key>
  decltype(auto) query(Key key) const noexcept(noexcept(env.query(key))) {
    return env.query(key);
  }

  [[no_unique_address]] Env env;
};

template <typename Env>
env_with_stop_possible(Env) -> env_with_stop_possible<Env>;

}  // namespace squiz::detail
