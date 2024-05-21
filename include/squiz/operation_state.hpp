///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

namespace squiz {

template <typename T>
concept operation_state = std::destructible<T> && requires(T& op) {
  { op.start() } noexcept -> std::same_as<void>;
};

/// An operation_state that supports receiving a stop-request by
/// calling the request_stop() member-function of the operation-state.
///
/// The request_stop() method may be called at most once on a given object.
/// Any call to request_stop() must happen-after the call to start() and
/// must happen-before a call to the destructor of the operation-state
/// (like calling any member-function on an object).
template <typename T>
concept stoppable_operation_state = operation_state<T> && requires(T& op) {
  { op.request_stop() } noexcept -> std::same_as<void>;
};

}  // namespace squiz
