///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include <squiz/child_operation.hpp>
#include <squiz/completion_signatures.hpp>
#include <squiz/concepts.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/just.hpp>
#include <squiz/overload.hpp>
#include <squiz/receiver.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/smallest_unsigned_integer.hpp>

namespace squiz {

namespace when_all_detail {

template <typename ValueSig, typename SigSet>
struct when_all_combine_value_sigs_helper;

template <typename... Args, typename... Sigs>
struct when_all_combine_value_sigs_helper<
    value_t<Args...>,
    completion_signatures<Sigs...>> {
  template <typename Sig>
  struct append_sig;

  template <typename... Args2>
  struct append_sig<value_t<Args2...>> {
    using type = value_t<Args..., Args2...>;
  };

  using type = completion_signatures<typename append_sig<Sigs>::type...>;
};

template <typename... SigSets>
struct when_all_combine_value_sigs;

template <>
struct when_all_combine_value_sigs<> {
  using type = completion_signatures<>;
};

template <typename SigSet>
struct when_all_combine_value_sigs<SigSet> {
  using type = SigSet;
};

template <typename... Sigs1, typename Sig2, typename... Rest>
struct when_all_combine_value_sigs<
    completion_signatures<Sigs1...>,
    Sig2,
    Rest...>
  : when_all_combine_value_sigs<
        merge_completion_signatures_t<
            typename when_all_combine_value_sigs_helper<Sigs1, Sig2>::type...>,
        Rest...> {};

template <typename... SigSets>
using when_all_combine_value_sigs_t =
    typename when_all_combine_value_sigs<SigSets...>::type;

// Checks whether a when_all() algorithm with the specified ChildSenders
// will always succeed with a value-result.
template <typename ChildSender, typename... Env>
concept when_all_child_always_succeeds =
    (error_or_stopped_signatures_t<detail::add_error_if_move_can_throw_t<
         completion_signatures_for_t<ChildSender, Env...>>>::size == 0);

}  // namespace when_all_detail

template <typename Receiver, typename Ids, typename... ChildSenders>
struct when_all_op;

template <typename Receiver, std::size_t... Ids, typename... ChildSenders>
struct when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>
  : inlinable_operation_state<
        when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>,
        Receiver>
  , child_operation<
        when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>,
        detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>,
        indexed_source_tag<Ids>,
        ChildSenders>... {
private:
  using env_t = detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>>;
  using inlinable_base_t = inlinable_operation_state<when_all_op, Receiver>;

  template <std::size_t Idx>
  using child_base_t = child_operation<
      when_all_op,
      env_t,
      indexed_source_tag<Idx>,
      ChildSenders...[Idx]>;

public:
  explicit when_all_op(Receiver rcvr, ChildSenders&&... children)
    : inlinable_base_t(std::move(rcvr))
    , child_base_t<Ids>(std::forward<ChildSenders>(children))... {}

  void start() noexcept {
    // Start child operations
    (child_base_t<Ids>::start(), ...);

    // Then update state to set the 'started_flag' to indicate to
    // completion signals that it's ok to either complete the overall operation
    // or request-stop. Note that there may be pending requests to do either
    // of these things which the completion-signal handling deferred to
    // start() to perform once start() has completed - so check for these
    // and either delier the result or request-stop if necessary.
    const state_t old_state =
        state_.fetch_add(started_flag, std::memory_order_release);
    assert((old_state & started_flag) == 0);
    assert((old_state & stop_requested_flag) == 0);
    const bool has_pending_failure = (old_state & failed_flag) != 0;
    const bool any_children_still_running = (old_state >= count_increment);
    if (any_children_still_running) {
      if (has_pending_failure) {
        request_stop_on_all_children();
      }
    } else {
      deliver_result();
    }
  }

  void request_stop() noexcept
    requires is_stop_possible_v<receiver_env_t<Receiver>> &&
      (child_base_t<Ids>::is_stoppable || ...)
  {
    const state_t old_state =
        state_.fetch_add(stop_requested_flag, std::memory_order_relaxed);
    assert((old_state & stop_requested_flag) == 0);
    assert((old_state & started_flag) != 0);
    const bool has_not_failed_yet = (old_state & failed_flag) == 0;
    const bool any_children_still_running = old_state >= count_increment;
    if (has_not_failed_yet && any_children_still_running) {
      request_stop_on_all_children();
    }
  }

  template <std::size_t Idx, typename... Vs>
  void set_result(
      indexed_source_tag<Idx>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    try {
      std::get<Idx>(value_results_)
          .template emplace<std::tuple<value_tag, Vs...>>(
              value_tag{}, squiz::forward_parameter<Vs>(vs)...);
    } catch (...) {
      constexpr bool is_nothrow =
          (std::is_nothrow_move_constructible_v<Vs> && ...);
      if constexpr (!is_nothrow) {
        if (try_set_failure_flag()) {
          error_or_stopped_
              .template emplace<std::tuple<error_tag, std::exception_ptr>>(
                  error_tag{}, std::current_exception());
        }
      } else {
        assert(false);
        std::unreachable();
      }
    }
    arrive();
  }

  template <std::size_t Idx, typename SignalTag, typename... Datums>
    requires(!std::same_as<SignalTag, value_tag>)
  void set_result(
      indexed_source_tag<Idx>,
      result_t<SignalTag, Datums...>,
      parameter_type<Datums>... datums) noexcept {
    if (try_set_failure_flag()) {
      try {
        error_or_stopped_.template emplace<std::tuple<SignalTag, Datums...>>(
            SignalTag{}, squiz::forward_parameter<Datums>(datums)...);
      } catch (...) {
        constexpr bool is_nothrow =
            (std::is_nothrow_move_constructible_v<Datums> && ...);
        if constexpr (!is_nothrow) {
          error_or_stopped_
              .template emplace<std::tuple<error_tag, std::exception_ptr>>(
                  error_tag{}, std::current_exception());
        } else {
          assert(false);
          std::unreachable();
        }
      }
    }
    arrive();
  }

  template <std::size_t Idx>
  env_t get_env(indexed_source_tag<Idx>) noexcept {
    return env_t{this->get_receiver().get_env()};
  }

private:
  bool try_set_failure_flag() noexcept {
    if ((state_.load(std::memory_order_relaxed) & failed_flag) != 0) {
      return false;
    }

    const state_t old_state =
        state_.fetch_or(failed_flag, std::memory_order_acquire);
    if ((old_state & failed_flag) != 0) {
      // Some other operation was the first failure
      return false;
    }

    // If there are operations that haven't completed yet, and start() has
    // finished launching all of the operations the send a stop-request to all
    // children.
    const bool ok_to_call_request_stop =
        (old_state & (started_flag | stop_requested_flag)) == started_flag;
    const bool any_children_still_running =
        (old_state >= (2 * count_increment));

    if (ok_to_call_request_stop && any_children_still_running) {
      request_stop_on_all_children();
    }

    return true;
  }

  void request_stop_on_all_children() noexcept {
    (child_base_t<Ids>::request_stop(), ...);
  }

  void arrive() noexcept {
    const state_t old_state =
        state_.fetch_sub(count_increment, std::memory_order_acq_rel);
    const bool is_last_to_arrive = old_state < (2 * count_increment);
    const bool start_has_finished = (old_state & started_flag) != 0;
    if (is_last_to_arrive && start_has_finished) {
      deliver_result();
    }
  }

  void deliver_result() noexcept {
    if (error_or_stopped_.index() != 0) {
      std::visit(
          overload(
              [](std::monostate) noexcept { std::unreachable(); },
              [this]<typename SignalTag, typename... Datums>(
                  std::tuple<SignalTag, Datums...>& tup) noexcept {
                std::apply(
                    [this](SignalTag, Datums&... datums) noexcept {
                      squiz::set_result(
                          this->get_receiver(),
                          squiz::result<SignalTag, Datums...>,
                          squiz::forward_parameter<Datums>(datums)...);
                    },
                    tup);
              }),
          error_or_stopped_);
    } else {
      deliver_value(std::integral_constant<std::size_t, 0>{}, squiz::value<>);
    }
  }

  template <typename... Vs>
  void deliver_value(
      std::integral_constant<std::size_t, sizeof...(ChildSenders)>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    squiz::set_value<Vs...>(
        this->get_receiver(), squiz::forward_parameter<Vs>(vs)...);
  }

  template <std::size_t Idx, typename... Vs>
  void deliver_value(
      std::integral_constant<std::size_t, Idx>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    std::visit(
        overload(
            [](std::monostate) noexcept { std::unreachable(); },
            [&]<typename... NewVs>(
                std::tuple<value_tag, NewVs...>& new_v) noexcept {
              std::apply(
                  [&](value_tag, NewVs&... new_vs) noexcept {
                    deliver_value(
                        std::integral_constant<std::size_t, Idx + 1>{},
                        squiz::value<Vs..., NewVs...>,
                        squiz::forward_parameter<Vs>(vs)...,
                        squiz::forward_parameter<NewVs>(new_vs)...);
                  },
                  new_v);
            }),
        std::get<Idx>(value_results_));
  }

  using error_or_stopped_sigs_t = merge_completion_signatures_t<
      error_or_stopped_signatures_t<detail::add_error_if_move_can_throw_t<
          completion_signatures_for_t<ChildSenders, env_t>>>...>;

  using error_or_stopped_result_t =
      detail::completion_signatures_to_variant_of_tuple_t<
          error_or_stopped_sigs_t>;

  using value_results_t =
      std::tuple<detail::completion_signatures_to_variant_of_tuple_t<
          value_signatures_t<completion_signatures_for_t<
              ChildSenders,
              receiver_env_t<Receiver>>>>...>;

  static constexpr std::uint8_t started_flag = 1;
  static constexpr std::uint8_t failed_flag = 2;
  static constexpr std::uint8_t stop_requested_flag = 4;
  static constexpr std::uint8_t count_increment = 8;

  using state_t = detail::smallest_unsigned_integer_for<
      started_flag + failed_flag + stop_requested_flag +
      (count_increment * sizeof...(ChildSenders))>;

  std::atomic<state_t> state_{count_increment * sizeof...(ChildSenders)};
  [[no_unique_address]] error_or_stopped_result_t error_or_stopped_;
  [[no_unique_address]] value_results_t value_results_;
};

/// Specialization for the case where none of the child operations can
/// fail with either stopped or error and thus we can use a more efficient
/// implementation that:
/// - Doesn't need to store an error or stopped result - only value results.
/// - Doesn't need to worry about modifying the environment to ensure that
///   the child operation has a request_stop() if supported, even if the
///   parent environment doesn't indicate a desire to send a stop-request.
template <typename Receiver, std::size_t... Ids, typename... ChildSenders>
  requires(when_all_detail::when_all_child_always_succeeds<
               ChildSenders,
               receiver_env_t<Receiver>> &&
           ...)
struct when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>
  : public inlinable_operation_state<
        when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>,
        Receiver>
  , public child_operation<
        when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>,
        receiver_env_t<Receiver>,
        indexed_source_tag<Ids>,
        ChildSenders>... {
  using inlinable_base_t = inlinable_operation_state<when_all_op, Receiver>;
  template <std::size_t Idx>
  using child_base_t = child_operation<
      when_all_op,
      receiver_env_t<Receiver>,
      indexed_source_tag<Idx>,
      ChildSenders...[Idx]>;

public:
  explicit when_all_op(Receiver rcvr, ChildSenders&&... children)
    : inlinable_base_t(std::move(rcvr))
    , child_base_t<Ids>(std::forward<ChildSenders>(children))... {}

  void start() noexcept {
    // Start child operations
    (child_base_t<Ids>::start(), ...);
  }

  void request_stop() noexcept
    requires is_stop_possible_v<receiver_env_t<Receiver>> &&
      (child_base_t<Ids>::is_stoppable || ...)
  {
    (child_base_t<Ids>::request_stop(), ...);
  }

  template <std::size_t Idx, typename... Vs>
  void set_result(
      indexed_source_tag<Idx>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    std::get<Idx>(value_results_)
        .template emplace<std::tuple<value_tag, Vs...>>(
            value_tag{}, squiz::forward_parameter<Vs>(vs)...);
    arrive();
  }

  template <std::size_t Idx>
  receiver_env_t<Receiver> get_env(indexed_source_tag<Idx>) noexcept {
    return this->get_receiver().get_env();
  }

private:
  void arrive() noexcept {
    if (state_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      deliver_value(std::integral_constant<std::size_t, 0>{}, squiz::value<>);
    }
  }

  template <typename... Vs>
  void deliver_value(
      std::integral_constant<std::size_t, sizeof...(ChildSenders)>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    squiz::set_value<Vs...>(
        this->get_receiver(), squiz::forward_parameter<Vs>(vs)...);
  }

  template <std::size_t Idx, typename... Vs>
  void deliver_value(
      std::integral_constant<std::size_t, Idx>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    std::visit(
        overload(
            [](std::monostate) noexcept { std::unreachable(); },
            [&]<typename... NewVs>(
                std::tuple<value_tag, NewVs...>& new_v) noexcept {
              std::apply(
                  [&](value_tag, NewVs&... new_vs) noexcept {
                    deliver_value(
                        std::integral_constant<std::size_t, Idx + 1>{},
                        squiz::value<Vs..., NewVs...>,
                        squiz::forward_parameter<Vs>(vs)...,
                        squiz::forward_parameter<NewVs>(new_vs)...);
                  },
                  new_v);
            }),
        std::get<Idx>(value_results_));
  }

  using value_results_t =
      std::tuple<detail::completion_signatures_to_variant_of_tuple_t<
          value_signatures_t<completion_signatures_for_t<
              ChildSenders,
              receiver_env_t<Receiver>>>>...>;

  using ref_count_t =
      detail::smallest_unsigned_integer_for<sizeof...(ChildSenders)>;
  std::atomic<ref_count_t> state_{sizeof...(ChildSenders)};
  [[no_unique_address]] value_results_t value_results_;
};

template <typename... ChildSenders>
struct when_all_sender {
  template <typename Self, typename... Env>
    requires(when_all_detail::when_all_child_always_succeeds<
                 detail::member_type_t<Self, ChildSenders>,
                 Env...> &&
             ...)
  auto get_completion_signatures(this Self&&, Env...)
      -> when_all_detail::when_all_combine_value_sigs_t<
          value_signatures_t<completion_signatures_for_t<
              detail::member_type_t<Self, ChildSenders>,
              Env...>>...>;

  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> merge_completion_signatures_t<
          when_all_detail::when_all_combine_value_sigs_t<
              value_signatures_t<completion_signatures_for_t<
                  detail::member_type_t<Self, ChildSenders>,
                  detail::env_with_stop_possible<Env>...>>...>,
          error_or_stopped_signatures_t<completion_signatures_for_t<
              detail::member_type_t<Self, ChildSenders>,
              detail::make_env_with_stop_possible_t<Env>...>>...>;

