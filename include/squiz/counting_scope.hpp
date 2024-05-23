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
#include <type_traits>
#include <utility>

#include <squiz/concepts.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/parameter_type.hpp>
#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/stop_possible.hpp>
#include <squiz/variant_child_operation.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/deliver_result.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/store_result.hpp>

namespace squiz {
class counting_scope_token;

template <typename Source, typename Receiver>
class counting_scope_nest_op;

template <typename Source>
class counting_scope_nest_sender;

class counting_scope {
  struct join_op_base {
    void (*execute)(join_op_base*) noexcept;
  };

  template <typename Receiver>
  class join_op
    : public inlinable_operation_state<join_op<Receiver>, Receiver>
    , private join_op_base {
  private:
    using inlinable_base = inlinable_operation_state<join_op, Receiver>;

  public:
    join_op(counting_scope& scope, Receiver r) noexcept
      : inlinable_base(std::move(r))
      , scope_(scope) {
      this->execute = &execute_impl;
    }

    void start() noexcept { scope_.start_join(this); }

  private:
    static void execute_impl(join_op_base* op) noexcept {
      auto& self = *static_cast<join_op*>(op);
      squiz::set_value<>(self.get_receiver());
    }

    counting_scope& scope_;
  };

  class join_sender {
  public:
    auto get_completion_signatures() && -> completion_signatures<value_t<>>;
    static auto is_always_nothrow_connectable() -> std::true_type;

    join_sender(join_sender&&) = default;
    join_sender(const join_sender&) = delete;

    template <typename Receiver>
    join_op<Receiver> connect(Receiver r) && noexcept {
      return join_op<Receiver>{scope_, std::move(r)};
    }

  private:
    friend counting_scope;

    explicit join_sender(counting_scope& scope) noexcept : scope_(scope) {}

    counting_scope& scope_;
  };

public:
  counting_scope() noexcept = default;

  ~counting_scope() { assert(is_in_valid_final_state()); }

  counting_scope(const counting_scope&) = delete;
  counting_scope(counting_scope&&) = delete;

  counting_scope_token get_token() noexcept;

  void close() noexcept {
    ref_count_and_closed_flag_.fetch_or(closed_flag, std::memory_order_relaxed);
  }

  join_sender join() noexcept { return join_sender{*this}; }

private:
  bool is_in_valid_final_state() noexcept {
    auto state = ref_count_and_closed_flag_.load(std::memory_order_relaxed);
    auto ref_count = state / ref_count_increment;
    bool join_has_run = (join_op_ != nullptr);

    return (ref_count == 0 && join_has_run) ||
        (ref_count == 1 && !join_has_run);
  }

  friend class counting_scope_token;

  template <typename Source, typename Receiver>
  friend class counting_scope_nest_op;

  template <typename Source>
  friend class counting_scope_nest_sender;

  struct ref_handle {
  public:
    ref_handle(ref_handle&& other) noexcept
      : scope_(std::exchange(other.scope_, nullptr)) {}

    ref_handle(const ref_handle& other) noexcept : scope_(other.scope_) {
      if (valid()) {
        scope_->increment_ref_count();
      }
    }

    ~ref_handle() { reset(); }

    bool valid() const noexcept { return scope_ != nullptr; }

    void reset() noexcept {
      if (valid()) {
        std::exchange(scope_, nullptr)->decrement_ref_count();
      }
    }

  private:
    friend counting_scope;
    explicit ref_handle(counting_scope* scope) noexcept : scope_(scope) {}
    counting_scope* scope_;
  };

  void start_join(join_op_base* op) noexcept {
    assert(join_op_ == nullptr);
    join_op_ = op;

    decrement_ref_count();
  }

  ref_handle try_get_ref_handle() noexcept {
    std::size_t old_state =
        ref_count_and_closed_flag_.load(std::memory_order_relaxed);
    do {
      if ((old_state & closed_flag) != 0) {
        return ref_handle{nullptr};
      }
      // Invalid to try to increment the ref-count once join() has already
      // finished if you haven't previously called close().
      assert(old_state >= ref_count_increment);
    } while (!ref_count_and_closed_flag_.compare_exchange_weak(
        old_state, old_state + ref_count_increment, std::memory_order_relaxed));

    return ref_handle{this};
  }

