#pragma once

#include <type_traits>

#include <squiz/concepts.hpp>
#include <squiz/queryable.hpp>

namespace squiz {

struct stop_possible_t {
  template <queryable Env>
    requires(Env::property_keys::template contains<stop_possible_t>)  //
  constexpr static bool_constant auto operator()(const Env& env) noexcept {
    return decltype(env.query(stop_possible_t{})){};
  }

  constexpr static std::false_type operator()(const queryable auto&) noexcept {
    return {};
  }
};

inline constexpr stop_possible_t stop_possible{};

template <typename Env>
inline constexpr bool is_stop_possible_v =
    decltype(stop_possible(std::declval<const Env&>()))::value;

}  // namespace squiz
