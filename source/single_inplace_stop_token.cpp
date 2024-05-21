///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/single_inplace_stop_token.hpp>

#include <cassert>

namespace squiz {

bool single_inplace_stop_source::request_stop() noexcept {
  void* old_state = state_.load(std::memory_order_relaxed);
  do {
    if (old_state == stop_requested_state() ||
        old_state == stop_requested_callback_done_state()) {
      return false;
    }
  } while (!state_.compare_exchange_weak(
      old_state,
      stop_requested_state(),
      std::memory_order_acq_rel,
      std::memory_order_relaxed));

  if (old_state != nullptr) {
    auto* callback = static_cast<callback_base*>(old_state);
    stopping_thread_id_.store(
        std::this_thread::get_id(), std::memory_order_relaxed);
    callback->execute(callback);
    state_.store(
        stop_requested_callback_done_state(), std::memory_order_release);
    state_.notify_one();
  }

  return true;
}

bool single_inplace_stop_source::try_register_callback(
    callback_base* base) const noexcept {
  void* old_state = state_.load(std::memory_order_acquire);
  do {
    if (old_state == stop_requested_state() ||
        old_state == stop_requested_callback_done_state()) {
      base->execute(base);
      return false;
    }
    assert(old_state == nullptr);
  } while (!state_.compare_exchange_weak(
      old_state,
      static_cast<void*>(base),
      std::memory_order_release,
      std::memory_order_acquire));

  return true;
}

void single_inplace_stop_source::deregister_callback(
    callback_base* base) const noexcept {
  void* old_state = static_cast<void*>(base);
  do {
    if (old_state == stop_requested_state()) {
      if (stopping_thread_id_.load(std::memory_order_relaxed) ==
          std::this_thread::get_id()) {
        // Deregistering from the same thread that is invoking the callback.
        // Either the invocation of the callback has completed and the thread
        // has gone on to do other things (in which case it's safe to destroy)
        // or we are still in the middle of executing the callback (in which
        // case we can't block as it would cause a deadlock).
        return;
      }

      // Deregistering from another thread.
      // Wait until the callback invocation completes.
      while (old_state != stop_requested_callback_done_state()) {
        state_.wait(old_state);
        old_state = state_.load(std::memory_order_acquire);
      }

      return;
    } else if (old_state == stop_requested_callback_done_state()) {
      return;
    }

    assert(old_state == static_cast<void*>(base));
  } while (!state_.compare_exchange_weak(
      old_state,
      nullptr,
      std::memory_order_relaxed,
      std::memory_order_acquire));
}

}  // namespace squiz
