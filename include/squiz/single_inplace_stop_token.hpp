///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <thread>

namespace squiz {

class single_inplace_stop_token;

template <typename CB>
class single_inplace_stop_callback;

class single_inplace_stop_source {
public:
  single_inplace_stop_source() noexcept : state_(nullptr) {}
  ~single_inplace_stop_source() {
    assert(
        state_.load(std::memory_order_relaxed) == stop_requested_state() ||
        state_.load(std::memory_order_relaxed) ==
            stop_requested_callback_done_state() ||
        state_.load(std::memory_order_relaxed) == nullptr);
  }

  bool stop_requested() const noexcept;
  bool request_stop() noexcept;

  single_inplace_stop_token get_token() const noexcept;

private:
  friend class single_inplace_stop_token;

  template <typename CB>
  friend class single_inplace_stop_callback;

  struct callback_base {
    void (*execute)(callback_base*) noexcept;
  };

  bool try_register_callback(callback_base* base) const noexcept;
  void deregister_callback(callback_base* base) const noexcept;

  void* stop_requested_state() const noexcept { return &state_; }
  void* stop_requested_callback_done_state() const noexcept {
    return &stopping_thread_id_;
  }

  // nullptr                    - no callback
  // &this->state_              - stop-requested
  // &this->stopping_thread_id_ - stop-requested-callback-done
  // other                      - pointer to callback_base
  mutable std::atomic<void*> state_;
  mutable std::atomic<std::thread::id> stopping_thread_id_;
};

class single_inplace_stop_token {
public:
  template <typename CB>
  using callback_type = single_inplace_stop_callback<CB>;

  single_inplace_stop_token() noexcept;

  bool stop_possible() const noexcept { return source_ != nullptr; }

  bool stop_requested() const noexcept {
    return source_ != nullptr && source_->stop_requested();
  }

  friend bool operator==(
      single_inplace_stop_token a, single_inplace_stop_token b) noexcept {
    return a.source_ == b.source_;
  }

private:
  template <typename CB>
  friend class single_inplace_stop_callback;

  friend class single_inplace_stop_source;

  explicit single_inplace_stop_token(
      const single_inplace_stop_source* source) noexcept
    : source_(source) {}

  const single_inplace_stop_source* source_;
};

template <typename CB>
class single_inplace_stop_callback
  : private single_inplace_stop_source::callback_base {
public:
  template <typename Init>
    requires std::constructible_from<CB, Init>
  single_inplace_stop_callback(
      single_inplace_stop_token st,
      Init&& init) noexcept(std::is_nothrow_constructible_v<CB, Init>)
    : source_(st.source_)
    , callback_(std::forward<Init>(init)) {
    this->execute = &execute_impl;
    if (source_ != nullptr) {
      if (!source_->try_register_callback(this)) {
        source_ = nullptr;
      }
    }
  }

  single_inplace_stop_callback(single_inplace_stop_callback&&) = delete;
  single_inplace_stop_callback(const single_inplace_stop_callback&) = delete;
  single_inplace_stop_callback&
  operator=(single_inplace_stop_callback&&) = delete;
  single_inplace_stop_callback&
  operator=(const single_inplace_stop_callback&) = delete;

  ~single_inplace_stop_callback() {
    if (source_ != nullptr) {
      source_->deregister_callback(this);
    }
  }

private:
  static void
  execute_impl(single_inplace_stop_source::callback_base* base) noexcept {
    auto& self = *static_cast<single_inplace_stop_callback*>(base);
    self.callback_();
  }

  const single_inplace_stop_source* source_;
  [[no_unique_address]] CB callback_;
};

inline single_inplace_stop_token
single_inplace_stop_source::get_token() const noexcept {
  return single_inplace_stop_token{this};
}

inline bool single_inplace_stop_source::stop_requested() const noexcept {
  void* p = state_.load(std::memory_order_acquire);
  return (
      p == stop_requested_state() || p == stop_requested_callback_done_state());
}

}  // namespace squiz
