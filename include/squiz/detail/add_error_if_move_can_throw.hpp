///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <exception>
#include <type_traits>

#include <squiz/completion_signatures.hpp>

namespace squiz::detail {

struct add_error_if_move_can_throw {
  template <typename Tag, typename... Datums>
    requires(std::is_nothrow_move_constructible_v<Datums> && ...)
  static auto apply(result_t<Tag, Datums...>)
      -> completion_signatures<result_t<Tag, Datums...>>;

  template <typename Tag, typename... Datums>
  static auto apply(result_t<Tag, Datums...>) -> completion_signatures<
                                                  result_t<Tag, Datums...>,
                                                  error_t<std::exception_ptr>>;
};

/// Type trait that modifies a \c completion_signatures instantiation to add
/// \c error_t<std::exception_ptr> if any of the completions have
/// move-constructors that are potentially throwing.
///
/// Use this in cases where you need to store the results in some intermediate
/// storage, such as in a \c variant of \c tuple of the results to also handle
/// the case where moving the value into the storage might throw.
template <typename SigSet>
using add_error_if_move_can_throw_t = squiz::
    transform_completion_signatures_t<SigSet, add_error_if_move_can_throw>;

}  // namespace squiz::detail
