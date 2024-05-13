///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/operation_state.hpp>

namespace squiz {
/// \fn squiz::request_stop(operation_state auto& os)
/// Helper function that calls \c os.request_stop() if the operation-state
/// supports that, otherwise is a no-op.
///
/// This makes it easier for clients of an operation-state to support sending a
/// stop-request to a child operation without having to guard all such calls
/// with an 'if constexpr'.
struct request_stop_t {
  static void operator()(operation_state auto&) noexcept {}
  static void operator()(stoppable_operation_state auto& op) noexcept {
    op.request_stop();
  }
};

inline constexpr request_stop_t request_stop{};
}  // namespace squiz
