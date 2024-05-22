///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <type_traits>

namespace squiz {

namespace stoppable_token_detail {
template <template <typename> class>
class check_callback_type;
}

template <typename T>
concept stoppable_token =
    std::regular<T> && std::is_nothrow_copy_constructible_v<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_copy_assignable_v<T> &&
    std::is_nothrow_move_assignable_v<T> && requires(const T& x) {
      { x.stop_requested() } noexcept -> std::same_as<bool>;
      { x.stop_possible() } noexcept -> std::same_as<bool>;
      typename stoppable_token_detail::check_callback_type<
          T::template callback_type>;
    };

template <typename T>
concept unstoppable_token = stoppable_token<T> && requires {
  typename std::bool_constant<T::stop_possible()>;
  requires(!T::stop_possible());
};

}  // namespace squiz
