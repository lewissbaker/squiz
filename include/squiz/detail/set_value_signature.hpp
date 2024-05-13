///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <type_traits>

#include <squiz/completion_signatures.hpp>

namespace squiz::detail {

/// set_value_signature is a meta-function that takes a pack of result-types and
/// produces the \c set_value_t(Vs...) completion-signature type, handling the
/// case where the input is a single result-type of void-type and mapping this
/// to set_value_t() instead of set_value_t(void).
template <typename... ResultTypes>
struct set_value_signature {
  using type = set_value_t(ResultTypes...);
};
template <typename ResultType>
requires std::is_void_v<ResultType>
struct set_value_signature<ResultType> {
  using type = set_value_t();
};

template <typename... ResultTypes>
using set_value_signature_t =
    typename set_value_signature<ResultTypes...>::type;
  
}  // namespace squiz::detail
