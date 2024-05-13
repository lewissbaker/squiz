///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/concepts.hpp>

namespace squiz {

template <typename T>
concept receiver = nothrow_move_constructible<T> && requires(const T& t) {
  { t.get_env() }
  noexcept;
};

template <typename R>
using receiver_env_t = decltype(std::declval<const R&>().get_env());

}  // namespace squiz
