///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <exception>
#include <functional>

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/overload.hpp>
#include <squiz/receiver.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/variant_child_operation.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/dispatch_index.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/smallest_unsigned_integer.hpp>

namespace squiz {

template <typename Source, typename BodyFactory, typename Receiver>
class let_value_op;

namespace let_value_detail {

// let_value_transform_t<ValueSig, BodyFactory> -> BodySender

template <typename Sig, typename BodyFactory>
struct let_value_transform {};

template <typename... Vs, typename BodyFactory>
  requires std::invocable<BodyFactory, Vs&...>
struct let_value_transform<value_t<Vs...>, BodyFactory> {
  using type = std::invoke_result_t<BodyFactory, Vs&...>;
};

template <typename Sig, typename BodyFactory>
using let_value_transform_t =
    typename let_value_transform<Sig, BodyFactory>::type;

// let_value_child_op<Source, BodyFactory, Receiver>
// -> variant_child_operation for each of the child operation-states.

template <typename Source, typename BodyFactory, typename Receiver>
struct make_let_value_variant_child_operation {
  template <typename... Sigs>
  using apply = variant_child_operation<
      let_value_op<Source, BodyFactory, Receiver>,
      receiver_env_t<Receiver>,
      indexed_source_tag,
      Source,
      let_value_transform_t<Sigs, BodyFactory>...>;
};

template <typename Source, typename BodyFactory, typename Receiver>
using let_value_child_op = value_signatures_t<
    completion_signatures_for_t<Source, receiver_env_t<Receiver>>>::
    template apply<
        make_let_value_variant_child_operation<Source, BodyFactory, Receiver>::
            template apply>;

// transform_body_signatures
// - Can be passed to transform_completion_signatures_t to transform each of
//   the completions of a body sender to the set of completions of the
//   let_value operation. This basically just involves detecting whether any
//   of the types are potentially throwing if we need to store the body result
//   while we wait for a delegated concurrent stop-request to finish executing.
struct transform_body_signatures {
  template <typename Tag, typename... Datums>
    requires(std::is_nothrow_move_constructible_v<Datums> && ...)
  static auto apply(result_t<Tag, Datums...>)
      -> completion_signatures<result_t<Tag, Datums...>>;

  template <typename Tag, typename... Datums>
  static auto apply(result_t<Tag, Datums...>) -> completion_signatures<
                                                  result_t<Tag, Datums...>,
                                                  error_t<std::exception_ptr>>;
};

template <typename BodyFactory, typename... Env>
struct transform_source_signatures {
private:
  template <typename... Vs>
  static constexpr bool is_body_sender_nothrow_startable =
      (std::is_nothrow_move_constructible_v<Vs> && ...) &&
      std::is_nothrow_invocable_v<BodyFactory, Vs&...> &&
      squiz::is_always_nothrow_connectable_v<
          std::invoke_result_t<BodyFactory, Vs&...>,
          Env...>;

  template <typename... Vs>
  using body_completion_sigs_t = merge_completion_signatures_t<
      detail::add_error_if_move_can_throw_t<completion_signatures_for_t<
          std::invoke_result_t<BodyFactory, Vs&...>,
          Env...>>,
      std::conditional_t<
          is_body_sender_nothrow_startable<Vs...>,
          completion_signatures<>,
          completion_signatures<error_t<std::exception_ptr>>>>;

public:
  template <typename... Vs>
    requires std::invocable<BodyFactory, Vs&...>
  static auto apply(value_t<Vs...>) -> body_completion_sigs_t<Vs...>;

