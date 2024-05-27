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

#include <squiz/child_operation.hpp>
#include <squiz/concepts.hpp>
#include <squiz/empty_env.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/manual_child_operation.hpp>
#include <squiz/parameter_type.hpp>
#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/stop_possible.hpp>
#include <squiz/detail/add_error_if_move_can_throw.hpp>
#include <squiz/detail/allocate_unique.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/deliver_result.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/store_result.hpp>

namespace squiz {
class counting_scope_token;

template <typename Source>
class counting_scope_nest_sender;

template <typename Source, typename Allocator, typename Env>
class counting_scope_future;

class counting_scope_ref;

//
// counting_scope
//

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

  // Not movable or copyable.
  counting_scope(const counting_scope&) = delete;
  counting_scope(counting_scope&&) = delete;

  /// Get a scope-token that can be passed to code that wants to create new work
  /// within this scope.
  counting_scope_token get_token() noexcept;

  /// Close the scope to prevent any new work from being added to the scope.
  ///
  /// After this call, any subsequent attempts to call nest(), spawn() or
  /// spawn_future() will fail to launch the operation and will complete
  /// immediately with a 'stopped' result.
  void close() noexcept {
    ref_count_and_closed_flag_.fetch_or(closed_flag, std::memory_order_relaxed);
  }

  /// Obtain a sender that can be used to join the work added to the scope.
  ///
  /// A scope must be joined before it is destroyed if it has had any work added
  /// to the scope. The join() operation completes once all of the work added
  /// to the scope has completed. The scope becomes implicitly closed once the
  /// join() operation completes. A scope can only be joined once.
  join_sender join() noexcept { return join_sender{*this}; }

private:
  bool is_in_valid_final_state() noexcept {
    auto state = ref_count_and_closed_flag_.load(std::memory_order_relaxed);
    auto ref_count = state / ref_count_increment;
    bool join_has_run = (join_op_ != nullptr);

    return (ref_count == 0 && join_has_run) ||
        (ref_count == 1 && !join_has_run);
  }

  void start_join(join_op_base* op) noexcept;
  void increment_ref_count() noexcept;
  void decrement_ref_count() noexcept;
  counting_scope_ref try_get_counting_scope_ref() noexcept;

  friend class counting_scope_token;
  friend class counting_scope_ref;

  static constexpr std::size_t ref_count_increment = 2;
  static constexpr std::size_t closed_flag = 1;

  // Initial ref-count is 1. This is decremented by start_join() after
  // setting the join_op_ pointer.
  std::atomic<std::size_t> ref_count_and_closed_flag_{ref_count_increment};
  join_op_base* join_op_{nullptr};
};

inline void counting_scope::start_join(join_op_base* op) noexcept {
  assert(join_op_ == nullptr);
  join_op_ = op;

  decrement_ref_count();
}

inline void counting_scope::increment_ref_count() noexcept {
  [[maybe_unused]] auto old_state = ref_count_and_closed_flag_.fetch_add(
      ref_count_increment, std::memory_order_relaxed);
  assert(old_state >= ref_count_increment);
}

inline void counting_scope::decrement_ref_count() noexcept {
  auto old_state = ref_count_and_closed_flag_.fetch_sub(
      ref_count_increment, std::memory_order_acq_rel);
  assert(old_state >= ref_count_increment);

  if (old_state < (2 * ref_count_increment)) {
    // There weren't any outstanding operations - complete synchronously.
    join_op_->execute(join_op_);
  }
}

//
// counting_scope_ref
//

class counting_scope_ref {
public:
  counting_scope_ref(counting_scope_ref&& other) noexcept
    : scope_(std::exchange(other.scope_, nullptr)) {}

  counting_scope_ref(const counting_scope_ref& other) noexcept
    : scope_(other.scope_) {
    if (valid()) {
      scope_->increment_ref_count();
    }
  }

  ~counting_scope_ref() { reset(); }

  bool valid() const noexcept { return scope_ != nullptr; }

  void reset() noexcept {
    if (valid()) {
      std::exchange(scope_, nullptr)->decrement_ref_count();
    }
  }

private:
  friend counting_scope;
  explicit counting_scope_ref(counting_scope* scope) noexcept : scope_(scope) {}
  counting_scope* scope_;
};

