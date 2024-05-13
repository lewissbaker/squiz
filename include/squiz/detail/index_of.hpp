///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

namespace squiz::detail {

/// Computes the index of the type \c T in the pack \c Ts...
///
/// \tparam T
/// The type to search for.
///
/// \tparam Ts...
/// The pack to search within.
template <typename T, typename... Ts>
requires one_of<T, Ts...>
inline constexpr std::size_t index_of_v =
    [](std::initializer_list<bool> matches) noexcept {
      std::size_t index = 0;
      for (bool match : matches) {
        if (match)
          break;
        ++index;
      }
      return index;
    }({std::same_as<T, Ts>...});

}  // namespace squiz::detail
