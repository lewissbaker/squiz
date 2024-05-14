///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/completion_signatures.hpp>

#include <tuple>

namespace squiz::detail {

/// Meta-function that maps a completion-signature of the form 'Tag(Args...)' to
/// the type 'std::tuple<Tag, Args...>'
template <typename Sig>
struct completion_signature_to_tuple;

template <typename Tag, typename... Args>
struct completion_signature_to_tuple<Tag(Args...)> {
  using type = std::tuple<Tag, Args...>;
};

template <typename Sig>
using completion_signature_to_tuple_t =
    typename completion_signature_to_tuple<Sig>::type;

}  // namespace squiz::detail