inline counting_scope_ref
counting_scope::try_get_counting_scope_ref() noexcept {
  std::size_t old_state =
      ref_count_and_closed_flag_.load(std::memory_order_relaxed);
  do {
    if ((old_state & closed_flag) != 0 || old_state < ref_count_increment) {
      // Scope is closed.
      return counting_scope_ref{nullptr};
    }
  } while (!ref_count_and_closed_flag_.compare_exchange_weak(
      old_state, old_state + ref_count_increment, std::memory_order_relaxed));

  return counting_scope_ref{this};
}

//
// counting_scope_token
//

class counting_scope_token {
public:
  /// Nest some work within a lazily-started sender that adds work to the scope.
  ///
  /// If the associated counting_scope is already closed at the time nest() is
  /// called then the returned sender will immediately complete with a 'stopped'
  /// result.
  template <sender Source>
    requires decay_copyable<Source>
  counting_scope_nest_sender<std::remove_cvref_t<Source>>
  nest(Source&& source) const noexcept(nothrow_decay_copyable<Source>);

  /// Start a sender as work in the associated counting_scope.
  ///
  /// Dynamically allocates storage for the operation-state using \c
  /// std::allocator.
  ///
  /// \param source
  /// The sender to start as work within the scope.
  ///
  /// \return
  /// \c true if the operation was successfully started or \c false if the scope
  /// was already closed.
  template <sender Source>
  bool spawn(Source&& source) const;

  /// Start a sender as work in the associated counting_scope.
  ///
  /// Guarantees that the any allocation will be released before
  /// the associated \c counting_scope::join() operation completes.
  ///
  /// \param source
  /// The sender to start as work within the scope.
  ///
  /// \param allocator
  /// The allocator to use to allocate storage
  ///
  /// \return
  /// \c true if the operation was successfully started or \c false if the scope
  /// was already closed.
  template <sender Source, typename Allocator>
  bool spawn(Source&& source, Allocator alloc) const;

  /// Start executing a sender as work in the associated counting_scope and
  /// return a new sender that can be used to observe the result once the
  /// operation completes.
  ///
  /// If the returned sender is discarded without connecting/starting then a
  /// stop-request is sent to the spawned operation and the operation will
  /// eventually complete and and be later joined by a call to \c join() on the
  /// associated \c counting_scope.
  ///
  /// If the returned sender is connected/started then the operation will
  /// complete when the spawned operation completes. If a stop-request is sent
  /// to the \c spawn_future() operation then the stop-request is forwarded to
  /// the spawned operation before then detaching and completing immediately.
  template <sender Source, typename Allocator>
  counting_scope_future<Source, Allocator, squiz::empty_env>
  spawn_future(Source&& source, Allocator alloc) const;

  template <sender Source>
  counting_scope_future<Source, std::allocator<void>, squiz::empty_env>
  spawn_future(Source&& source) const;

private:
  friend counting_scope;

  explicit counting_scope_token(counting_scope& scope) noexcept
    : scope_(&scope) {}

  counting_scope* scope_;
};

inline counting_scope_token counting_scope::get_token() noexcept {
  return counting_scope_token(*this);
}

//
// counting_scope_token::nest() implementation
//

template <typename Source, typename Receiver>
class counting_scope_nest_op final
  : public inlinable_operation_state<
        counting_scope_nest_op<Source, Receiver>,
        Receiver>
  , public manual_child_operation<
        counting_scope_nest_op<Source, Receiver>,
        receiver_env_t<Receiver>,
        source_tag,
        Source> {
  using inlinable_base =
      inlinable_operation_state<counting_scope_nest_op, Receiver>;
  using child_base = manual_child_operation<
      counting_scope_nest_op,
      receiver_env_t<Receiver>,
      source_tag,
      Source>;

public:
  counting_scope_nest_op(
      counting_scope_ref scope_handle,
      Source&& source,
      Receiver r) noexcept(child_base::is_nothrow_connectable)
    : inlinable_base(std::move(r))
    , scope_(std::move(scope_handle))
    , active_(scope_.valid()) {
    if (active_) {
      child_base::construct(std::forward<Source>(source));
    }
  }

  ~counting_scope_nest_op() {
    if (active_) {
      child_base::destruct();
    }
  }

  void start() noexcept {
    if (active_) {
      child_base::start();
    } else {
      squiz::set_stopped(this->get_receiver());
    }
  }

  void request_stop() noexcept
    requires child_base::is_stoppable
  {
    if (active_) {
      child_base::request_stop();
    }
  }

  template <typename Tag, typename... Datums>
  void set_result(
      source_tag,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    scope_.reset();
    squiz::set_result(
        this->get_receiver(), sig, squiz::forward_parameter<Datums>(datums)...);
  }

private:
  counting_scope_ref scope_;
  bool active_;
};

