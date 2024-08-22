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
#include <variant>

#include <squiz/child_operation.hpp>
#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/stop_possible.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/deliver_result.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/smallest_unsigned_integer.hpp>

namespace squiz {

namespace first_succ_detail {

// The results of the child operations need to be categorised into results that
// can be stored safely in a single variant (because we can guarantee that only
// a single thread will be writing to it) and results that need to be given
// independent storage (because the act of storing those results might throw
// an exception so we need to successfully store the result before we can signal
// that this value will be the result. As there might be multiple such
// completions happening at the same time, each of them needs to be written to
// separate storage.
//
// Note that the error/stopped results will only need to be stored in the case
// that none of the operations completed with a value, so these results can be
// stored in the same union as the values.
//
// In order to make dispatching the final result a single visit of a variant, we
// need to also include in the result variant pointers to tuple-results for each
// of the possible throwing value results.

struct nothrow_values_and_stopped_errors {
  static auto apply(stopped_t) -> completion_signatures<stopped_t>;

  template <typename Error>
  static auto apply(error_t<Error>)
      -> completion_signatures<error_t<Error>, error_t<std::exception_ptr>>;

  template <typename Error>
    requires std::is_nothrow_move_constructible_v<Error>
  static auto apply(error_t<Error>) -> completion_signatures<error_t<Error>>;

  template <typename... Values>
  static auto apply(value_t<Values...>)
      -> completion_signatures<error_t<std::exception_ptr>>;

  template <typename... Values>
    requires(std::is_nothrow_move_constructible_v<Values> && ...)
  static auto
      apply(value_t<Values...>) -> completion_signatures<value_t<Values...>>;
};

struct throwing_values {
  static auto apply(stopped_t) -> completion_signatures<>;

  template <typename Error>
  static auto apply(error_t<Error>) -> completion_signatures<>;

  template <typename... Values>
    requires(std::is_nothrow_move_constructible_v<Values> && ...)
  static auto apply(value_t<Values...>) -> completion_signatures<>;

