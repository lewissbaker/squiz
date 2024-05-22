///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include <squiz/concepts.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/variant_child_operation.hpp>
#include <squiz/variant_sender.hpp>
#include <squiz/detail/member_type.hpp>

namespace squiz {

/// Helper function for constructing a cond_sender from either
/// a 'true_src' sender or a 'false_src' sender.
template <typename TrueSource, typename FalseSource>
auto cond(
    bool condition,
    TrueSource&& true_src,
    FalseSource&& false_src)  //
    noexcept(
        nothrow_decay_copyable<TrueSource> &&
        nothrow_decay_copyable<FalseSource>)  //
    -> variant_sender<
        std::remove_cvref_t<TrueSource>,
        std::remove_cvref_t<FalseSource>> {
  if (condition) {
    return {std::in_place_index<0>, std::forward<TrueSource>(true_src)};
  } else {
    return {std::in_place_index<1>, std::forward<FalseSource>(false_src)};
  }
}

// Specialisation for the case where both TrueSource and FalseSource
// are the same type and thus we don't need a cond_sender to allow
// holding either type. We can just return the same type as the input,
// and only choose which of the arguments to return.
template <typename TrueSource, typename FalseSource>
  requires std::same_as<
               std::remove_cvref_t<TrueSource>,
               std::remove_cvref_t<FalseSource>>
auto cond(bool condition, TrueSource&& true_src, FalseSource&& false_src) noexcept(
    nothrow_decay_copyable<TrueSource> &&
    nothrow_decay_copyable<FalseSource>) -> std::remove_cvref_t<TrueSource> {
  if (condition) {
    return std::forward<TrueSource>(true_src);
  } else {
    return std::forward<FalseSource>(false_src);
  }
}

}  // namespace squiz