template <typename Source>
class counting_scope_nest_sender {
public:
  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> merge_completion_signatures_t<
          completion_signatures_for_t<
              detail::member_type_t<Self, Source>,
              Env...>,
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
          detail::member_type_t<Self, counting_scope_ref>,
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
      counting_scope_ref scope,
      Source2&&
          source) noexcept(std::is_nothrow_constructible_v<Source, Source2>)
    : scope_(std::move(scope))
    , source_(std::forward<Source2>(source)) {}

  counting_scope_ref scope_;
  Source source_;
};

template <sender Source>
  requires decay_copyable<Source>
counting_scope_nest_sender<std::remove_cvref_t<Source>>
counting_scope_token::nest(Source&& source) const
    noexcept(nothrow_decay_copyable<Source>) {
  return counting_scope_nest_sender<std::remove_cvref_t<Source>>(
      scope_->try_get_counting_scope_ref(), std::forward<Source>(source));
}

//
// counting_scope_token::spawn() implementation
//

template <typename Source, typename Allocator>
class counting_scope_spawn_op final
  : public child_operation<
        counting_scope_spawn_op<Source, Allocator>,
        empty_env,
        source_tag,
        Source> {
  using child_base =
      child_operation<counting_scope_spawn_op, empty_env, source_tag, Source>;
  using alloc_traits = std::allocator_traits<Allocator>::template rebind_traits<
      counting_scope_spawn_op>;
  using alloc_type = typename alloc_traits::allocator_type;

public:
  void set_result(source_tag, squiz::value_t<>) noexcept { destroy(); }
  void set_result(source_tag, squiz::stopped_t) noexcept { destroy(); }

  static void
  spawn(counting_scope_ref scope_ref, Source&& source, alloc_type alloc) noexcept(
      noexcept(
          detail::allocate_memory_unique<counting_scope_spawn_op>(alloc)) &&
      child_base::is_nothrow_connectable) {
    assert(scope_ref.valid());
    auto ptr = detail::allocate_memory_unique<counting_scope_spawn_op>(alloc);
    ::new (static_cast<void*>(ptr.get())) counting_scope_spawn_op(
        std::move(scope_ref), std::forward<Source>(source), std::move(alloc));
    ptr.release()->start();
  }

private:
  counting_scope_spawn_op(
      counting_scope_ref scope_ref,
      Source&& source,
      Allocator&& allocator) noexcept(child_base::is_nothrow_connectable)
    : child_base(std::forward<Source>(source))
    , scope_ref_(std::move(scope_ref))
    , allocator_(std::move(allocator)) {}

  void start() noexcept { child_base::start(); }

  void destroy() noexcept {
    auto scope_ref = std::move(scope_ref_);
    alloc_type alloc = std::move(allocator_);
    alloc_traits::destroy(alloc, this);
    alloc_traits::deallocate(alloc, this, 1);
    // scope_ref goes out of scope here and decrements the ref-count.
  }

  counting_scope_ref scope_ref_;
  [[no_unique_address]] alloc_type allocator_;
};

template <sender Source, typename Allocator>
inline bool
counting_scope_token::spawn(Source&& source, Allocator alloc) const {
  auto scope_ref = scope_->try_get_counting_scope_ref();
  if (!scope_ref.valid()) {
    return false;
  }

  counting_scope_spawn_op<Source, Allocator>::spawn(
      std::move(scope_ref), std::forward<Source>(source), std::move(alloc));
  return true;
}

template <sender Source>
inline bool counting_scope_token::spawn(Source&& source) const {
  return this->spawn(std::forward<Source>(source), std::allocator<void>{});
}

//
// counting_scope_token::spawn_future() implementation
//

template <typename Sig>
struct counting_scope_future_vtable_entry;