  void increment_ref_count() noexcept {
    [[maybe_unused]] auto old_state = ref_count_and_closed_flag_.fetch_add(
        ref_count_increment, std::memory_order_relaxed);
    assert(old_state >= ref_count_increment);
  }

  void decrement_ref_count() noexcept {
    auto old_state = ref_count_and_closed_flag_.fetch_sub(
        ref_count_increment, std::memory_order_acq_rel);
    assert(old_state >= ref_count_increment);

    if (old_state < (2 * ref_count_increment)) {
      // There weren't any outstanding operations - complete synchronously.
      join_op_->execute(join_op_);
    }
  }

  static constexpr std::size_t ref_count_increment = 2;
  static constexpr std::size_t closed_flag = 1;

  std::atomic<std::size_t> ref_count_and_closed_flag_{ref_count_increment};
  join_op_base* join_op_{nullptr};
};

template <typename Source, typename Receiver>
class counting_scope_nest_op
  : public inlinable_operation_state<
        counting_scope_nest_op<Source, Receiver>,
        Receiver>
  , public variant_child_operation<
        counting_scope_nest_op<Source, Receiver>,
        receiver_env_t<Receiver>,
        indexed_source_tag,
        Source> {
  using inlinable_base =
      inlinable_operation_state<counting_scope_nest_op, Receiver>;
  using child_base = variant_child_operation<
      counting_scope_nest_op,
      receiver_env_t<Receiver>,
      indexed_source_tag,
      Source>;

  static constexpr bool is_stop_possible =
      is_stop_possible_v<receiver_env_t<Receiver>> &&
      child_base::template is_stoppable<0>;

public:
  counting_scope_nest_op(
      counting_scope::ref_handle scope_handle,
      Source&& source,
      Receiver r) noexcept(child_base::template is_nothrow_constructible<0>)
    : inlinable_base(std::move(r))
    , scope_(std::move(scope_handle)) {
    if (scope_.valid()) {
      child_base::template construct<0>(std::forward<Source>(source));
    }
  }

  ~counting_scope_nest_op() {
    if (scope_.valid()) {
      destroy_and_decref();
    }
  }

  void start() noexcept {
    if (scope_.valid()) {
      child_base::template start<0>();
    } else {
      if constexpr (is_stop_possible) {
        state_.flags.store(completed_flag, std::memory_order_relaxed);
      }
      squiz::set_stopped(this->get_receiver());
    }
  }

  void request_stop() noexcept
    requires is_stop_possible
  {
    std::uint8_t old_state = 0;
    if (state_.flags.compare_exchange_strong(
            old_state, stop_request_active_flag, std::memory_order_relaxed)) {
      // Won the race with set_result().
      // Safe to call request_stop() on the child (if there is a child).
      assert(scope_.valid());

      child_base::template request_stop<0>();

      old_state = state_.flags.fetch_sub(
          stop_request_active_flag, std::memory_order_acq_rel);
      if ((old_state & completed_flag) != 0) {
        // set_result() has delegated completion to us.
        destroy_and_decref();
        detail::deliver_result(this->get_receiver(), state_.result);
      }
    }
  }

  template <typename Tag, typename... Datums>
  void set_result(
      indexed_source_tag<0>,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    if constexpr (is_stop_possible) {
      // We might be racing with a call to request_stop(), in which case
      // we cannot destroy the child operation-state immediately.
      // Instead, we need to store the result and then set a flag, indicating
      // that the operation is complete so that the request_stop() call can
      // then deliver the result when we complete here.
      std::uint8_t old_flags = 0;
      if (state_.flags.compare_exchange_strong(
              old_flags, completed_flag, std::memory_order_relaxed)) {
        // We successfully won the race with a potential call to request_stop()
        // Any subsequent call to request_stop() will not access the child op
        // so we are free to destroy it and just forward through the results.
        destroy_and_decref();

        squiz::set_result(
            this->get_receiver(),
            sig,
            squiz::forward_parameter<Datums>(datums)...);
      } else {
        // Otherwise, there is a race with a call to request_stop().
        // We need to store the results in the op-state and then set a flag.
        // If the request_stop() has finished then we can deliver the result
        // here, otherwise we defer to request_stop() when it returns from
        // requesting stop on the child to deliver the result.

        detail::store_result(
            state_.result, sig, squiz::forward_parameter<Datums>(datums)...);

        old_flags =
            state_.flags.fetch_add(completed_flag, std::memory_order_acq_rel);

        if ((old_flags & stop_request_active_flag) == 0) {
          destroy_and_decref();
          detail::deliver_result(this->get_receiver(), state_.result);
        }
      }
    } else {
      try {
        // Copy the  values locally in case we were passed references to objects
        // owned by the child operation-state we are about to destroy.
        std::tuple<Datums...> datums_copy{std::forward<Datums>(datums)...};
        destroy_and_decref();
        std::apply(
            [&](Datums&... datums) noexcept {
              squiz::set_result(
                  this->get_receiver(),
                  sig,
                  squiz::forward_parameter<Datums>(datums)...);
            },
            datums_copy);
      } catch (...) {
        constexpr bool is_nothrow =
            (std::is_nothrow_move_constructible_v<Datums> && ...);
        if constexpr (!is_nothrow) {
          destroy_and_decref();
          squiz::set_error<std::exception_ptr>(
              this->get_receiver(), std::current_exception());
        } else {
          std::unreachable();
        }
      }
    }
  }

private:
  using result_variant_t = detail::completion_signatures_to_variant_of_tuple_t<
      detail::add_error_if_move_can_throw_t<
          completion_signatures_for_t<Source, receiver_env_t<Receiver>>>>;

  void destroy_and_decref() noexcept {
    assert(scope_.valid());
    child_base::template destruct<0>();
    scope_.reset();
  }

  struct empty_state {};

  struct stoppable_state {
    result_variant_t result;
    std::atomic<std::uint8_t> flags;
  };

  using state_t =
      std::conditional_t<is_stop_possible, stoppable_state, empty_state>;

  static constexpr std::uint8_t completed_flag = 1;
  static constexpr std::uint8_t stop_request_active_flag = 2;

  counting_scope::ref_handle scope_;
  [[no_unique_address]] state_t state_;
};

template <typename Source>
class counting_scope_nest_sender {
public:
  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> merge_completion_signatures_t<
          detail::add_error_if_move_can_throw_t<completion_signatures_for_t<
              detail::member_type_t<Self, Source>,
              Env...>>,
          completion_signatures<stopped_t>>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<is_always_nothrow_connectable_v<
          detail::member_type_t<Self, Source>,
          Env...>>;

