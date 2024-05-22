///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/receiver.hpp>
#include <squiz/stop_possible.hpp>
#include <squiz/stoppable_token.hpp>
#include <squiz/detail/stop_callback_type.hpp>

namespace squiz {

template <typename StopToken, typename Receiver>
class stop_token_op
  : public inlinable_operation_state<
        stop_token_op<StopToken, Receiver>,
        Receiver> {
  using inlinable_base = inlinable_operation_state<stop_token_op, Receiver>;

  using state_t = std::uint8_t;

public:
  stop_token_op(StopToken st, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , stop_token_(std::move(st)) {}

  void start() noexcept {
    if (stop_token_.stop_requested()) {
      squiz::set_value<>(this->get_receiver());
      return;
    }

    callback_.emplace(std::move(stop_token_), on_stop_request{*this});

    state_t old_state = state_.fetch_add(
        callback_construction_complete_flag, std::memory_order_acq_rel);
    if (old_state == stop_callback_invoked_flag) {
      callback_.reset();
      squiz::set_value<>(this->get_receiver());
    }
  }

  void request_stop() noexcept
    requires is_stop_possible_v<receiver_env_t<Receiver>>
  {
    state_t old_state =
        state_.fetch_add(stop_requested_flag, std::memory_order_acquire);
    if (old_state == callback_construction_complete_flag) {
      // The stop-request on this operation has arrived before the stop-callback
      callback_.reset();
      squiz::set_stopped(this->get_receiver());
    }
  }

private:
  void on_stop_callback() noexcept {
    state_t old_state =
        state_.fetch_add(stop_callback_invoked_flag, std::memory_order_acquire);
    if (old_state == callback_construction_complete_flag) {
      callback_.reset();
      squiz::set_value<>(this->get_receiver());
    }
  }

  struct on_stop_request {
    stop_token_op& op;
    void operator()() noexcept { op.on_stop_callback(); }
  };

  using callback_t = detail::stop_callback_type_t<StopToken, on_stop_request>;

  static constexpr state_t callback_construction_complete_flag = 1;
  static constexpr state_t stop_callback_invoked_flag = 2;
  static constexpr state_t stop_requested_flag = 4;

  StopToken stop_token_;
  std::atomic<state_t> state_;
  std::optional<callback_t> callback_;
};

template <typename StopToken, typename Receiver>
  requires unstoppable_token<StopToken>
class stop_token_op<StopToken, Receiver>
  : public inlinable_operation_state<
        stop_token_op<StopToken, Receiver>,
        Receiver> {
  using inlinable_base = inlinable_operation_state<stop_token_op, Receiver>;

public:
  stop_token_op(StopToken, Receiver r) noexcept
    : inlinable_base(std::move(r)) {}

  void start() noexcept {}

  void request_stop() noexcept
    requires is_stop_possible_v<receiver_env_t<Receiver>>
  {
    squiz::set_stopped(this->get_receiver());
  }
};

// A sender that completes with set_value() when a stop-request is sent to the
// stop-token. It unsubscribes from the stop-token when the operation is sent
// a stop-request and completes with set_stopped().
template <typename StopToken>
struct stop_token_sender {
  [[no_unique_address]] StopToken stop_token;

  template <typename Env>
  auto get_completion_signatures(Env)
      -> std::conditional_t<
          is_stop_possible_v<Env>,
          completion_signatures<value_t<>, stopped_t>,
          completion_signatures<value_t<>>>;

  template <typename Env>
    requires unstoppable_token<StopToken>
  auto get_completion_signatures(Env) -> std::conditional_t<
                                          is_stop_possible_v<Env>,
                                          completion_signatures<stopped_t>,
                                          completion_signatures<>>;

  static auto is_always_nothrow_connectable() -> std::true_type;

  template <typename Self, typename Receiver>
  stop_token_op<StopToken, Receiver>
  connect(this Self&& self, Receiver r) noexcept {
    return stop_token_op<StopToken, Receiver>{
        std::forward<Self>(self).stop_token, std::move(r)};
  }
};

template <typename StopToken>
stop_token_sender(StopToken) -> stop_token_sender<StopToken>;

}  // namespace squiz