template <typename Tag, typename... Datums>
struct counting_scope_future_vtable_entry<result_t<Tag, Datums...>> {
  using set_result_func_t =
      void(void*, parameter_type<Datums>... datums) noexcept;
  set_result_func_t* set_result;
};

template <typename... Sigs>
struct counting_scope_future_vtable
  : private counting_scope_future_vtable_entry<Sigs>... {
  constexpr counting_scope_future_vtable(
      typename counting_scope_future_vtable_entry<
          Sigs>::set_result_func_t*... funcs) noexcept
    : counting_scope_future_vtable_entry<Sigs>(funcs)... {}

  template <typename Sig>
    requires one_of<Sig, Sigs...>
  constexpr typename counting_scope_future_vtable_entry<Sig>::set_result_func_t*
  get() const noexcept {
    using entry_t = counting_scope_future_vtable_entry<Sig>;
    return static_cast<const entry_t*>(this)->set_result;
  }
};

namespace counting_scope_detail {
template <typename T>
using pass_parameter_by_value_if_not_already_t =
    std::conditional_t<enable_pass_by_value<T>, T&&, T>;
}

template <typename Source, typename Allocator, typename Env>
class counting_scope_future_state
  : public child_operation<
        counting_scope_future_state<Source, Allocator, Env>,
        detail::make_env_with_stop_possible_t<Env>,
        source_tag,
        Source> {
  using child_base = child_operation<
      counting_scope_future_state,
      detail::make_env_with_stop_possible_t<Env>,
      source_tag,
      Source>;
  using alloc_traits = std::allocator_traits<Allocator>::template rebind_traits<
      counting_scope_future_state>;
  using alloc_type = typename alloc_traits::allocator_type;

public:
  using result_signatures_t =
      detail::add_error_if_move_can_throw_t<completion_signatures_for_t<
          Source,
          detail::make_env_with_stop_possible_t<Env>>>;
  using receiver_vtable_t =
      result_signatures_t::template apply<counting_scope_future_vtable>;

private:
  using result_variant_t =
      detail::completion_signatures_to_variant_of_tuple_t<result_signatures_t>;

  counting_scope_future_state(
      counting_scope_ref scope_ref,
      Source&& source,
      Env env,
      alloc_type
          alloc) noexcept(child_base::is_nothrow_connectable && std::is_nothrow_move_constructible_v<Env>)
    : child_base(std::forward<Source>(source))
    , scope_(std::move(scope_ref))
    , env_(std::move(env))
    , allocator_(alloc) {}

  ~counting_scope_future_state() = default;

  void start() noexcept { child_base::start(); }

public:
  static counting_scope_future_state* allocate_and_start(
      counting_scope_ref scope_ref,
      Source&& source,
      Env env,
      alloc_type alloc) {
    auto ptr =
        detail::allocate_memory_unique<counting_scope_future_state>(alloc);
    ::new (static_cast<void*>(ptr.get())) counting_scope_future_state(
        std::move(scope_ref),
        std::forward<Source>(source),
        std::move(env),
        std::move(alloc));
    ptr->start();
    return ptr.release();
  }

  /// Method called when a consumer operation-state has previously called
  /// set_receiver() and is now trying to detach from the future-state in its
  /// request_stop() method.
  ///
  /// This is potentially racing with \c set_result() trying to call the
  /// receiver.
  ///
  /// \return
  /// \c true if we have successfully won the race with set_result() and can
  /// guarantee that the receiver method will not be invoked when the operation
  /// completes.
  /// \c false if there is a concurrent call to \c set_result() which will
  /// invoke the receiver.
  bool try_detach() noexcept {
    assert(is_stop_possible_);

    state_t old_state = state_.load(std::memory_order_acquire);
    assert((old_state & has_waiting_receiver_flag) != 0);

    if ((old_state & completed_flag) != 0) {
      // Already completed
      return false;
    }

    assert(old_state == has_waiting_receiver_flag);

    if (!state_.compare_exchange_strong(
            old_state, old_state | detached_flag, std::memory_order_acquire)) {
      // Operation completed concurrently and won the race.
      assert(old_state == (has_waiting_receiver_flag | completed_flag));
      return false;
    }

    // Successfully won the race against completion and detached the receiver.

    // Send a stop-request to the child operation (if it supports it)
    if constexpr (child_base::is_stoppable) {
      child_base::request_stop();

      // Signal that we have finished calling request_stop().
      // It's possible that the operation may have completed during this call
      // and that it has delegated responsibility for destroying the
      // future-state to us.
      old_state = state_.load(std::memory_order_acquire);
      if ((old_state & completed_flag) != 0) {
        destroy();
      } else {
        old_state =
            state_.fetch_add(stop_request_done_flag, std::memory_order_acq_rel);
        if ((old_state & completed_flag) != 0) {
          destroy();
        }
      }
    }

    return true;
  }

  /// This method is called to detach the consumer from the state.
  ///
  /// This is called from the destructor of a future object that was not
  /// connected/started.
  ///
  /// Once a future has been connected/started (i.e. \c set_receiver() has been
  /// called) you must call \c try_detach() instead of this method.
  void detach() noexcept {
    state_t old_state = state_.load(std::memory_order_acquire);
    assert((old_state & has_waiting_receiver_flag) == 0);

    if ((old_state & completed_flag) == 0) {
      // Doesn't seem to have completed yet.
      // Try to set the flag to decide the race.
      old_state = state_.fetch_add(detached_flag, std::memory_order_acquire);
      if ((old_state & completed_flag) == 0) {
        // Successfully set the 'detached' state before it completed.
        if constexpr (child_base::is_stoppable) {
          // Need to send a stop-request to the operation.
          child_base::request_stop();

          // Check if it completed while calling request_stop().
          old_state = state_.load(std::memory_order_acquire);
          if ((old_state & completed_flag) == 0) {
            // Doesn't seem to have completed yet.
            // Try setting the flag to decide the race.
            old_state = state_.fetch_add(
                stop_request_done_flag, std::memory_order_acq_rel);
            if ((old_state & completed_flag) == 0) {
              // Successfully set the 'request_done' flag before the operation
              // completed. We have delegated responsibility for destruction
              // of the state to the set_result() call.
              return;
            }
          }
        } else {
          // No subsequent call to request_stop() is needed.
          // We have successfully delegated destruction of the state
          // to the set_result() call.
          return;
        }
      }
    }

    // Otherwise, we are responsible for destruction.

    assert((old_state & completed_flag) != 0);

    destroy();
  }

  void set_receiver(
      const receiver_vtable_t* vtable,
      void* data,
      bool is_stop_possible) noexcept {
    assert(vtable != nullptr);
    assert(receiver_vtable_ == nullptr);
    receiver_vtable_ = vtable;
    receiver_data_ = data;
    is_stop_possible_ = is_stop_possible;

    state_t old_state = state_.load(std::memory_order_acquire);
    if (old_state == completed_flag) {
      state_.store(
          completed_flag | has_waiting_receiver_flag,
          std::memory_order_relaxed);
    } else {
      old_state = state_.fetch_add(
          has_waiting_receiver_flag, std::memory_order_acq_rel);
      if (old_state != completed_flag) {
        // We set the 'has_waiting_receiver_flag' before the op completed.
        // It is the responsibility of the 'set_result()' call to deliver the
        // result.
        assert(old_state == 0);
        return;
      }
    }

    // Operation has already completed - deliver the result synchronously.
    deliver_stored_result();
  }

  template <typename Tag, typename... Datums>
  void set_result(
      source_tag,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    auto disposition = try_begin_set_result();
    if (disposition == completion_disposition_t::discard) {
      return;
    } else if (disposition == completion_disposition_t::invoke_receiver) {
      deliver_result(sig, squiz::forward_parameter<Datums>(datums)...);
      return;
    }

    assert(disposition == completion_disposition_t::store_result);

    // Store the result and then set the 'completed_flag' to publish the result.
    state_t old_state;
    try {
      auto& result_tuple = result_.template emplace<std::tuple<Tag, Datums...>>(
          Tag{}, std::forward<Datums>(datums)...);

      old_state = state_.fetch_add(completed_flag, std::memory_order_acq_rel);
      if (old_state == has_waiting_receiver_flag) {
        // A receiver was concurrently registered
        deliver_result_tuple(result_tuple);
        return;
      }
    } catch (...) {
      constexpr bool is_nothrow =
          (std::is_nothrow_move_constructible_v<Datums> && ...);
      if constexpr (!is_nothrow) {
        auto& result_tuple =
            result_.template emplace<std::tuple<error_tag, std::exception_ptr>>(
                error_tag{}, std::current_exception());
        old_state = state_.fetch_add(completed_flag, std::memory_order_acq_rel);
        if (old_state == has_waiting_receiver_flag) {
          // A receiver was concurrently registered
          deliver_result_tuple(result_tuple);
          return;
        }
      } else {
        assert(false);
        std::unreachable();
      }
    }

    // If the consumer has fully-detached then we can destroy the op-state.
    // Otherwise, we have delegated completion/destruction to a subsequent
    // or concurrent call to set_receiver(), detach() or try_detach().

    if ((old_state & fully_detached_flags) == fully_detached_flags) {
      destroy();
    }
  }

  detail::make_env_with_stop_possible_t<Env> get_env(source_tag) noexcept {
    // TODO: Return an env_ref<Env> rather than a copy.
    return env_;
  }

  void destroy() noexcept {
    assert((state_.load(std::memory_order_relaxed) & completed_flag) != 0);
    auto scope_handle = std::move(scope_);
    auto alloc = std::move(allocator_);
    this->~counting_scope_future_state();
    alloc_traits::deallocate(alloc, this, 1);
    // scope_handle goes out of scope here
  }

private:
  enum class completion_disposition_t {
    discard,
    invoke_receiver,
    store_result
  };

  /// Helper method called at the start of set_result() to determine if there
  /// are any fast-paths to take for completion - either
  completion_disposition_t try_begin_set_result() noexcept {
    // First check for cases that do not require any atomic RMW ops to decide.
    state_t old_state = state_.load(std::memory_order_acquire);
    if ((old_state & fully_detached_flags) == fully_detached_flags) {
      // Safe to destroy.
      state_.store(old_state | completed_flag, std::memory_order_relaxed);
      destroy();
      return completion_disposition_t::discard;
    }

    if (old_state == has_waiting_receiver_flag && !is_stop_possible_) {
      // Receiver has already been attached, no possible race for completion.
      state_.store(
          has_waiting_receiver_flag | completed_flag,
          std::memory_order_relaxed);
      return completion_disposition_t::invoke_receiver;
    }

    return completion_disposition_t::store_result;
  }

  template <typename Tag, typename... Datums>
  void deliver_result(
      result_t<Tag, Datums...>, parameter_type<Datums>... datums) noexcept {
    auto* set_result_fn =
        receiver_vtable_->template get<result_t<Tag, Datums...>>();
    set_result_fn(receiver_data_, squiz::forward_parameter<Datums>(datums)...);
  }

  template <typename Tag, typename... Datums>
  void deliver_result_tuple(std::tuple<Tag, Datums...>& datums_tuple) noexcept {
    std::apply(
        [&](Tag, Datums&... datums) noexcept {
          deliver_result<Tag, Datums...>(
              {}, squiz::forward_parameter<Datums>(datums)...);
        },
        datums_tuple);
  }

  void deliver_stored_result() noexcept {
    std::visit(
        squiz::overload(
            [](std::monostate) noexcept { std::unreachable(); },
            [&]<typename Tag, typename... Datums>(
                std::tuple<Tag, Datums...>& result) noexcept {
              deliver_result_tuple(result);
            }),
        result_);
  }

  using state_t = std::uint8_t;

  static constexpr state_t completed_flag = 1;
  static constexpr state_t detached_flag = 2;
  static constexpr state_t has_waiting_receiver_flag = 4;
  static constexpr state_t stop_request_done_flag = 8;
  static constexpr state_t fully_detached_flags = child_base::is_stoppable
      ? (detached_flag | stop_request_done_flag)
      : detached_flag;

  result_variant_t result_;
  counting_scope_ref scope_;
  const receiver_vtable_t* receiver_vtable_{nullptr};
  void* receiver_data_{nullptr};
  bool is_stop_possible_{true};
  std::atomic<state_t> state_;
  [[no_unique_address]] detail::make_env_with_stop_possible_t<Env> env_;
  [[no_unique_address]] alloc_type allocator_;
};

