///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cassert>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/manual_child_operation.hpp>
#include <squiz/parameter_type.hpp>
#include <squiz/receiver.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/deliver_result.hpp>
#include <squiz/detail/member_type.hpp>

namespace squiz {

template <typename Source, typename Receiver>
class statement_op
  : public inlinable_operation_state<statement_op<Source, Receiver>, Receiver>
  , public manual_child_operation<
        statement_op<Source, Receiver>,
        receiver_env_t<Receiver>,
        source_tag,
        Source> {
  using inlinable_base = inlinable_operation_state<statement_op, Receiver>;
  using child_base = manual_child_operation<
      statement_op,
      receiver_env_t<Receiver>,
      source_tag,
      Source>;

public:
  statement_op(Source&& source, Receiver r) noexcept(
      child_base::is_nothrow_connectable)
    : inlinable_base(std::move(r)) {
    child_base::construct(std::forward<Source>(source));
  }

  ~statement_op() {
    if (!state_.is_destroyed()) {
      child_base::destruct();
    }
  }

  void start() noexcept { child_base::start(); }

  void request_stop() noexcept
    requires child_base::is_stoppable
  {
    if (state_.flags.load(std::memory_order_relaxed) != 0) {
      return;
    }

    std::uint8_t old_state = state_.flags.fetch_add(
        stop_request_active_flag, std::memory_order_relaxed);
    if (old_state == 0) {
      child_base::request_stop();

      old_state = state_.flags.fetch_sub(
          stop_request_active_flag, std::memory_order_acq_rel);
      if (old_state == (stop_request_active_flag | completed_flag)) {
        child_base::destruct();
        detail::deliver_result(this->get_receiver(), state_.result);
      }
    }
  }

  template <typename Tag, typename... Datums>
  void set_result(
      source_tag,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    constexpr bool need_to_copy_datums =
        !(squiz::enable_pass_by_value<Datums> && ...);
    constexpr bool is_nothrow =
        (std::is_nothrow_move_constructible_v<Datums> && ...);

    if constexpr (child_base::is_stoppable) {
      // A potential race with request_stop().
      using result_t = std::tuple<Tag, Datums...>;

      std::uint8_t old_state = 0;
      if (state_.flags.compare_exchange_strong(
              old_state, completed_flag, std::memory_order_relaxed)) {
        // Successfully marked operation as completed.
        // Safe to destroy the operation-state as reuest_stop() will not access
        // it.

        if constexpr (need_to_copy_datums) {
          // Some params passed by ref - need to make a copy in case they
          // are references to objects in the operation-state.
          try {
            auto& datums_copy = state_.result.template emplace<result_t>(
                Tag{}, std::forward<Datums>(datums)...);
            child_base::destruct();
	    detail::deliver_result_tuple(this->get_receiver(), datums_copy);
          } catch (...) {
            if constexpr (!is_nothrow) {
              child_base::destruct();
              squiz::set_error<std::exception_ptr>(
                  this->get_receiver(), std::current_exception());
            } else {
              assert(false);
              std::unreachable();
            }
          }
        } else {
          child_base::destruct();
          squiz::set_result(
              this->get_receiver(),
              sig,
              squiz::forward_parameter<Datums>(datums)...);
        }
      } else {
        // A concurrent call to request_stop() is active.
        // Need to store the result in the op-state so that when
        // we set the 'completed_flag' that if the stop-request
        // is still active that we can defer destruction of the
        // child op-state and delivery of the result to it.
        try {
          auto& datums_copy = state_.result.template emplace<result_t>(
              Tag{}, std::forward<Datums>(datums)...);
          old_state =
              state_.flags.fetch_add(completed_flag, std::memory_order_acq_rel);
          if (old_state == 0) {
            // The request_stop() call completed before we set the
            // completed_flag. We can just destroy the op-state and deliver the
            // result.
            child_base::destruct();
	    detail::deliver_result_tuple(this->get_receiver(), datums_copy);
          }
        } catch (...) {
          if constexpr (!is_nothrow) {
            // Copying the results failed with an exception
            using error_result_t = std::tuple<error_tag, std::exception_ptr>;
            auto& error_result = state_.result.template emplace<error_result_t>(
                error_tag{}, std::current_exception());
            old_state = state_.flags.fetch_add(
                completed_flag, std::memory_order_acq_rel);
            if (old_state == 0) {
              // The request_stop() call completed before we set the
              // completed_flag. We can just destroy the op-state and deliver
              // the result.
              child_base::destruct();
              squiz::set_error<std::exception_ptr>(
                  this->get_receiver(), std::get<1>(error_result));
            }
          } else {
            assert(false);
            std::unreachable();
          }
        }
      }
    } else {
      // Unstoppable case
      if constexpr (need_to_copy_datums) {
        // Some datums are passed by reference.
        // They might be references to objects in the op-state.
        // So we copy them to the stack first before destroying the op-state.
        try {
          std::tuple<Tag, Datums...> datums_copy{Tag{}, std::forward<Datums>(datums)...};
          child_base::destruct();
          state_.destroyed = true;
	  detail::deliver_result_tuple(this->get_receiver(), datums_copy);
        } catch (...) {
          if constexpr (!is_nothrow) {
            child_base::destruct();
            state_.destroyed = true;
            squiz::set_error<std::exception_ptr>(
                this->get_receiver(), std::current_exception());
          } else {
            assert(false);
            std::unreachable();
          }
        }
      } else {
        // All datums are passed-by-value - no need to copy locally first.
        child_base::destruct();
        state_.destroyed = true;
        squiz::set_result(
            this->get_receiver(),
            sig,
            squiz::forward_parameter<Datums>(datums)...);
      }
    }
  }

  receiver_env_t<Receiver> get_env(source_tag) noexcept {
    return this->get_receiver().get_env();
  }

private:
  using result_variant_t = detail::completion_signatures_to_variant_of_tuple_t<
      detail::add_error_if_move_can_throw_t<
          completion_signatures_for_t<Source, receiver_env_t<Receiver>>>>;

  struct unstoppable_state {
    bool is_destroyed() const noexcept { return destroyed; }
    bool destroyed{false};
  };

  struct stoppable_state {
    bool is_destroyed() const noexcept {
      return flags.load(std::memory_order_relaxed) != 0;
    }
    std::atomic<std::uint8_t> flags{0};
    result_variant_t result;
  };

  static constexpr std::uint8_t completed_flag = 1;
  static constexpr std::uint8_t stop_request_active_flag = 2;

  std::conditional_t<
      child_base::is_stoppable,
      stoppable_state,
      unstoppable_state>
      state_;
};

template <typename Source>
struct statement_sender {
  Source source;

  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> detail::add_error_if_move_can_throw_t<completion_signatures_for_t<
          detail::member_type_t<Self, Source>,
          Env...>>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable()
      -> std::bool_constant<is_always_nothrow_connectable_v<
          detail::member_type_t<Self, Source>,
          Env...>>;

  template <typename Self, typename Receiver>
  statement_op<detail::member_type_t<Self, Source>, Receiver>
  connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<
          statement_op<detail::member_type_t<Self, Source>, Receiver>,
          detail::member_type_t<Self, Source>,
          Receiver>) {
    return statement_op<detail::member_type_t<Self, Source>, Receiver>{
        std::forward<Self>(self).source, std::move(r)};
  }
};

template <typename Source>
statement_sender(Source) -> statement_sender<Source>;

}  // namespace squiz