  template <typename Tag, typename... Datums>
  static auto apply(result_t<Tag, Datums...>)
      -> completion_signatures<result_t<Tag, Datums...>>;
};

template <typename Source, typename BodyFactory, typename... Env>
using let_value_completion_signatures = merge_completion_signatures_t<
    completion_signatures<stopped_t>,
    error_or_stopped_signatures_t<completion_signatures_for_t<Source, Env...>>,
    transform_completion_signatures_t<
        value_signatures_t<completion_signatures_for_t<Source, Env...>>,
        transform_source_signatures<BodyFactory, Env...>>>;

}  // namespace let_value_detail

template <typename Source, typename BodyFactory, typename Receiver>
class let_value_op final
  : public inlinable_operation_state<
        let_value_op<Source, BodyFactory, Receiver>,
        Receiver>
  , public let_value_detail::let_value_child_op<Source, BodyFactory, Receiver> {
private:
  using inlinable_base = inlinable_operation_state<let_value_op, Receiver>;
  using child_base =
      let_value_detail::let_value_child_op<Source, BodyFactory, Receiver>;

  using state_t = std::uint8_t;

  using child_index_t =
      detail::smallest_unsigned_integer_for<child_base::sender_count + 1>;

  static constexpr child_index_t empty_child_index =
      std::numeric_limits<child_index_t>::max();

public:
  template <typename BodyFactory2>
  let_value_op(Source&& source, BodyFactory2&& factory, Receiver receiver) noexcept(
      child_base::template is_nothrow_constructible<0> &&
      std::is_nothrow_constructible_v<BodyFactory, BodyFactory2>)
    : inlinable_base(std::move(receiver))
    , factory_(std::forward<BodyFactory2>(factory)) {
    child_base::template construct<0>(std::forward<Source>(source));
  }

  ~let_value_op() {
    if (child_index_ != empty_child_index) {
      child_base::destruct(child_index_);
    }
  }

  void start() noexcept { child_base::template start<0>(); }

  void request_stop() noexcept
    requires child_base::is_any_stoppable
  {
    [[maybe_unused]] constexpr state_t stop_request_flags =
        (stop_requested_flag | stop_request_done_flag);

    state_t old_state = state_.load(std::memory_order_acquire);
    while (true) {
      assert((old_state & stop_request_flags) == 0);
      if ((old_state & body_done_flag) != 0) {
        return;
      } else if ((old_state & body_started_flag) != 0) {
        // The body operation has started and not yet finished.
        // Send it a stop-request.
        child_base::request_stop(child_index_);
        return;
      } else if ((old_state & src_done_flag) != 0) {
        // Source operation has finished and either set_value() is in
        // middle of constructing the body operation, or the operation
        // as whole has finished with the set_error/set_stopped result
        // of the source operation.
        // In case it is the set_value() case, we try to set the
        // 'stop_requested_flag' and if successful then we know that set_value()
        // will call request_stop() on the child once it has finished calling
        // start() on the body operation.
        if (state_.compare_exchange_weak(
                old_state,
                src_done_flag | stop_requested_flag,
                std::memory_order_relaxed,
                std::memory_order_acquire)) {
          // Successfully deferred call to child_base::request_stop() to be
          // done by set_value() if needed once it finishes constructing the
          // child operation state and starting it.
          return;
        }
      } else {
        assert(old_state == 0);
        // If successful, don't need the 'acquire' semantics as we have already
        // done the 'acquire' on the previous load.
        // If the source operation later completes with set_value() it will
        // complete the whole operation with set_stopped() and won't destroy
        // the operation-state. The receiver's set_stopped() implementation
        // will have to wait until this call to request_stop() returns before
        // it can destroy the operation-state.
        if (state_.compare_exchange_weak(
                old_state,
                stop_requested_flag,
                std::memory_order_relaxed,
                std::memory_order_acquire)) {
          child_base::template request_stop<0>();
          return;
        }
      }
    }
  }

  template <typename... Vs>
  void set_result(
      indexed_source_tag<0>,
      value_t<Vs...>,
      parameter_type<Vs>... vs) noexcept {
    using next_sender_t = std::invoke_result_t<BodyFactory, Vs&...>;
    constexpr std::size_t sender_index =
        child_base::template sender_index<next_sender_t>;
    constexpr bool is_nothrow =
        (std::is_nothrow_move_constructible_v<Vs> && ...) &&
        std::is_nothrow_invocable_v<BodyFactory, Vs&...> &&
        child_base::template is_nothrow_constructible<sender_index>;

    // The fetch_add(stop_request_done_flag, release) in request_stop()
    // synchronises with this if the stop_request_done_flag is set.
    state_t old_state =
        state_.fetch_add(src_done_flag, std::memory_order_relaxed);
    assert((old_state & stage_mask) == 0);

    if ((old_state & stop_requested_flag) != 0) {
      // stop was requested - discard the result and complete with
      // set_stopped().
      squiz::set_stopped(this->get_receiver());
      return;
    }

    try {
      auto& value_tuple =
          value_storage_.template emplace<std::tuple<value_tag, Vs...>>(
              value_tag{}, squiz::forward_parameter<Vs>(vs)...);

      child_base::template destruct<0>();
      child_index_ = empty_child_index;

      std::apply(
          [&](value_tag, Vs&... stored_vs) {
            child_base::template construct<sender_index>(
                std::invoke(std::move(factory_), stored_vs...));
            child_index_ = sender_index;
          },
          value_tuple);

      child_base::template start<sender_index>();
    } catch (...) {
      // Only instantiate the call to set_error() if it is potentially-throwing.
      if constexpr (!is_nothrow) {
        // Make sure the value is destroyed first.
        value_storage_.template emplace<0>();
        squiz::set_error<std::exception_ptr>(
            this->get_receiver(), std::current_exception());
        return;
      }
      assert(false);
      std::unreachable();
    }

    // Mark the body operation as started.
    old_state = state_.fetch_add(body_started_flag, std::memory_order_release);

    // Check if a stop-request came in while we were starting the body
    if (old_state == (src_done_flag | stop_requested_flag)) {
      // Stop request pending and body operation not yet completed.
      // Send the body operation a stop-request.
      child_base::template request_stop<sender_index>();

      // Need to mark the stop-request as done as we are potentially
      // running it on some thread other than the one that called request_stop()
      // on this operation state.
      old_state =
          state_.fetch_add(stop_request_done_flag, std::memory_order_acq_rel);
    }

    if ((old_state & body_done_flag) != 0) {
      // The body operation has completed but was unable to call the completion
      // handler because we were still accessing the operation-state and so the
      // completion has stored the result and delegated the invocation of the
      // completion-handler to us.
      dispatch_result();
    }
  }

  template <one_of<stopped_tag, error_tag> Tag, typename... Datums>
  void set_result(
      indexed_source_tag<0>,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    squiz::set_result(
        this->get_receiver(), sig, squiz::forward_parameter<Datums>(datums)...);
  }

  //
  // Body operation completion
  //

private:
  /// Query if it is safe for the handler for a body operation completion signal
  /// to complete inline within that handler after setting the 'body_done_flag'.
  ///
  /// \param old_state
  /// The state immediately before the body operation completed.
  static bool can_complete_inline(state_t old_state) noexcept {
    constexpr state_t body_started_without_pending_stop_request =
        (src_done_flag | body_started_flag);
    constexpr state_t body_started_with_done_pending_stop_request =
        (src_done_flag | body_started_flag | stop_requested_flag |
         stop_request_done_flag);
    return old_state == body_started_without_pending_stop_request ||
        old_state == body_started_with_done_pending_stop_request;
  }

  // Send the result
  void dispatch_result() noexcept {
    std::visit(
        squiz::overload(
            [](std::monostate) noexcept {
              assert(false);
              std::unreachable();
            },
            [&]<typename Tag, typename... Datums>(
                std::tuple<Tag, Datums...>& result) noexcept {
              std::apply(
                  [&](Tag, Datums&... datums) noexcept {
                    squiz::set_result(
                        this->get_receiver(),
                        squiz::result<Tag, Datums...>,
                        squiz::forward_parameter<Datums>(datums)...);
                  },
                  result);
            }),
        result_storage_);
  }

public:
  template <std::size_t Idx, typename Tag, typename... Datums>
    requires(Idx != 0)
  void set_result(
      indexed_source_tag<Idx>,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    state_t old_state = state_.load(std::memory_order_acquire);

    if (can_complete_inline(old_state)) {
      // operation started without a pending stop-request.
      // any subsequent stop-request will come from the stop_request()
      // method on this operation state, which is guaranteed not to
      // race with destruction of this operation state by the caller.
      squiz::set_result(
          this->get_receiver(),
          sig,
          squiz::forward_parameter<Datums>(datums)...);
      return;
    }

    // Otherwise, the set_value() call above the starts the body operation
    // potentially still accessing the operation state. So we need to store
    // the result and signal that the body operation has completed and then
    // see who won the race and should deliver the result.
    try {
      auto& result =
          result_storage_.template emplace<std::tuple<Tag, Datums...>>(
              Tag{}, squiz::forward_parameter<Datums>(datums)...);

      old_state = state_.fetch_add(body_done_flag, std::memory_order_acq_rel);
      if (can_complete_inline(old_state)) {
        std::apply(
            [&](Tag, Datums&... tuple_datums) noexcept {
              squiz::set_result(
                  this->get_receiver(),
                  sig,
                  squiz::forward_parameter<Datums>(tuple_datums)...);
              return;
            },
            result);
      }
    } catch (...) {
      const bool is_nothrow =
          (std::is_nothrow_move_constructible_v<Datums> && ...);
      if constexpr (!is_nothrow) {
        auto& err =
            result_storage_
                .template emplace<std::tuple<error_tag, std::exception_ptr>>(
                    error_tag{}, std::current_exception());

        old_state = state_.fetch_add(body_done_flag, std::memory_order_acq_rel);
        if (can_complete_inline(old_state)) {
          squiz::set_error<std::exception_ptr>(
              this->get_receiver(), std::move(std::get<1>(err)));
        }
      } else {
        assert(false);
        std::unreachable();
      }
    }
  }

  template <std::size_t Idx>
  receiver_env_t<Receiver> get_env(indexed_source_tag<Idx>) noexcept {
    return this->get_receiver().get_env();
  }

private:
  using source_completion_sigs_t =
      completion_signatures_for_t<Source, receiver_env_t<Receiver>>;
  using source_value_sigs_t = value_signatures_t<source_completion_sigs_t>;
  using body_completion_sigs_t = transform_completion_signatures_t<
      source_value_sigs_t,
      let_value_detail::
          transform_source_signatures<BodyFactory, receiver_env_t<Receiver>>>;

  using value_variant_t =
      detail::completion_signatures_to_variant_of_tuple_t<source_value_sigs_t>;

  using result_variant_t = detail::completion_signatures_to_variant_of_tuple_t<
      body_completion_sigs_t>;

  // This flag indicates that there is an active operation-state that
  // is constructed that can receive a stop-request.
  static constexpr state_t src_done_flag = 1;
  static constexpr state_t body_started_flag = 2;
  static constexpr state_t body_done_flag = 4;
  static constexpr state_t stage_mask =
      src_done_flag | body_started_flag | body_done_flag;
  static constexpr state_t stop_requested_flag = 8;
  static constexpr state_t stop_request_done_flag = 16;

  std::atomic<state_t> state_{0};
  child_index_t child_index_{0};
  [[no_unique_address]] BodyFactory factory_;
  [[no_unique_address]] value_variant_t value_storage_;
  [[no_unique_address]] result_variant_t result_storage_;
};

template <typename Source, typename BodyFactory>
struct let_value_sender {
  Source source;
  BodyFactory body_factory;

  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> let_value_detail::
          let_value_completion_signatures<Source, BodyFactory, Env...>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<
          (squiz::is_always_nothrow_connectable_v<
               detail::member_type_t<Self, Source>,
               Env...> &&
           std::is_nothrow_constructible_v<
               BodyFactory,
               detail::member_type_t<Self, BodyFactory>>)>;

  template <typename Self, typename Receiver>
  let_value_op<detail::member_type_t<Self, Source>, BodyFactory, Receiver>
  connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<
          let_value_op<
              detail::member_type_t<Self, Source>,
              BodyFactory,
              Receiver>,
          detail::member_type_t<Self, Source>,
          detail::member_type_t<Self, BodyFactory>,
          Receiver>) {
    return let_value_op<
        detail::member_type_t<Self, Source>,
        BodyFactory,
        Receiver>{
        std::forward<Self>(self).source,
        std::forward<Self>(self).body_factory,
        std::move(r)};
  }
};

template <typename Source, typename BodyFactory>
let_value_sender(Source, BodyFactory) -> let_value_sender<Source, BodyFactory>;

template <typename Source, typename BodyFactory>
using let_value = let_value_sender<Source, BodyFactory>;

}  // namespace squiz
