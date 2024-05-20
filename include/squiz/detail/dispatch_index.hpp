///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cassert>
#include <cstdlib>
#include <functional>
#include <utility>

namespace squiz::detail {

/// \fn dispatch_index(Func&& func, Index index, std::integer_sequence<Index,
/// Ids...>)
///
/// Invokes \c func with a \c std::integral_constant<Index,I> where \c I is a
/// constant that is equal to \c index. This allows converting a run-time index
/// back into a compile-time index by effectively switching on the run-time
/// value and dispatching to the corresponding overload of \c func.
///
/// This can be useful for implementing variant-like types.
///
/// \pre
/// The \c index must be equal to one of the specified \c Ids.
///
/// \return
/// The result of invoking \c func(std::integral_constant<Index,I>{}).
/// All overloads of \c func that might be invoked must return the same type.

#define SQUIZ_DISPATCH_INDEX_CASE(id) \
  case id:                            \
    return std::invoke(               \
        std::forward<Func>(func), std::integral_constant<decltype(id), id>{})

template <typename Func, typename Index>
[[noreturn]] constexpr void
_dispatch_index_impl(Func&&, Index, std::integer_sequence<Index>) noexcept {
  assert(false);
  std::unreachable();
}

template <typename Func, typename Index, Index Id0>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func, Index index, std::integer_sequence<Index, Id0>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <typename Func, typename Index, Index Id0, Index Id1>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func, Index index, std::integer_sequence<Index, Id0, Id1>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <typename Func, typename Index, Index Id0, Index Id1, Index Id2>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func, Index index, std::integer_sequence<Index, Id0, Id1, Id2>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <
    typename Func,
    typename Index,
    Index Id0,
    Index Id1,
    Index Id2,
    Index Id3>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func,
    Index index,
    std::integer_sequence<Index, Id0, Id1, Id2, Id3>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    SQUIZ_DISPATCH_INDEX_CASE(Id3);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <
    typename Func,
    typename Index,
    Index Id0,
    Index Id1,
    Index Id2,
    Index Id3,
    Index Id4>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func,
    Index index,
    std::integer_sequence<Index, Id0, Id1, Id2, Id3, Id4>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    SQUIZ_DISPATCH_INDEX_CASE(Id3);
    SQUIZ_DISPATCH_INDEX_CASE(Id4);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <
    typename Func,
    typename Index,
    Index Id0,
    Index Id1,
    Index Id2,
    Index Id3,
    Index Id4,
    Index Id5>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func,
    Index index,
    std::integer_sequence<Index, Id0, Id1, Id2, Id3, Id4, Id5>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    SQUIZ_DISPATCH_INDEX_CASE(Id3);
    SQUIZ_DISPATCH_INDEX_CASE(Id4);
    SQUIZ_DISPATCH_INDEX_CASE(Id5);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <
    typename Func,
    typename Index,
    Index Id0,
    Index Id1,
    Index Id2,
    Index Id3,
    Index Id4,
    Index Id5,
    Index Id6>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func,
    Index index,
    std::integer_sequence<Index, Id0, Id1, Id2, Id3, Id4, Id5, Id6>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    SQUIZ_DISPATCH_INDEX_CASE(Id3);
    SQUIZ_DISPATCH_INDEX_CASE(Id4);
    SQUIZ_DISPATCH_INDEX_CASE(Id5);
    SQUIZ_DISPATCH_INDEX_CASE(Id6);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <
    typename Func,
    typename Index,
    Index Id0,
    Index Id1,
    Index Id2,
    Index Id3,
    Index Id4,
    Index Id5,
    Index Id6,
    Index Id7>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func,
    Index index,
    std::integer_sequence<Index, Id0, Id1, Id2, Id3, Id4, Id5, Id6, Id7>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    SQUIZ_DISPATCH_INDEX_CASE(Id3);
    SQUIZ_DISPATCH_INDEX_CASE(Id4);
    SQUIZ_DISPATCH_INDEX_CASE(Id5);
    SQUIZ_DISPATCH_INDEX_CASE(Id6);
    SQUIZ_DISPATCH_INDEX_CASE(Id7);
    default: {
      assert(false);
      std::unreachable();
    }
  }
}

template <
    typename Func,
    typename Index,
    Index Id0,
    Index Id1,
    Index Id2,
    Index Id3,
    Index Id4,
    Index Id5,
    Index Id6,
    Index Id7,
    Index Id8,
    Index... Rest>
constexpr decltype(auto) _dispatch_index_impl(
    Func&& func,
    Index index,
    std::integer_sequence<
        Index,
        Id0,
        Id1,
        Id2,
        Id3,
        Id4,
        Id5,
        Id6,
        Id7,
        Id8,
        Rest...>) {
  switch (index) {
    SQUIZ_DISPATCH_INDEX_CASE(Id0);
    SQUIZ_DISPATCH_INDEX_CASE(Id1);
    SQUIZ_DISPATCH_INDEX_CASE(Id2);
    SQUIZ_DISPATCH_INDEX_CASE(Id3);
    SQUIZ_DISPATCH_INDEX_CASE(Id4);
    SQUIZ_DISPATCH_INDEX_CASE(Id5);
    SQUIZ_DISPATCH_INDEX_CASE(Id6);
    SQUIZ_DISPATCH_INDEX_CASE(Id7);
    SQUIZ_DISPATCH_INDEX_CASE(Id8);
    default: {
      if constexpr (sizeof...(Rest) != 0) {
        return detail::_dispatch_index_impl(
            std::forward<Func>(func),
            index,
            std::integer_sequence<Index, Rest...>{});
      } else {
        assert(false);
        std::unreachable();
      }
    }
  }
}

#undef SQUIZ_DISPATCH_INDEX_CASE

template <typename Func, typename Index, Index... Indices>
  requires(
      std::invocable<Func, std::integral_constant<Index, Indices>> && ...)  //
constexpr decltype(auto) dispatch_index(
    Func&& func, Index index, std::integer_sequence<Index, Indices...>)  //
    noexcept(
        (std::is_nothrow_invocable_v<
             Func,
             std::integral_constant<Index, Indices>> &&
         ...)) {
  return detail::_dispatch_index_impl(
      std::forward<Func>(func),
      index,
      std::integer_sequence<Index, Indices...>{});
}

template <auto N, typename Func>
  requires requires(Func&& func) {
    detail::dispatch_index(
        std::forward<Func>(func),
        N,
        std::make_integer_sequence<decltype(N), N>{});
  }
constexpr decltype(auto)
dispatch_index(Func&& func, decltype(N) index) noexcept(
    noexcept(detail::dispatch_index(
        std::forward<Func>(func),
        N,
        std::make_integer_sequence<decltype(N), N>{}))) {
  return detail::dispatch_index(
      std::forward<Func>(func),
      index,
      std::make_integer_sequence<decltype(N), N>{});
}

}  // namespace squiz::detail
