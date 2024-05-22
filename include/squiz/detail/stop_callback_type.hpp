///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <stop_token>

namespace squiz::detail {

// Meta-function for computing the stop-callback type for a given stop-token.
//
// This is mainly to work around usage of std::stop_token which doesn't yet have
// the member 'callback_type' type alias template.

template <typename StopToken, typename CB>
struct stop_callback_type {
  using type = typename StopToken::template callback_type<CB>;
};

template <typename CB>
struct stop_callback_type<std::stop_token, CB> {
  using type = std::stop_callback<CB>;
};

template <typename StopToken, typename CB>
using stop_callback_type_t = typename stop_callback_type<StopToken, CB>::type;

}  // namespace squiz::detail