  template <typename... Values>
  static auto
      apply(value_t<Values...>) -> completion_signatures<value_t<Values...>>;
};

template <typename Sigs>
struct variant_of_tuple_or_empty {
  using type = detail::completion_signatures_to_variant_of_tuple_t<Sigs>;
};

template <>
struct variant_of_tuple_or_empty<completion_signatures<>> {
  using type = std::monostate;
};

template <typename Sigs>
using variant_of_tuple_or_empty_t =
    typename variant_of_tuple_or_empty<Sigs>::type;

template <typename InlineSigs, typename ExternalSigs>
struct sigs_to_result_variant;

template <typename... InlineSigs, typename... ExternalSigs>
struct sigs_to_result_variant<
    completion_signatures<InlineSigs...>,
    completion_signatures<ExternalSigs...>> {
  using type = std::variant<
      std::monostate,
      detail::completion_signature_to_tuple_t<InlineSigs>...,
      detail::completion_signature_to_tuple_t<ExternalSigs>&...>;
};

template <typename InlineSigs, typename ExternalSigs>
using sigs_to_result_variant_t =
    typename sigs_to_result_variant<InlineSigs, ExternalSigs>::type;

}  // namespace first_succ_detail

template <typename Receiver, typename Ids, typename... ChildSenders>
struct first_successful_op;

template <typename Receiver, std::size_t... Ids, typename... ChildSenders>
struct first_successful_op<
    Receiver,
    std::index_sequence<Ids...>,
    ChildSenders...>
  : inlinable_operation_state<
        first_successful_op<
            Receiver,
            std::index_sequence<Ids...>,
            ChildSenders...>,
        Receiver>
  , child_operation<
        first_successful_op<
            Receiver,
            std::index_sequence<Ids...>,
            ChildSenders...>,
        detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>,
        indexed_source_tag<Ids>,
        ChildSenders>... {
private:
  static_assert(sizeof...(ChildSenders) >= 2);

  using env_t = detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>;
  using inlinable_base_t =
      inlinable_operation_state<first_successful_op, Receiver>;

  template <std::size_t Idx>
  using child_base_t = child_operation<
      first_successful_op,
      env_t,
      indexed_source_tag<Idx>,
      ChildSenders...[Idx]>;

  static constexpr bool is_request_stop_supported =
      is_stop_possible_v<receiver_env_t<Receiver>> &&
      (child_base_t<Ids>::is_stoppable || ...);

public:
  explicit first_successful_op(Receiver r, ChildSenders&&... children) noexcept(
      (std::is_nothrow_constructible_v<child_base_t<Ids>, ChildSenders> && ...))
    : inlinable_base_t(std::move(r))
    , child_base_t<Ids>(std::forward<ChildSenders>(children))... {}

  void start() noexcept {
    // Start all of the child operations
    (child_base_t<Ids>::start(), ...);

    const auto all_children_complete = [](state_t state) noexcept {
      return state < count_increment;
    };
    const auto has_pending_success = [](state_t state) noexcept {
      return (state & has_value_flag) != 0;
    };

    // Once they have all started, mark the state_ variable to indicate that if
    // one of them completes now that it is now safe to call request_stop() on
    // child operations.
    state_t old_state = state_.load(std::memory_order_acquire);
    do {
      if (all_children_complete(old_state)) {
        state_.store(old_state | started_flag, std::memory_order_relaxed);
        deliver_result();
        return;
      }

      if (has_pending_success(old_state)) {
        // An operation succeeded already, and there are still children that
        // have not yet completed, the first operation to succeed has delegated
        // to start() the responsibility for cancelling the others. Do this
        // before setting the 'started' flag as setting the 'started' flag
        // allows the last operation to complete to deliver the overall result.
        request_stop_on_all_children();

        // Set the 'started' flag.
        old_state = state_.fetch_add(started_flag, std::memory_order_acq_rel);

        // The last child may have completed while we were calling
        // request_stop() on the child operations, in which case it has
        // delegated to start() the responsibility for delivering the overall
        // result.
        if (all_children_complete(old_state)) {
          deliver_result();
        }

        return;
      }
    } while (!state_.compare_exchange_weak(
        old_state,
        old_state | started_flag,
        std::memory_order_release,
        std::memory_order_acquire));
  }

  void request_stop() noexcept
    requires is_request_stop_supported
  {
    const state_t old_state =
        state_.fetch_add(stop_requested_flag, std::memory_order_relaxed);
    assert((old_state & stop_requested_flag) == 0);
    assert((old_state & started_flag) != 0);
    const bool has_succeeded = (old_state & has_value_flag) != 0;
    const bool any_children_still_running = (old_state >= count_increment);
    if (any_children_still_running && !has_succeeded) {
      request_stop_on_all_children();
    }
  }

  template <std::size_t Idx, typename... Vs>
  void set_result(
      indexed_source_tag<Idx>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    constexpr bool is_nothrow =
        (std::is_nothrow_move_constructible_v<Vs> && ...);

    state_t old_state = state_.load(std::memory_order_acquire);
    if ((old_state & has_value_flag) != 0) {
      // There is already a success value.
      // Just decrement the count and discard this value result as it was not
      // the first result.
      discard_value_result();
      return;
    }

    const auto can_complete_inline = [&]() noexcept {
      return (old_state < (2 * count_increment)) &&
          (old_state & started_flag) != 0;
    };

    if (can_complete_inline()) {
      // This is the last operation to complete, and start() has finished
      // launching all of the operations, so we can complete by just
      // passing this value-result straight through without first storing
      // it in the operation-state.

      if constexpr (is_request_stop_supported) {
        // Decrement the ref-count and add the 'has_value_flag' before
        // completing so that a subsequent call to this->request_stop()
        // doesn't try to call request_stop() on the child operations.
        state_.fetch_sub(
            count_increment - has_value_flag, std::memory_order_relaxed);
      }

      squiz::set_value<Vs...>(
          this->get_receiver(), squiz::forward_parameter<Vs>(vs)...);
      return;
    }

    // Otherwise, as there is at least one other operation still running, we
    // need to store the value and send a stop-request to the outstanding
    // operations.
    if constexpr (is_nothrow) {
      // We can set the 'has_value_flag' first, and if we win the race then we
      // can store it in the 'nothrow_result' variant, without worrying about
      // races from other operations completing with values and without worrying
      // that storing this result will fail.
      old_state = state_.fetch_or(has_value_flag, std::memory_order_acquire);
      if ((old_state & has_value_flag) != 0) {
        // Other value operation won the race.
        discard_value_result();
        return;
      }

      if (can_complete_inline()) {
        // This is the last op, no need to store the value.
        squiz::set_value<Vs...>(
            this->get_receiver(), squiz::forward_parameter<Vs>(vs)...);
        return;
      }

      // Otherwise, store the result in the operation-state.
      auto& result_tuple =
          result_variant_.template emplace<std::tuple<value_tag, Vs...>>(
              value_tag{}, squiz::forward_parameter<Vs>(vs)...);

      // If start() has finished starting all of the child operations then we
      // can call request_stop() on the child operations here, otherwise we
      // have delegated to start() to call request_stop() on the child
      // operations. We also want to avoid calling request_stop() on children
      // of a call to this->request_stop() has already been made.
      if ((old_state & (started_flag | stop_requested_flag)) == started_flag) {
        // There should still be at least one other child operation still
        // running, otherwise we would have entered the 'can_complete_inline'
        // branch above.
        assert(old_state >= (2 * count_increment));
        request_stop_on_all_children_except<Idx>();
      }

      // Now mark the operation as complete by decrementing the count.
      old_state = state_.fetch_sub(count_increment, std::memory_order_acq_rel);
      if (can_complete_inline()) {
        // This  was the last operation to complete.
        detail::deliver_result_tuple(this->get_receiver(), result_tuple);
      }

      return;
    } else {
      // Handle the case where copying the value might throw an exception.

      // First need to store a copy of the values in the storage reserved for
      // it.

      try {
        // Copy the results into local storage of the operation-state.
        // This is the only line in the try-block that can throw.
        auto& result_tuple =
            std::get<Idx>(throwing_value_results_)
                .template emplace<std::tuple<value_tag, Vs...>>(
                    value_tag{}, squiz::forward_parameter<Vs>(vs)...);

        old_state = state_.fetch_or(has_value_flag, std::memory_order_release);
        if ((old_state & has_value_flag) == 0) {
          // We won the race and get to set the value-result

          // Set the value-result to a reference to the storage we initialized
          // above.
          result_variant_.template emplace<std::tuple<value_tag, Vs...>&>(
              result_tuple);

          old_state =
              state_.fetch_sub(count_increment, std::memory_order_acq_rel);
          if (can_complete_inline()) {
            detail::deliver_result_tuple(this->get_receiver(), result_tuple);
          }
        } else {
          // Otherwise we lost the race - some other child succeeded first.
          // We can now discard the value results.
          std::get<Idx>(throwing_value_results_)
              .template emplace<std::monostate>();

          discard_value_result();
        }

      } catch (...) {
        handle_non_value_result(
            error_t<std::exception_ptr>{}, std::current_exception());
      }
    }
  }

  template <
      std::size_t Idx,
      one_of<error_tag, stopped_tag> Tag,
      typename... Datums>
  void set_result(
      indexed_source_tag<Idx>,
      result_t<Tag, Datums...> tag,
      parameter_type<Datums>... datums) noexcept {
    handle_non_value_result(tag, squiz::forward_parameter<Datums>(datums)...);
  }

  template <std::size_t Idx>
  env_t get_env(indexed_source_tag<Idx>) noexcept {
    return env_t{this->get_receiver().get_env()};
  }

private:
  void request_stop_on_all_children() noexcept {
    (child_base_t<Ids>::request_stop(), ...);
  }

  template <std::size_t Idx>
  void request_stop_on_all_children_except() noexcept {
    ((Ids != Idx ? child_base_t<Ids>::request_stop() : (void)0), ...);
  }

  // Call this to signal that an operation completed with a value-result which
  // was not the first value-result and so is discarded. In this case we know
  // that the result is not going to be the final result as we already have a
  // final result (the other value).
  //
  // This decrements the refcount and then delivers the result if it was the
  // last-ref count to be delivered.
  void discard_value_result() noexcept {
    const state_t old_state =
        state_.fetch_sub(count_increment, std::memory_order_acq_rel);
    assert((old_state & has_value_flag) != 0);
    if ((old_state < (2 * count_increment)) &&
        ((old_state & started_flag) != 0)) {
      deliver_result();
    }
  }

  template <typename Tag, typename... Datums>
  void handle_non_value_result(
      result_t<Tag, Datums...>, parameter_type<Datums>... datums) noexcept {
    state_t old_state = state_.load(std::memory_order_acquire);

    // Try to decrement the ref-count, but only if it is not the last reference.
    while ((old_state >= (2 * count_increment))) {
      if (state_.compare_exchange_weak(
              old_state,
              old_state - count_increment,
              std::memory_order_release,
              std::memory_order_acquire)) {
        // Successfully decremented the ref-count and discarded the result.
        // Someone else will deliver the result.
        return;
      }
    }

    // If we get here then we know that this is the last result and so we may
    // need to deliver this result as the overall result.

    if ((old_state & has_value_flag) != 0) {
      // Already has a value-result, discarding this non-value result.

      if ((old_state & started_flag) == 0) {
	// Hasn't started yet, we might be racing with start()
	// setting the started_flag so need to do an atomic operation
	// to decide the race.

	old_state = state_.fetch_sub(count_increment, std::memory_order_acq_rel);
	if ((old_state & started_flag) == 0) {
	  // We won the race. start() will call deliver_result().
	  return;
	}
      }

      deliver_result();
    } else {
      // No prior value-result.
      // The result will be this non-value result.

      if ((old_state & started_flag) == 0) {
	// The start() function hasn't set the 'started_flag' yet.

	// We can't necessarily deliver the result here, so instead we
	// need to store the result in the result_variant_ and then try to
	// decrement the ref-count. If the started_flag is still not set
	// then the start() function will deliver the result when it
	// evenutally gets to set the started_flag and sees that the
	// ref-count is zero. Otherwise, if the started_flag is set first
	// then we can deliver the result immediately here.
        try {
          auto& result_tuple =
              result_variant_.template emplace<std::tuple<Tag, Datums...>>(
                  Tag{}, squiz::forward_parameter<Datums>(datums)...);

          old_state =
              state_.fetch_sub(count_increment, std::memory_order_acq_rel);
          if ((old_state & started_flag) != 0) {
            detail::deliver_result_tuple(this->get_receiver(), result_tuple);
          }
        } catch (...) {
          constexpr bool is_nothrow =
              (std::is_nothrow_move_constructible_v<Datums> && ...);
          if constexpr (!is_nothrow) {
            auto& error_tuple = result_variant_.template emplace<
                std::tuple<error_tag, std::exception_ptr>>(
                error_tag{}, std::current_exception());

            old_state =
                state_.fetch_sub(count_increment, std::memory_order_acq_rel);
            if ((old_state & started_flag) != 0) {
              detail::deliver_result_tuple(this->get_receiver(), error_tuple);
            }
          }
        }
      } else {
	// The started flag has already been set, so we can just deliver the
	// result.
	
        if constexpr (is_request_stop_supported) {
          // Set the ref-count to zero to prevent subsequent call to
          // request_stop() from trying to call request_stop() on child
          // operations.
          state_.store(old_state - count_increment, std::memory_order_relaxed);
        }

        squiz::set_result(
            this->get_receiver(),
            result_t<Tag, Datums...>{},
            squiz::forward_parameter<Datums>(datums)...);
      }
    }
  }

  /// Deliver the result that was stored in the 'result_variant_' data-member.
  void deliver_result() noexcept {
    assert(
        result_variant_.index() != 0 &&
        !result_variant_.valueless_by_exception());
    detail::deliver_result(this->get_receiver(), result_variant_);
  }

  using all_sigs_t = merge_completion_signatures_t<
      completion_signatures_for_t<ChildSenders, env_t>...>;

  using result_variant_t = first_succ_detail::sigs_to_result_variant_t<
      transform_completion_signatures_t<
          all_sigs_t,
          first_succ_detail::nothrow_values_and_stopped_errors>,
      transform_completion_signatures_t<
          all_sigs_t,
          first_succ_detail::throwing_values>>;

  using throwing_value_results_t =
      std::tuple<first_succ_detail::variant_of_tuple_or_empty_t<
          transform_completion_signatures_t<
              completion_signatures_for_t<ChildSenders, env_t>,
              first_succ_detail::throwing_values>>...>;

  static constexpr std::uint8_t started_flag = 1;
  static constexpr std::uint8_t has_value_flag = 2;
  static constexpr std::uint8_t stop_requested_flag = 4;
  static constexpr std::uint8_t count_increment = 8;

  using state_t = detail::smallest_unsigned_integer_for<
      started_flag + has_value_flag + stop_requested_flag +
      (count_increment * sizeof...(ChildSenders))>;

  std::atomic<state_t> state_{count_increment * sizeof...(ChildSenders)};

  // This field stores the final result.
  // This is only allowed to be written by either the first thread to set  the
  // 'has_value_flag' or the thread to decrement the last count_increment.
  [[no_unique_address]] result_variant_t result_variant_;

  // Additional storage for storing a copy of the value where moving the value
  // might throw an exception. Each child operation has a separate storage area
  // to store its 'throwing' value results. If a child has no 'throwing' value
  // results then its corresponding entry in this tuple will be std::monostate.
  [[no_unique_address]] throwing_value_results_t throwing_value_results_;
};

template <typename... ChildSenders>
struct first_successful_sender;

template <typename... ChildSenders>
  requires(sizeof...(ChildSenders) >= 2)
struct first_successful_sender<ChildSenders...> {
  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> detail::add_error_if_move_can_throw_t<
          merge_completion_signatures_t<completion_signatures_for_t<
              ChildSenders,
              detail::make_env_with_stop_possible_t<Env>...>...>>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<
          (is_always_nothrow_connectable_v<
               detail::member_type_t<Self, ChildSenders>,
               detail::make_env_with_stop_possible_t<Env>...> &&
           ...)>;