template <typename Source, typename Allocator, typename Env, typename Receiver>
class counting_scope_future_op final
  : public inlinable_operation_state<
        counting_scope_future_op<Source, Allocator, Env, Receiver>,
        Receiver> {
  using inlinable_base =
      inlinable_operation_state<counting_scope_future_op, Receiver>;
  using future_state_t = counting_scope_future_state<Source, Allocator, Env>;

  static constexpr bool is_stop_possible =
      is_stop_possible_v<receiver_env_t<Receiver>>;

public:
  explicit counting_scope_future_op(future_state_t* state, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , state_(state) {}

  ~counting_scope_future_op() {
    if (state_ != nullptr) {
      state_->detach();
    }
  }

  void start() noexcept {
    if (state_ != nullptr) {
      state_->set_receiver(
          get_vtable(), static_cast<void*>(this), is_stop_possible);
    } else {
      if constexpr (is_stop_possible) {
        // Prevent a call to request_stop() after start() returns from
        // trying to access the state_.
        flags_.store(completed_flag, std::memory_order_relaxed);
      }
      squiz::set_stopped(this->get_receiver());
    }
  }

  void request_stop() noexcept
    requires is_stop_possible
  {
    flags_t old_flags = flags_.load(std::memory_order_relaxed);
    if (old_flags == 0 &&
        flags_.compare_exchange_strong(
            old_flags, stop_requested_flag, std::memory_order_relaxed)) {
      // Won the potential race with completion.
      // We will complete with set_stopped(), but still need to deal with a
      // potential concurrent call to set_result(). If calling try_detach() is
      // successful then it indicates that the future-state will not
      // subsequently invoke the set_result() function and will take care of
      // destroying the future-state when the underlying operation completes.
      // Otherwise, it indicates that the future-state is in the process of
      // trying to call set_result() and so we need to then wait until both this
      // call is done with the state and the set_result() call is done with the
      // state - whichever is last is responsible for destroying the
      // future-state and completing with set_stopped().

      assert(state_ != nullptr);
      if (state_->try_detach()) {
        // set_result() will not be called.
        // The future_state_t object will be destroyed when the underlying
        // operation completes. Clear the state_ pointer to prevent the
        // destructor from calling detach().
        state_ = nullptr;
        squiz::set_stopped(this->get_receiver());
      } else {
        // The operation is in the process of calling set_result().
        // We need to perform another atomic operation to decide who loses the
        // race and must destroy the future-state and send the set_stopped()
        // signal.
        old_flags = flags_.load(std::memory_order_acquire);
        if ((old_flags & completed_flag) == 0) {
          old_flags = flags_.fetch_add(
              stop_request_done_flag, std::memory_order_acq_rel);
          if ((old_flags & completed_flag) == 0) {
            // We have deferred the completion to set_result()
            return;
          }
        }

        // The set_result() function won the race and deferred
        // completion/destruction of the state to us.
        state_->destroy();
        state_ = nullptr;

        squiz::set_stopped(this->get_receiver());
      }
    }
  }

private:
  using vtable_t = typename future_state_t::receiver_vtable_t;

  // Try to set the 'completed_flag' and return 'true' if successful, indicating
  // that the operation should complete with the result of the underlying
  // operation. Otherwise if 'false' is returned then this indicates that a
  // concurrent call to request_stop() won the race and we will complete with
  // 'set_stopped' and so we should not try to complete with the result of the
  // underlying operation.
  bool try_set_completed_flag() noexcept
    requires is_stop_possible
  {
    flags_t old_flags = flags_.load(std::memory_order_acquire);
    if (old_flags != (stop_requested_flag | stop_request_done_flag)) {
      old_flags = flags_.fetch_add(completed_flag, std::memory_order_acq_rel);
    }

    switch (old_flags) {
      case 0:
        // Won the race with request_stop() - will complete with result.
        return true;
      case stop_requested_flag:
        // We delegated completion to request_stop()
        return false;
      case (stop_requested_flag | stop_request_done_flag):
        // request_stop() delegated completion to us
        state_->destroy();
        state_ = nullptr;
        squiz::set_stopped(this->get_receiver());
        return false;
      default: std::unreachable();
    }
  }

  template <typename Tag, typename... Datums>
  static void
  set_result(void* data, parameter_type<Datums>... datums) noexcept {
    auto& self = *static_cast<counting_scope_future_op*>(data);

    if constexpr (is_stop_possible) {
      if (!self.try_set_completed_flag()) {
        return;
      }
    }

    // If any datums are prvalues which are passed by rvalue-ref then they
    // might refer to state within the operation state (which is about to be
    // destroyed). So we ensure that we take copies of any prvalue datums
    // that were passed by rvalue-ref before destroying the op-state and
    // then invoking the receiver.
    // The move-constructors of some types may throw an exception, so handle
    // this case by delivering an error_t<std::exception_ptr> result instead.
    try {
      [&](counting_scope_detail::pass_parameter_by_value_if_not_already_t<
          Datums>... datums_copy) noexcept {
        self.state_->destroy();
        self.state_ = nullptr;
        squiz::set_result(
            self.get_receiver(),
            squiz::result<Tag, Datums...>,
            squiz::forward_parameter<Datums>(datums_copy)...);
      }(std::forward<Datums>(datums)...);
    } catch (...) {
      // Moving the datums to new values on the stack might have thrown an
      // exception. Check if it's possible that the move can throw and if so
      // just forward the exception.
      constexpr bool is_nothrow =
          ((squiz::enable_pass_by_value<Datums> ||
            std::is_nothrow_move_constructible_v<Datums>) &&
           ...);
      if constexpr (!is_nothrow) {
        self.state_->destroy();
        self.state_ = nullptr;
        squiz::set_error<std::exception_ptr>(
            self.get_receiver(), std::current_exception());
      } else {
        assert(false);
        std::unreachable();
      }
    }
  }

  template <typename Tag, typename... Datums>
  static constexpr auto* make_vtable_entry(result_t<Tag, Datums...>) noexcept {
    return &set_result<Tag, Datums...>;
  }

  template <typename... Sigs>
  static constexpr vtable_t
  make_vtable(completion_signatures<Sigs...>) noexcept {
    return vtable_t{make_vtable_entry(Sigs{})...};
  }

  static const vtable_t* get_vtable() noexcept {
    static constexpr vtable_t vtable =
        make_vtable(typename future_state_t::result_signatures_t{});
    return &vtable;
  }

  using flags_t = std::uint8_t;
  static constexpr flags_t completed_flag = 1;
  static constexpr flags_t stop_requested_flag = 2;
  static constexpr flags_t stop_request_done_flag = 4;

  struct empty {
    empty(flags_t) noexcept {}
  };
  using flags_state_t =
      std::conditional_t<is_stop_possible, std::atomic<flags_t>, empty>;

  future_state_t* state_;
  [[no_unique_address]] flags_state_t flags_{0};
};

template <typename Source, typename Allocator, typename Env>
class counting_scope_future {
  using future_state_t = counting_scope_future_state<Source, Allocator, Env>;

public:
  auto
  get_completion_signatures() -> merge_completion_signatures_t<
                                  typename future_state_t::result_signatures_t,
                                  completion_signatures<stopped_t>>;

  auto is_always_nothrow_connectable() -> std::true_type;

  counting_scope_future(const counting_scope_future&) = delete;

  counting_scope_future(counting_scope_future&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

  ~counting_scope_future() {
    if (state_ != nullptr) {
      state_->detach();
    }
  }

  template <typename Receiver>
  counting_scope_future_op<Source, Allocator, Env, Receiver>
  connect(Receiver r) noexcept {
    return counting_scope_future_op<Source, Allocator, Env, Receiver>{
        std::exchange(state_, nullptr), std::move(r)};
  }

private:
  friend class counting_scope_token;

  explicit counting_scope_future(future_state_t* state) noexcept
    : state_(state) {}

  future_state_t* state_;
};

template <sender Source, typename Allocator>
counting_scope_future<Source, Allocator, squiz::empty_env>
counting_scope_token::spawn_future(Source&& source, Allocator alloc) const {
  using state_t =
      counting_scope_future_state<Source, Allocator, squiz::empty_env>;
  using future_t = counting_scope_future<Source, Allocator, squiz::empty_env>;
  auto scope_ref = scope_->try_get_counting_scope_ref();
  if (!scope_ref.valid()) {
    return future_t(nullptr);
  }

  return future_t(state_t::allocate_and_start(
      std::move(scope_ref),
      std::forward<Source>(source),
      squiz::empty_env{},
      std::move(alloc)));
}

template <sender Source>
counting_scope_future<Source, std::allocator<void>, squiz::empty_env>
counting_scope_token::spawn_future(Source&& source) const {
  return spawn_future(std::forward<Source>(source), std::allocator<void>{});
}

}  // namespace squiz