  template <typename Self, typename... Env>
    requires(when_all_detail::when_all_child_always_succeeds<
                 detail::member_type_t<Self, ChildSenders>,
                 Env...> &&
             ...)
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<
          (squiz::is_always_nothrow_connectable_v<
               detail::member_type_t<Self, ChildSenders>,
               Env...> &&
           ...)>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<
          (squiz::is_always_nothrow_connectable_v<
               detail::member_type_t<Self, ChildSenders>,
               detail::make_env_with_stop_possible_t<Env>...> &&
           ...)>;

  template <typename... ChildSenders2>
    requires(sizeof...(ChildSenders) == sizeof...(ChildSenders2)) &&
      ((sizeof...(ChildSenders) != 1) ||
       (!std::same_as<
           std::remove_cvref_t<ChildSenders2...[0]>,
           when_all_sender>)) &&
      (std::constructible_from<ChildSenders, ChildSenders2> && ...)
  when_all_sender(ChildSenders2&&... cs) noexcept(
      (std::is_nothrow_constructible_v<ChildSenders, ChildSenders2> && ...))
    : children(std::forward<ChildSenders2>(cs)...) {}

  template <typename Self, typename Receiver>
  auto connect(this Self&& self, Receiver rcvr) noexcept(
      squiz::is_always_nothrow_connectable_v<Self, receiver_env_t<Receiver>>) {
    return std::apply(
        [&](auto&&... cs) {
          return when_all_op<
              Receiver,
              std::index_sequence_for<ChildSenders...>,
              detail::member_type_t<Self, ChildSenders>...>(
              std::move(rcvr), static_cast<decltype(cs)>(cs)...);
        },
        std::forward<Self>(self).children);
  }

private:
  [[no_unique_address]] std::tuple<ChildSenders...> children;
};

template <typename... ChildSenders>
  requires(sizeof...(ChildSenders) >= 2)  //
auto when_all(ChildSenders&&... cs) noexcept(
    (nothrow_decay_copyable<ChildSenders> && ...)) {
  return when_all_sender<std::remove_cvref_t<ChildSenders>...>(
      std::forward<ChildSenders>(cs)...);
}

/// when_all() of a single sender is the same as just returning that sender.
template <typename ChildSender>
std::remove_cvref_t<ChildSender>
when_all(ChildSender&& child) noexcept(nothrow_decay_copyable<ChildSender>) {
  return std::forward<ChildSender>(child);
}

/// when_all() of zero senders synchronously completes with set_value_t().
inline just_value_sender<> when_all() noexcept {
  return just_value_sender<>{};
}

}  // namespace squiz
