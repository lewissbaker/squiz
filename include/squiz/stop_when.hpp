///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <squiz/child_operation.hpp>
#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/parameter_type.hpp>
#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/stop_possible.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/deliver_result.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/store_result.hpp>

namespace squiz {

struct trigger_tag {};

template <typename Source, typename Trigger, typename Receiver>
class stop_when_op
  : public inlinable_operation_state<
        stop_when_op<Source, Trigger, Receiver>,
        Receiver>
  , public child_operation<
        stop_when_op<Source, Trigger, Receiver>,
        detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>,
        source_tag,
        Source>
  , public child_operation<
        stop_when_op<Source, Trigger, Receiver>,
        detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>,
        trigger_tag,
        Trigger> {
  using env = detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>;

  using inline_base = inlinable_operation_state<stop_when_op, Receiver>;
  using source_child_base =
      child_operation<stop_when_op, env, source_tag, Source>;
  using trigger_child_base =
      child_operation<stop_when_op, env, trigger_tag, Trigger>;

  static constexpr bool is_stop_possible =
      is_stop_possible_v<receiver_env_t<Receiver>> &&
      (source_child_base::is_stoppable || trigger_child_base::is_stoppable);

  using state_t = std::uint8_t;

public:
  explicit stop_when_op(Source&& source, Trigger&& trigger, Receiver r) noexcept(
      source_child_base::is_nothrow_connectable &&
      trigger_child_base::is_nothrow_connectable)
    : inline_base(std::move(r))
    , source_child_base(std::forward<Source>(source))
    , trigger_child_base(std::forward<Trigger>(trigger)) {}

  void start() noexcept {
    source_child_base::start();

    // NOTE: Disabling this optimization to allow the trigger operation to start and
    // release its resources on completion. Once we come up with a way to ensure that
    // the operation-states are promptly destroyed upon completion we can reinstate
    // this optimization.
#if 0
    if (state_.load(std::memory_order_acquire) == source_completed_flag) {
      // Completed synchronously - don't bother to start the trigger operation.
      deliver_result();
      return;
    }
#endif

    trigger_child_base::start();

    constexpr state_t both_completed =
        source_completed_flag | trigger_completed_flag;
    constexpr state_t both_completed_and_started =
        both_completed | children_started_flag;

    // Check for the case where both have completed synchronously to avoid
    // any further atomic RMW ops.
    if (state_.load(std::memory_order_acquire) == both_completed) {
      deliver_result();
      return;
    }

    // Otherwise, we set the 'started' flag to indicate that it is safe
    // subsequent completions to call request_stop() on the other operation.
    // Handle the cases of deferred completion and deferred stop-requests
    // that were waiting for start() to finish.
    state_t old_state =
        state_.fetch_add(children_started_flag, std::memory_order_acq_rel);
    if (old_state == both_completed) {
      // Ok to complete - neither completion will have called request_stop() on
      // the other yet.
      deliver_result();
    } else if (old_state == source_completed_flag) {
      trigger_child_base::request_stop();

      old_state =
          state_.fetch_add(stop_request_done_flag, std::memory_order_acq_rel);
      if (old_state == both_completed_and_started) {
        deliver_result();
      }
    } else if (old_state == trigger_completed_flag) {
      source_child_base::request_stop();

      old_state =
          state_.fetch_add(stop_request_done_flag, std::memory_order_acq_rel);
      if (old_state == both_completed_and_started) {
        deliver_result();
      }
    }
  }

  void request_stop() noexcept
    requires is_stop_possible
  {
    // Only valid to call request_stop() after start() has returned.
    assert(
        (state_.load(std::memory_order_relaxed) & children_started_flag) != 0);

    state_t old_state = children_started_flag;
    if (state_.compare_exchange_strong(
            old_state,
            children_started_flag | stop_requested_flag,
            std::memory_order_relaxed)) {
      // The call to request_stop() occurred before either of the source or
      // trigger operations completed.
      source_child_base::request_stop();
      trigger_child_base::request_stop();
    }
  }

  template <typename SignalTag, typename... Datums>
  void set_result(
      source_tag,
      result_t<SignalTag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    state_t old_state = state_.load(std::memory_order_acquire);

    // Check for the trivial cases where we can just forward the result through.
    if (old_state ==
            (trigger_completed_flag | children_started_flag |
             stop_request_done_flag) ||
        old_state ==
            (trigger_completed_flag | children_started_flag |
             stop_requested_flag)) {
      // Trigger completed before the source and there are no active calls to
      // request_stop() on the source operation, so we can just forward through
      // the result.
      squiz::set_result(
          this->get_receiver(),
          squiz::result<SignalTag, Datums...>,
          squiz::forward_parameter<Datums>(datums)...);
      return;
    }

    // Otherwise, we need to store the result in the tuple and then signal that
    // the source operation has completed.
    detail::store_result(result_, sig, datums...);

    source_completed();
  }

  template <typename SignalTag, typename... Datums>
  void set_result(
      trigger_tag,
      result_t<SignalTag, Datums...>,
      parameter_type<Datums>...) noexcept {
    trigger_completed();
  }

  env get_env(source_tag) const noexcept {
    return env{this->get_receiver().get_env()};
  }

  env get_env(trigger_tag) const noexcept {
    return env{this->get_receiver().get_env()};
  }

private:
  using result_variant_t = detail::completion_signatures_to_variant_of_tuple_t<
      detail::add_error_if_move_can_throw_t<completion_signatures_for_t<
          Source,
          detail::env_with_stop_possible<receiver_env_t<Receiver>>>>>;

  static bool ok_to_complete(state_t state) noexcept {
    constexpr state_t required_flags =
        source_completed_flag | children_started_flag | trigger_completed_flag;
    if constexpr (is_stop_possible) {
      return state == (required_flags | stop_request_done_flag) ||
          state == (required_flags | stop_requested_flag);
    } else {
      return state == (required_flags | stop_request_done_flag);
    }
  }

  void source_completed() noexcept {
    state_t old_state =
        state_.fetch_add(source_completed_flag, std::memory_order_acq_rel);
    if (old_state ==
            (children_started_flag | trigger_completed_flag |
             stop_request_done_flag) ||
        old_state ==
            (children_started_flag | trigger_completed_flag |
             stop_requested_flag)) {
      deliver_result();
    } else if (old_state == children_started_flag) {
      trigger_child_base::request_stop();

      old_state =
          state_.fetch_add(stop_request_done_flag, std::memory_order_acq_rel);
      if (old_state ==
          (children_started_flag | trigger_completed_flag |
           source_completed_flag)) {
        deliver_result();
      }
    }
  }

  void trigger_completed() noexcept {
    state_t old_state = state_.load(std::memory_order_acquire);
    if (old_state ==
            (children_started_flag | source_completed_flag |
             stop_request_done_flag) ||
        old_state ==
            (children_started_flag | source_completed_flag |
             stop_requested_flag)) {
      deliver_result();
      return;
    }

    old_state =
        state_.fetch_add(trigger_completed_flag, std::memory_order_acq_rel);
    if (old_state ==
            (children_started_flag | source_completed_flag |
             stop_request_done_flag) ||
        old_state ==
            (children_started_flag | source_completed_flag |
             stop_requested_flag)) {
      deliver_result();
    } else if (old_state == children_started_flag) {
      source_child_base::request_stop();

      old_state =
          state_.fetch_add(stop_request_done_flag, std::memory_order_acq_rel);
      if (old_state ==
          (children_started_flag | trigger_completed_flag |
           source_completed_flag)) {
        deliver_result();
      }
    }
  }

  void deliver_result() noexcept {
    detail::deliver_result(this->get_receiver(), result_);
  }

  static constexpr state_t children_started_flag = 1;
  static constexpr state_t source_completed_flag = 2;
  static constexpr state_t trigger_completed_flag = 4;
  static constexpr state_t stop_requested_flag = 8;
  static constexpr state_t stop_request_done_flag = 16;

  std::atomic<state_t> state_{0};
  result_variant_t result_;
};

template <typename Source, typename Trigger>
struct stop_when_sender {
  Source source;
  Trigger trigger;

  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> detail::add_error_if_move_can_throw_t<completion_signatures_for_t<
          detail::member_type_t<Self, Source>,
          detail::make_env_with_stop_possible_t<Env>...>>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<
          (is_always_nothrow_connectable_v<
               detail::member_type_t<Self, Source>,
               detail::make_env_with_stop_possible_t<Env>...> &&
           is_always_nothrow_connectable_v<
               detail::member_type_t<Self, Trigger>,
               detail::make_env_with_stop_possible_t<Env>...>)>;

  template <typename Self, typename Receiver>
  stop_when_op<
      detail::member_type_t<Self, Source>,
      detail::member_type_t<Self, Trigger>,
      Receiver>
  connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<
          stop_when_op<
              detail::member_type_t<Self, Source>,
              detail::member_type_t<Self, Trigger>,
              Receiver>,
          detail::member_type_t<Self, Source>,
          detail::member_type_t<Self, Trigger>,
          Receiver>) {
    return stop_when_op<
        detail::member_type_t<Self, Source>,
        detail::member_type_t<Self, Trigger>,
        Receiver>{
        std::forward<Self>(self).source,
        std::forward<Self>(self).trigger,
        std::move(r)};
  }
};

template <typename Source, typename Trigger>
stop_when_sender(Source, Trigger) -> stop_when_sender<Source, Trigger>;

}  // namespace squiz
