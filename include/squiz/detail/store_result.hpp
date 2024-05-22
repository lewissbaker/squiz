///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

namespace squiz::detail {

  /// Helper function for storing a result in a variant of results.
  ///
  /// If storing the result in the tuple throws an exception then stores
  /// an error_t<std::exception_ptr> result instead.
  ///
  /// Use \c detail::deliver_result() to take a result stored in such a variant
  /// and invoke \c squiz::set_result() with that result.
template <typename Variant, typename Tag, typename... Datums>
void store_result(
    Variant& result, result_t<Tag, Datums...>, Datums&... datums) noexcept {
  try {
    result.template emplace<std::tuple<Tag, Datums...>>(
        Tag{}, std::forward<Datums>(datums)...);
  } catch (...) {
    constexpr bool is_nothrow =
        (std::is_nothrow_move_constructible_v<Datums> && ...);
    if constexpr (!is_nothrow) {
      result.template emplace<std::tuple<error_tag, std::exception_ptr>>(
          error_tag{}, std::current_exception());
    } else {
      assert(false);
      std::unreachable();
    }
  }
}

}  // namespace squiz::detail