  template <typename... ChildSenders2>
    requires(sizeof...(ChildSenders2) == sizeof...(ChildSenders))
  first_successful_sender(ChildSenders2&&... children) noexcept(
      (std::is_nothrow_constructible_v<ChildSenders, ChildSenders2> && ...))
    : children_(std::forward<ChildSenders2>(children)...) {}

private:
  template <typename Self, typename Receiver>
  using op_state_t = first_successful_op<
      Receiver,
      std::make_index_sequence<sizeof...(ChildSenders)>,
      detail::member_type_t<Self, ChildSenders>...>;

public:
  template <typename Self, typename Receiver>
  op_state_t<Self, Receiver> connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<
          op_state_t<Self, Receiver>,
          Receiver,
          detail::member_type_t<Self, ChildSenders>...>) {
    return std::apply(
        [&](auto&&... children) {
          return op_state_t<Self, Receiver>{
              std::move(r), static_cast<decltype(children)>(children)...};
        },
        std::forward<Self>(self).children_);
  }

private:
  std::tuple<ChildSenders...> children_;
};

template <typename... Children>
first_successful_sender(Children...) -> first_successful_sender<Children...>;

// Specialization for a single child.
//
// This needs to be separate because in the case of a single child, we don't
// send any additional stop-request to the child, so it's set of
// completion-signatures may be different from the computation used for 2 or
// more children.
//
// Also, define it as an aggregate to allow aggregate-initialization.
template <typename Child>
struct first_successful_sender<Child> {
  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> completion_signatures_for_t<
          detail::member_type_t<Self, Child>,
          Env...>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<is_always_nothrow_connectable_v<
          detail::member_type_t<Self, Child>,
          Env...>>;

  template <typename Self, typename Receiver>
  auto connect(this Self&& self, Receiver r) noexcept(
      is_nothrow_connectable_v<detail::member_type_t<Self, Child>, Receiver>) {
    return std::forward<Self>(self).child.connect(std::move(r));
  }

  Child child;
};

}  // namespace squiz
