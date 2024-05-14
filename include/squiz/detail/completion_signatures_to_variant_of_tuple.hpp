///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/completion_signatures.hpp>
#include <squiz/detail/completion_signature_to_tuple.hpp>

#include <variant>

namespace squiz::detail {

/// Meta-function that maps a set of completion-signatures to a std::variant
/// std::monostate and a std::tuple<Tag, Args...> entry, one for each input
/// completion-signature.
template <typename Sigs>
struct completion_signatures_to_variant_of_tuple;

template <typename... Sigs>
struct completion_signatures_to_variant_of_tuple<
    completion_signatures<Sigs...>> {
  using type =
      std::variant<std::monostate, completion_signature_to_tuple_t<Sigs>...>;
};

template <typename Sigs>
using completion_signatures_to_variant_of_tuple_t =
    typename completion_signatures_to_variant_of_tuple<Sigs>::type;
  
}
