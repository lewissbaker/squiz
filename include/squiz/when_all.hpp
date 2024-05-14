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
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/smallest_unsigned_integer.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/just.hpp>
#include <squiz/overload.hpp>
#include <squiz/source_tag.hpp>

namespace squiz {

template <typename Receiver, typename Ids, typename... ChildSenders>
struct when_all_op;

template <typename Receiver, std::size_t... Ids, typename... ChildSenders>
struct when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>
  : inlinable_operation_state<
        when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>,
        Receiver>
  , child_operation<
        when_all_op<Receiver, std::index_sequence<Ids...>, ChildSenders...>,
        indexed_source_tag<Ids>,
        ChildSenders>... {
  using inlinable_base_t = inlinable_operation_state<when_all_op, Receiver>;

  template <std::size_t Idx>
  using child_base_t = child_operation<
      when_all_op,
      indexed_source_tag<Idx>,
      ChildSenders...[Idx]>;

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
      requires(child_base_t<Ids>::is_stoppable || ...) {
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
  void set_value(indexed_source_tag<Idx>, Vs&&... vs) noexcept {
    // TODO: handle exceptions that might be thrown when copying/moving values
    std::get<Idx>(value_results_)
        .template emplace<std::tuple<set_value_t, Vs...>>(
            set_value_t{}, std::forward<Vs>(vs)...);
    arrive();
  }

  template <std::size_t Idx, typename E>
  void set_error(indexed_source_tag<Idx>, E&& e) noexcept {
    if (try_set_failure_flag()) {
      error_or_stopped_.template emplace<std::tuple<set_error_t, E>>(
          set_error_t{}, std::forward<E>(e));
    }
    arrive();
  }

  template <std::size_t Idx>
  void set_stopped(indexed_source_tag<Idx>) noexcept {
    if (try_set_failure_flag()) {
      error_or_stopped_.template emplace<std::tuple<set_stopped_t>>(
          set_stopped_t{});
    }
    arrive();
  }

  template <std::size_t Idx>
  decltype(auto) get_env(indexed_source_tag<Idx>) noexcept {
    return this->get_receiver().get_env();
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
              [this]<typename E>(std::tuple<set_error_t, E>&& tup) noexcept {
                // Need an extra lambda here so that the lookup of .set_error
                // member is dependent and deferred to second phase of 2-phase
                // lookup as a receiver might not have a set_error() if the
                // outer lambda is never instantiated.
                [&]<typename R, typename E2>(R&& r, E2&& e) noexcept {
                  r.set_error(std::forward<E2>(e));
                }(this->get_receiver(), std::get<1>(std::move(tup)));
              },
              [this]<std::same_as<set_stopped_t> T>(std::tuple<T>) noexcept {
                // Extra lambda to force 2-phase lookup of set_stopped() member.
                [&]<typename R>(R&& r) noexcept {
                  r.set_stopped();
                }(this->get_receiver());
              }),
          std::move(error_or_stopped_));
    } else {
      deliver_value(std::integral_constant<std::size_t, 0>{});
    }
  }

  template <typename... Vs>
  void deliver_value(
      std::integral_constant<std::size_t, sizeof...(ChildSenders)>,
      Vs&&... vs) noexcept {
    this->get_receiver().set_value(std::forward<Vs>(vs)...);
  }

  template <std::size_t Idx, typename... Vs>
  void
  deliver_value(std::integral_constant<std::size_t, Idx>, Vs&&... vs) noexcept {
    std::visit(
        overload(
            [](std::monostate) noexcept { std::unreachable(); },
            [&](auto&& new_v) {
              std::apply(
                  [&](set_value_t, auto&&... new_vs) {
                    deliver_value(
                        std::integral_constant<std::size_t, Idx + 1>{},
                        std::forward<Vs>(vs)...,
                        static_cast<decltype(new_vs)>(new_vs)...);
                  },
                  std::move(new_v));
            }),
        std::get<Idx>(std::move(value_results_)));
  }

  using error_or_stopped_sigs_t = merge_completion_signatures_t<
      error_or_stopped_signatures_t<completion_signatures_for_t<
          ChildSenders,
          receiver_env_t<Receiver>>>...>;

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

namespace when_all_detail {

template <typename ValueSig, typename SigSet>
struct when_all_combine_value_sigs_helper;

template <typename... Args, typename... Sigs>
struct when_all_combine_value_sigs_helper<
    set_value_t(Args...),
    completion_signatures<Sigs...>> {
  template <typename Sig>
  struct append_sig;

  template <typename... Args2>
  struct append_sig<set_value_t(Args2...)> {
    using type = set_value_t(Args..., Args2...);
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

}  // namespace when_all_detail

template <typename... ChildSenders>
struct when_all_sender {
  template <typename Self, typename... Envs>
  auto get_completion_signatures(this Self&&, Envs...)
      -> merge_completion_signatures_t<
          when_all_detail::when_all_combine_value_sigs_t<
              value_signatures_t<completion_signatures_for_t<
                  detail::member_type_t<Self, ChildSenders>,
                  Envs...>>...>,
          error_or_stopped_signatures_t<completion_signatures_for_t<
              detail::member_type_t<Self, ChildSenders>,
              Envs...>>...>;

  template <typename... ChildSenders2>
  requires(sizeof...(ChildSenders) == sizeof...(ChildSenders2)) &&
      ((sizeof...(ChildSenders) != 1) ||
       (!std::same_as<
           std::remove_cvref_t<ChildSenders2...[0]>,
           when_all_sender>)) &&
      (std::constructible_from<ChildSenders, ChildSenders2> && ...)
          when_all_sender(ChildSenders2&&... cs) noexcept(
              (std::is_nothrow_constructible_v<ChildSenders, ChildSenders2> &&
               ...))
    : children(std::forward<ChildSenders2>(cs)...) {}

  template <typename Self, typename Receiver>
  auto connect(this Self&& self, Receiver rcvr) {
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
inline just_sender<> when_all() noexcept {
  return just_sender<>{};
}

}  // namespace squiz
