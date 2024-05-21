///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>

#include <squiz/sender.hpp>

namespace squiz {

template <typename T>
concept scheduler =
    std::copyable<T> && std::equality_comparable<T> && requires(const T sched) {
      { sched.schedule() } -> sender;
    };

}  // namespace squiz