  counting_scope_nest_sender(counting_scope_nest_sender&& other) = default;

  counting_scope_nest_sender(const counting_scope_nest_sender& other) = default;

  ~counting_scope_nest_sender() = default;

  template <typename Self, typename Receiver>
  counting_scope_nest_op<detail::member_type_t<Self, Source>, Receiver>
  connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<
          counting_scope_nest_op<detail::member_type_t<Self, Source>, Receiver>,
          detail::member_type_t<Self, counting_scope::ref_handle>,
          detail::member_type_t<Self, Source>,
          Receiver>) {
    return counting_scope_nest_op<
        detail::member_type_t<Self, Source>,
        Receiver>{
        std::forward<Self>(self).scope_,
        std::forward<Self>(self).source_,
        std::move(r)};
  }

private:
  friend counting_scope_token;

  template <typename Source2>
    requires std::constructible_from<Source, Source2>
  explicit counting_scope_nest_sender(
      counting_scope::ref_handle scope,
      Source2&&
          source) noexcept(std::is_nothrow_constructible_v<Source, Source2>)
    : scope_(std::move(scope))
    , source_(std::forward<Source2>(source)) {}

  counting_scope::ref_handle scope_;
  Source source_;
};

class counting_scope_token {
public:
  template <sender Source>
    requires decay_copyable<Source>
  counting_scope_nest_sender<std::remove_cvref_t<Source>>
  nest(Source&& source) const noexcept(nothrow_decay_copyable<Source>) {
    return counting_scope_nest_sender<std::remove_cvref_t<Source>>{
        scope_->try_get_ref_handle(), std::forward<Source>(source)};
  }

private:
  friend counting_scope;

  explicit counting_scope_token(counting_scope& scope) noexcept
    : scope_(&scope) {}

  counting_scope* scope_;
};

inline counting_scope_token counting_scope::get_token() noexcept {
  return counting_scope_token(*this);
}

}  // namespace squiz
