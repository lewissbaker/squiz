///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/receiver.hpp>
#include <squiz/stop_possible.hpp>
#include <squiz/detail/atomic_intrusive_queue.hpp>
#include <squiz/detail/binned_time_priority_queue.hpp>
#include <squiz/detail/intrusive_list.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/stop_callback_type.hpp>

#include <squiz/linuxos/detail/file_handle.hpp>
#include <squiz/linuxos/detail/mmap_region.hpp>
#include <squiz/linuxos/io_uring.hpp>

#include <thread>

namespace squiz::linuxos {

/// An \c iouring_context is a manually-driven execution context that supports
/// async I/O operations using the Linux io_uring facilities.
class iouring_context {
  class schedule_sender;
  template <typename Receiver>
  class schedule_op;

  template <typename Derived, typename Receiver>
  class schedule_at_op_base;

  class schedule_at_sender;
  template <typename Receiver>
  class schedule_at_op;

  template <typename Duration>
  class schedule_after_sender;
  template <typename Duration, typename Receiver>
  class schedule_after_op;

  template <typename Derived, typename Receiver>
  class iouring_operation;

  template <typename Receiver>
  class openat_op;
  class openat_sender;

  template <typename Receiver>
  class close_op;
  class close_sender;

public:
  class scheduler;

  /// Initialize the iouring_context with some default io_uring settings.
  iouring_context();

  /// Initialize the iouring_context with specific settings for the io_uring
  /// resource used to
  iouring_context(const io_uring::setup_args& io_uring_args);

  ~iouring_context();

  /// Obtain a scheduler that may be used to schedule work onto this execution
  /// context.
  ///
  /// Note that the work may be pure execution or I/O operations.
  scheduler get_scheduler() noexcept;

  /// Run the event-loop for this execution context.
  ///
  /// Only a single thread may call run() at a time.
  ///
  /// You must call run() for items scheduled to this execution context to
  /// make forward-progress.
  ///
  /// \param stop_token
  /// A stoppable-token that can be sent a stop-request in order to request that
  /// the run() method returns promptly.
  template <typename StopToken>
  void run(StopToken stop_token);

private:
  //
  // operation_base
  //

  struct operation_base {
    union {
      /// Function to be set before scheduling this operation using one of the
      /// schedule_now_...() or schedule_at_...() functions.
      void (*execute)(operation_base*) noexcept;

      /// Function to be set before submitting an SQE with this operation as the
      /// user_data.
      void (*handle_completion)(
          operation_base*, std::int32_t result, std::uint32_t flags) noexcept;

      /// Function to be set before scheduling this using
      /// schedule_when_can_submit_more_sqes().
      ///
      /// Will be invoked when there is at least one SQE.
      ///
      /// \param max_sqes
      /// The current number of SQEs available.
      ///
      /// \return
      /// The number of SQEs that remain to be enqueued.
      /// If this number is greater than zero then the item will remain on the
      /// front of the queue and will be called again when the number of
      /// available SQEs that can be submitted is at least the returned value.
      std::uint32_t (*submit_sqes)(
          operation_base*, std::uint32_t max_sqes) noexcept;
    };
    operation_base* next{nullptr};
    operation_base* prev{nullptr};
  };

  using operation_list = squiz::detail::intrusive_list<
      operation_base,
      &operation_base::next,
      &operation_base::prev>;

  using atomic_operation_queue = squiz::detail::
      atomic_intrusive_queue<operation_base, &operation_base::next>;

  //
  // stoppable_operation_base
  //

  struct stoppable_operation_base {
    union {
      // Function that should be set before calling:
      // - schedule_stop_request_remote()
      void (*execute_stop)(stoppable_operation_base*) noexcept;

      // Function that should be set before calling:
      // - schedule_async_cancel_when_can_submit_more_sqes()
      std::uint32_t (*submit_cancel_sqes)(
          stoppable_operation_base*, std::uint32_t max_sqes) noexcept;
    };
    stoppable_operation_base* stoppable_next{nullptr};
  };

  using stoppable_operation_queue = squiz::detail::intrusive_queue<
      stoppable_operation_base,
      &stoppable_operation_base::stoppable_next>;

  using atomic_stoppable_operation_queue =
      squiz::detail::atomic_intrusive_queue<
          stoppable_operation_base,
          &stoppable_operation_base::stoppable_next>;

  struct time_operation_base : operation_base {
    monotonic_clock::time_point due_time;
  };

  //
  // Methods relating to running the I/O loop
  //

  /// Query if the calling thread is the I/O thread.
  /// i.e. the thread that is currently calling \c run().
  bool is_on_io_thread() const noexcept;

  /// Run the main loop, processing queue items.
  void run_impl(std::atomic<bool>& stop_requested);

  //
  // Methods relating to enqueuing work
  //

  void wake_up_io_thread() noexcept;

  /// Tries to add an sqe to the submission queue that reads from the eventfd
  /// which will be signalled when some thread is trying to wake-up the I/O
  /// thread.
  bool try_submit_wait_for_wake_up() noexcept;

  //
  // Ready to run queue
  //

  /// Enqueue an item that is ready-to-run.
  ///
  /// The I/O loop will eventually process it by calling \c op->execute(op).
  void schedule_now(operation_base* op) noexcept;
  void schedule_now_local(operation_base* op) noexcept;
  void schedule_now_remote(operation_base* op) noexcept;

  /// Remove an operation-state that is in the ready-to run queue and has
  /// not yet been dequeued.
  ///
  /// Only safe to run on the I/O thread.
  void remove_local(operation_base* op) noexcept;

  //
  // Time queue
  //

  /// Enqueue a time operation
  ///
  /// Must be called from the I/O thread.
  void schedule_at_local(time_operation_base* op) noexcept;

  /// Remove an item that was scheduled using schedule_at_local()
  ///
  /// Must be called from the I/O thread.
  void remove_time_op_local(time_operation_base* op) noexcept;

  //
  // Remote queue
  //

  /// Acquire any remote-enqueued items and move them to the ready-to-run queue.
  ///
  /// Moves items enqueued by schedule_now_remote() to the ready_to_run_queue_.
  ///
  /// Must be called from the I/O thread.
  void acquire_remote_items() noexcept;

  /// Acquire any remote-enqueued stop-items and execute them.
  void execute_remote_stop_items() noexcept;

  /// Schedule a stop-request operation to run on the I/O thread
  void schedule_stop_request_remote(stoppable_operation_base* op) noexcept;

  //
  // io_uring SQE submission
  //

  /// Query the number of slots in the SQE buffer that can be submitted at this
  /// time.
  std::uint32_t available_sqe_space() const noexcept;

  /// Try to submit an SQE that is an OP_ASYNC_CANCEL for an outstanding
  /// operation.
  bool try_submit_cancel_for(std::uint64_t user_data) noexcept;

  /// Submit an SQE that is an OP_ASYNC_CANCEL.
  ///
  /// Requires that there is sp
  void submit_cancel_for(std::uint64_t user_data) noexcept;

  /// Schedule the op->submit_sqes method to run when the io_uring
  /// data-structure has space to submit new submission-queue entries.
  void schedule_when_can_submit_more_sqes(operation_base* op) noexcept;

  void schedule_async_cancel_when_can_submit_more_sqes(
      stoppable_operation_base* op) noexcept;

  void execute_pending_sqe_submission_tasks() noexcept;

  //
  // Special io_uring_sqe/cqe user_data values
  //

  // Value used to identify the "wake-up" read on the eventfd.
  std::uint64_t wake_up_user_data() const noexcept {
    return reinterpret_cast<std::uintptr_t>(&wake_up_read_storage_);
  }

  // The user_data value to use for IORING_OP_ASYNC_CANCEL SQEs.
  std::uint64_t cancel_user_data() const noexcept {
    return reinterpret_cast<std::uintptr_t>(&remote_stop_queue_);
  }

private:
  /// The granularity of time-scheduling.
  ///
  /// This controls the size of the bins in the binned_time_priority_queue
  /// data-structure.
  using timer_granularity = std::chrono::duration<std::int64_t, std::micro>;

  // Pick a max-timestamp that allows the binned_time_priority_queue to
  // have 6 major bins, each containing 256 minor bins (1536 bins in total).
  // Using 6 bins instead of the default 8 bins needed to index the full
  // 64-bit range reduces the storage requirement from ~32kB to ~24kB,
  // but requires reindexing the timestamps once every ~8 years.
  static constexpr std::uint64_t max_timestamp = (std::uint64_t(1) << 48) - 1;

  // Reindex the binned_time_priority_queue when the current time gets within
  // 1 week of the maximum possible timestamp that the binned index can
  // represent.
  static constexpr std::uint64_t reindex_threshold_timestamp = max_timestamp -
      std::chrono::duration_cast<timer_granularity>(std::chrono::days(7))
          .count();

  /// Convert a time_point to a "timestamp" value - which effectively becomes a
  /// bin index for the binned_time_priority_queue.
  ///
  /// \param tp
  /// The time_point to convert.
  ///
  /// \param round_up
  /// Whether to round the time-point up to the next bin index or round down
  /// to the previous bin index if the time-point does not fall exactly on
  /// a bin boundary.
  std::uint64_t time_point_to_timestamp(
      const monotonic_clock::time_point& tp, bool round_up) const noexcept;

  struct get_due_time {
    get_due_time(iouring_context& context) noexcept : context_(&context) {}

    const iouring_context* context_;

    /// Computes the number of microseconds until an item's 'due_time'
    ///
    /// The item must actually be a 'time_operation_base' but this needs to
    /// be able to take an 'operation_base' because we reuse the 'next/prev'
    /// pointers from the operation_base.
    std::uint64_t operator()(const operation_base* item) const noexcept {
      auto* time_item = static_cast<const time_operation_base*>(item);
      return context_->time_point_to_timestamp(time_item->due_time, true);
    }
  };

  void reset_timer_base() noexcept;
  monotonic_clock::time_point get_earliest_timer() const noexcept;

  using timer_queue = squiz::detail::binned_time_priority_queue<
      operation_base,  // Note: actual items must be time_operation_base
      &operation_base::next,
      &operation_base::prev,
      std::uint64_t,
      get_due_time,
      max_timestamp>;

  /////////////////
  // State modified by the I/O thread

  io_uring io_uring_;

  std::atomic<std::thread::id> running_thread_id_;

  /// Maximum number of pending CQEs that can safely be outstanding.
  ///
  /// In more recent Linux kernels the kernel is able to buffer additional CQEs
  /// internally if the CQ buffer fills up, and will enqueue them later once you
  /// have freed up space in the CQ buffer. However, earlier kernel versions did
  /// not have this capability and would just drop CQEs if there was not space
  /// in the buffer. This keeps track of the maximum
  std::size_t max_pending_cqes_{0};

  /// The current number of CQEs that are still outstanding.
  std::size_t pending_cqes_{0};

  // Queue of items that are ready-to-run.
  operation_list ready_to_run_queue_;

  // Queue of items that are waiting for SQE space to issue a new SQE.
  operation_list waiting_for_sqes_queue_;

  // Queue of items that are waiting for SQE space to submit an OP_ASYNC_CANCEL
  // SQE.
  stoppable_operation_queue waiting_for_cancel_sqes_queue_;

  // time_points less than or equal to this time get put into the first bin.
  std::int64_t timer_base_time_seconds_;
  // time_points larger than this time get put into the last bin.
  std::int64_t timer_max_time_seconds_;
  // timer item inserted at the time when we need to reindex the the timer
  // queue. when this item is dequeued we will call reset_timer_base().

  struct reindex_timer_operation : time_operation_base {
    explicit reindex_timer_operation(iouring_context& context) noexcept
      : context_(context) {
      this->execute = &execute_impl;
    }

  private:
    static void execute_impl(operation_base* op) noexcept {
      auto& self = *static_cast<reindex_timer_operation*>(op);
      self.context_.reset_timer_base();
    }

    iouring_context& context_;
  };

  reindex_timer_operation reindex_timer_op_;

  detail::file_handle wake_up_event_fd_;
  std::uint64_t wake_up_read_storage_ = 0;
  bool is_wake_up_read_submitted_ = false;

  /////////////////
  // State modified by remote threads.

  // A list of ready-to-run operations enqueued by remote threads.
  alignas(64) atomic_operation_queue remote_queue_;

  atomic_stoppable_operation_queue remote_stop_queue_;

  //////////////////
  // Big objects

  // Put the timer queue last as it is very large.
  alignas(64) timer_queue timer_queue_;
};

template <typename StopToken>
void iouring_context::run(StopToken st) {
  std::atomic<bool> stop_requested{false};

  struct on_stop_request {
    iouring_context& context;
    std::atomic<bool>& stop_requested;

    void operator()() noexcept {
      stop_requested.store(true, std::memory_order_relaxed);
      context.wake_up_io_thread();
    }
  };

  using stop_cb =
      squiz::detail::stop_callback_type_t<StopToken, on_stop_request>;

  stop_cb cb{std::move(st), on_stop_request{*this, stop_requested}};

  run_impl(stop_requested);
}

//
// iouring_context::schedule_op
//

template <typename Receiver>
class iouring_context::schedule_op
  : public inlinable_operation_state<schedule_op<Receiver>, Receiver>
  , private operation_base {
  using inlinable_base = inlinable_operation_state<schedule_op, Receiver>;

public:
  explicit schedule_op(iouring_context& context, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , context_(context) {
    this->execute = &execute_impl;
  }

  void start() noexcept { context_.schedule_now(this); }

private:
  static void execute_impl(operation_base* op) noexcept {
    auto& self = *static_cast<schedule_op*>(op);
    squiz::set_value<>(self.get_receiver());
  }

  iouring_context& context_;
};

template <typename Receiver>
  requires is_stop_possible_v<receiver_env_t<Receiver>>
class iouring_context::schedule_op<Receiver>
  : public inlinable_operation_state<schedule_op<Receiver>, Receiver>
  , private stoppable_operation_base {
  using inlinable_base = inlinable_operation_state<schedule_op, Receiver>;

public:
  explicit schedule_op(iouring_context& context, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , context_(context) {
    this->execute = &execute_impl;
  }

  void start() noexcept {
    if (context_.is_on_io_thread()) {
      state_ = state_t::locally_enqueued;
      context_.schedule_now_local(this);
    } else {
      state_ = state_t::remotely_enqueued;
      context_.schedule_now_remote(this);
    }
  }

  void request_stop() noexcept {
    if (context_.is_on_io_thread()) {
      switch (state_) {
        case state_t::remotely_enqeued:
          context_.acquire_remote_items();
          [[fallthrough]];
        case state_t::locally_enqueued:
          context_.remove_local(this);
          squiz::set_stopped(this->get_receiver());
      }
    } else {
      // TODO: Consider enqueueing a high-priority task to remote queue that
      // gets processed before other normal tasks. For now we just treat the
      // schedule-op as non-cancellable from remote threads and wait for the I/O
      // thread to dequeue it normally and complete.
    }
  }

private:
  static void execute_impl(operation_base* op) noexcept {
    auto& self = *static_cast<schedule_op*>(op);
    self.state_ = state_t::completed;
    squiz::set_value<>(self.get_receiver());
  }

  enum class state_t : std::uint8_t {
    not_started,
    locally_enqueued,
    remotely_enqueued,
    completed
  };

  iouring_context& context_;
  state_t state_{state_t::not_started};
};

//
// iouring_context::schedule_sender
//

class iouring_context::schedule_sender {
public:
  template <typename Env>
    requires is_stop_possible_v<Env>
  static auto get_completion_signatures(Env)
      -> completion_signatures<value_t<>, stopped_t>;

  template <typename Env>
  static auto
      get_completion_signatures(Env) -> completion_signatures<value_t<>>;

  static auto is_always_nothrow_connectable() -> std::true_type;

  template <typename Receiver>
  schedule_op<Receiver> connect(Receiver r) const noexcept {
    return schedule_op<Receiver>{context_, std::move(r)};
  }

private:
  friend iouring_context::scheduler;
  explicit schedule_sender(iouring_context& context) noexcept
    : context_(context) {}

  iouring_context& context_;
};

//
// iouring_context::scheudle_at_op_base
//

// Non-cancellable version
template <typename Derived, typename Receiver>
class iouring_context::schedule_at_op_base
  : public inlinable_operation_state<Derived, Receiver>
  , protected time_operation_base {
  using inlinable_base = inlinable_operation_state<Derived, Receiver>;

public:
  schedule_at_op_base(iouring_context& context, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , context_(context) {}

protected:
  void start_impl() noexcept {
    if (context_.is_on_io_thread()) {
      when_on_io_context(this);
    } else {
      this->execute = &when_on_io_context;
      context_.schedule_now_remote(this);
    }
  }

private:
  static void when_on_io_context(operation_base* op) noexcept {
    auto& self = *static_cast<schedule_at_op_base*>(op);
    self.execute = &when_complete;
    self.context_.schedule_at_local(&self);
  }

  static void when_complete(operation_base* op) noexcept {
    auto& self = *static_cast<schedule_at_op_base*>(op);
    squiz::set_value<>(self.get_receiver());
  }

  iouring_context& context_;
};

// Cancellable version
template <typename Derived, typename Receiver>
  requires is_stop_possible_v<receiver_env_t<Receiver>>
class iouring_context::schedule_at_op_base<Derived, Receiver>
  : public inlinable_operation_state<Derived, Receiver>
  , protected time_operation_base
  , private stoppable_operation_base {
  using inlinable_base = inlinable_operation_state<Derived, Receiver>;

protected:
  schedule_at_op_base(iouring_context& context, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , context_(context) {}

  /// Launches the operation.
  ///
  /// @pre
  /// The @c due_time member has been initialized to the desired due-time of the
  /// schedule_at operation.
  void start_impl() noexcept {
    if (context_.is_on_io_thread()) {
      remote_enqueued_ = false;
      this->execute = &when_timer_elapsed;
      context_.schedule_at_local(this);
    } else {
      remote_enqueued_ = true;
      this->execute = &when_on_io_context;
      context_.schedule_now_remote(this);
    }
  }

public:
  void request_stop() noexcept {
    state_t old_state = state_.load(std::memory_order_relaxed);
    if ((old_state & completed_flag) != 0) {
      return;
    }

    if (context_.is_on_io_thread()) {
      // Running on the I/O thread.
      on_stop_before_complete(this);
    } else {
      // On a remote thread.
      // Decide the race between the timer finishing and the stop-request being
      // posted.
      old_state = state_.fetch_add(
          remote_stop_request_enqueued_flag, std::memory_order_relaxed);
      if ((old_state & completed_flag) != 0) {
        // Completed concurrently
        return;
      }

      // Otherwise we need to post a 'cancellation' item
      // to the I/O thread for it to wake up and run logic
      // to remove the item from the timer queue.
      this->execute_stop = &on_stop_before_complete;
      context_.schedule_stop_request_remote(this);
    }
  }

private:
  static void when_on_io_context(operation_base* op) noexcept {
    auto& self = *static_cast<schedule_at_op_base*>(op);
    assert(self.context_.is_on_io_thread());
    self.remote_enqueued_ = false;
    self.execute = &when_timer_elapsed;
    self.context_.schedule_at_local(&self);
  }

  static void when_timer_elapsed(operation_base* op) noexcept {
    auto& self = *static_cast<schedule_at_op_base*>(op);
    assert(self.context_.is_on_io_thread());

    state_t old_state = self.state_.load(std::memory_order_relaxed);
    if ((old_state & remote_stop_request_enqueued_flag) == 0) {
      old_state =
          self.state_.fetch_add(completed_flag, std::memory_order_relaxed);
    }

    if ((old_state & remote_stop_request_enqueued_flag) != 0) {
      // A stop-request came in before the timer elapsed.
      // Let the stop-request item handle the completion.
      self.execute_stop = &on_stop_after_complete;
      return;
    }

    // Otherwise, we can just complete
    squiz::set_value<>(self.get_receiver());
  }

  static void on_stop_before_complete(stoppable_operation_base* op) noexcept {
    auto& self = *static_cast<schedule_at_op_base*>(op);
    assert(self.context_.is_on_io_thread());
    if (self.remote_enqueued_) {
      // Task for adding the timer might still be in the remote-queue.
      // So acquire the remote items and have them added to the ready-queue.
      self.context_.acquire_remote_items();

      // Then we can just remove it from the ready-queue.
      self.context_.remove_local(&self);
    } else if (
        (self.state_.load(std::memory_order_relaxed) & completed_flag) == 0) {
      // Need to remove it from the timer-queue.
      self.context_.remove_time_op_local(&self);
    }

    // Now can signal completion.
    squiz::set_stopped(self.get_receiver());
  }

  static void on_stop_after_complete(stoppable_operation_base* op) noexcept {
    auto& self = *static_cast<schedule_at_op_base*>(op);
    assert(self.context_.is_on_io_thread());
    squiz::set_stopped(self.get_receiver());
  }

  using state_t = std::uint8_t;

  static constexpr state_t remote_stop_request_enqueued_flag = 1;
  static constexpr state_t completed_flag = 2;

  iouring_context& context_;
  std::atomic<state_t> state_{0};
  bool remote_enqueued_{false};
};

//
// iouring_context::schedule_at_op
//

template <typename Receiver>
class iouring_context::schedule_at_op
  : public iouring_context::
        schedule_at_op_base<schedule_at_op<Receiver>, Receiver> {
  using base = schedule_at_op_base<schedule_at_op, Receiver>;

public:
  schedule_at_op(
      iouring_context& context,
      monotonic_clock::time_point due_time,
      Receiver r) noexcept
    : base(context, std::move(r)) {
    this->due_time = due_time;
  }

  void start() noexcept { this->start_impl(); }
};

//
// iouring_context::schedule_at_sender
//

class iouring_context::schedule_at_sender {
public:
  template <typename Env>
    requires is_stop_possible_v<Env>
  static auto get_completion_signatures(Env)
      -> completion_signatures<value_t<>, stopped_t>;

  template <typename Env>
  static auto
      get_completion_signatures(Env) -> completion_signatures<value_t<>>;

  static auto is_always_nothrow_connectable() -> std::true_type;

  template <typename Receiver>
  schedule_at_op<Receiver> connect(Receiver r) const noexcept {
    return schedule_at_op<Receiver>(context_, due_time_, std::move(r));
  }

private:
  friend iouring_context::scheduler;

  schedule_at_sender(
      iouring_context& context, monotonic_clock::time_point due_time) noexcept
    : context_(context)
    , due_time_(due_time) {}

  iouring_context& context_;
  monotonic_clock::time_point due_time_;
};

//
// iouring_context::schedule_after_op
//

template <typename Duration, typename Receiver>
class iouring_context::schedule_after_op
  : public schedule_at_op_base<
        schedule_after_op<Duration, Receiver>,
        Receiver> {
  using base = schedule_at_op_base<schedule_after_op, Receiver>;

public:
  template <typename Duration2>
  schedule_after_op(
      iouring_context& context,
      Duration2&& delay,
      Receiver r) noexcept(std::is_nothrow_constructible_v<Duration, Duration2>)
    : base(context, std::move(r))
    , delay_(std::forward<Duration2>(delay)) {}

  void start() noexcept {
    this->due_time = monotonic_clock::now() + delay_;
    this->start_impl();
  }

private:
  Duration delay_;
};

//
// iouring_context::schedule_after_sender
//

template <typename Duration>
class iouring_context::schedule_after_sender {
public:
  template <typename Env>
    requires is_stop_possible_v<Env>
  static auto get_completion_signatures(Env)
      -> completion_signatures<value_t<>, stopped_t>;

  template <typename Env>
  static auto
      get_completion_signatures(Env) -> completion_signatures<value_t<>>;

  template <typename Self>
  auto is_always_nothrow_connectable(this Self&&)
      -> std::bool_constant<
          std::is_nothrow_move_constructible_v<Duration> &&
          std::is_nothrow_constructible_v<
              Duration,
              squiz::detail::member_type_t<Self, Duration>>>;

  template <typename Self, typename Receiver>
  schedule_after_op<Duration, Receiver>
  connect(this Self&& self, Receiver r) noexcept(
      decltype(std::forward<Self>(self)
                   .is_always_nothrow_connectable())::value) {
    return schedule_after_op<Duration, Receiver>(
        self.context_, std::forward<Self>(self).delay_, std::move(r));
  }

private:
  friend iouring_context::scheduler;

  template <typename Duration2>
  schedule_after_sender(iouring_context& context, Duration2&& delay) noexcept(
      std::is_nothrow_constructible_v<Duration, Duration2>)
    : context_(context)
    , delay_(std::forward<Duration2>(delay)) {}

  iouring_context& context_;
  Duration delay_;
};

//
// iouring_context::iouring_operation
//

// non-cancellable version
template <typename Derived, typename Receiver>
class iouring_context::iouring_operation
  : public inlinable_operation_state<Derived, Receiver>
  , private operation_base {
  using inlinable_base = inlinable_operation_state<Derived, Receiver>;

protected:
  iouring_operation(iouring_context& context, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , context_(context) {}

public:
  void start() noexcept {
    if (context_.is_on_io_thread()) {
      when_on_io_thread(this);
    } else {
      this->execute = &when_on_io_thread;
      context_.schedule_now_remote(this);
    }
  }

protected:
  std::uint64_t user_data_value() noexcept {
    return reinterpret_cast<std::uintptr_t>(static_cast<operation_base*>(this));
  }

private:
  // Default implementation - may be overridden by derived classes.
  static void when_on_io_thread(operation_base* op) noexcept {
    auto& self = *static_cast<iouring_operation*>(op);
    const std::uint32_t max_sqes = self.context_.available_sqe_space();
    if (max_sqes > 0) [[likely]] {
      when_sqes_available(op, max_sqes);
    } else {
      self.submit_sqes = &when_sqes_available;
      self.context_.schedule_when_can_submit_more_sqes(op);
    }
  }

  static std::uint32_t when_sqes_available(
      operation_base* op, [[maybe_unused]] std::uint32_t max_sqes) noexcept {
    assert(max_sqes > 0);
    auto& self = *static_cast<iouring_operation*>(op);
    auto* sqe = self.context_.io_uring_.get_next_submission_entry();
    static_cast<Derived*>(op)->populate_sqe(sqe);
    self.handle_completion = &when_op_completed;
    self.context_.io_uring_.enqueue_entry_for_submission(sqe);
    return 0;
  }

  static void when_op_completed(
      operation_base* op,
      std::int32_t result,
      [[maybe_unused]] std::uint32_t flags) noexcept {
    static_cast<Derived*>(op)->on_complete(result, flags);
  }

  iouring_context& context_;
};

// cancellable version
template <typename Derived, typename Receiver>
  requires is_stop_possible_v<receiver_env_t<Receiver>>
class iouring_context::iouring_operation<Derived, Receiver>
  : public inlinable_operation_state<Derived, Receiver>
  , protected operation_base
  , protected stoppable_operation_base {
  using inlinable_base = inlinable_operation_state<Derived, Receiver>;

protected:
  iouring_operation(iouring_context& context, Receiver r) noexcept
    : inlinable_base(std::move(r))
    , context_(context) {}

public:
  void start() noexcept {
    if (context_.is_on_io_thread()) {
      try_submit_sqe();
    } else {
      this->execute = &when_on_io_thread;
      remote_enqueued_ = true;
      context_.schedule_now_remote(this);
    }
  }

  void request_stop() noexcept {
    state_t old_state = state_.load(std::memory_order_relaxed);
    if ((old_state & completed_flag) != 0) {
      // Already completed, nothing to do.
      return;
    }

    if (context_.is_on_io_thread()) {
      request_stop_from_io_thread();
    } else {
      old_state = state_.fetch_add(
          stop_request_enqueued_flag, std::memory_order_relaxed);
      if ((old_state & completed_flag) == 0) {
        this->execute_stop = &when_stop_request_on_io_thread;
        context_.schedule_stop_request_remote(this);
      }
    }
  }

private:
  //
  // Steps for normal path
  //

  static void when_on_io_thread(operation_base* op) noexcept {
    auto& self = *static_cast<iouring_operation*>(op);
    self.remote_enqueued_ = false;

    const state_t state = self.state_.load(std::memory_order_relaxed);
    if ((state & stop_request_enqueued_flag) != 0) {
      // A stop request arrived before we submitted the SQE.
      // Need to wait until the remote-enqueued stop request gets
      // processed for it to issue the set_stopped() result.
      return;
    }

    self.try_submit_sqe();
  }

  void try_submit_sqe() noexcept {
    const std::uint32_t max_sqes = context_.available_sqe_space();
    if (max_sqes > 0) [[likely]] {
      submit_sqe();
    } else {
      waiting_for_sqe_ = true;
      this->submit_sqes = &when_sqes_available;
      context_.schedule_when_can_submit_more_sqes(this);
    }
  }

  static std::uint32_t when_sqes_available(
      operation_base* op,
      [[maybe_unused]] std::uint32_t available_sqes) noexcept {
    assert(available_sqes > 0);
    auto& self = *static_cast<iouring_operation*>(op);
    self.waiting_for_sqe_ = false;

    const state_t state = self.state_.load(std::memory_order_relaxed);
    if ((state & stop_request_enqueued_flag) != 0) {
      // A stop request has been enqueued.
      // Don't submit any SQEs and wait until the remote stop-request
      // has been processed to complete with set_stopped().
      // See when_stop_request_on_io_thread().
      return 0;
    }

    self.submit_sqe();
    return 0;
  }

  void submit_sqe() noexcept {
    auto* sqe = context_.io_uring_.get_next_submission_entry();
    static_cast<Derived*>(this)->populate_sqe(sqe);
    this->sqe_submitted_ = true;
    this->handle_completion = &when_op_completed;
    context_.io_uring_.enqueue_entry_for_submission(sqe);
  }

  // Called when the SQE submitted with submit_sqe() issues a corresponding CQE
  // indicating the operation completed.
  //
  // This handler handles the possibility of a stop-request being issued
  // concurrently.
  static void when_op_completed(
      operation_base* op, std::int32_t result, std::uint32_t flags) noexcept {
    auto& self = *static_cast<iouring_operation*>(op);
    state_t old_state = self.state_.load(std::memory_order_relaxed);
    if ((old_state & stop_request_enqueued_flag) == 0) {
      // A stop request has not yet been made.
      // Need and atomic op to decide the race
      old_state =
          self.state_.fetch_add(completed_flag, std::memory_order_relaxed);
    } else {
      self.state_.store(old_state | completed_flag, std::memory_order_relaxed);
    }

    if ((old_state & stop_request_enqueued_flag) != 0) {
      // A pending stop-request has been enqueued from another thread.
      // Can't complete until the remote-stop-request item is dequeued.
      //
      // Stash the result and wait for the remote stop-request to be
      // dequeued and processed in when_stop_request_on_io_thread().
      self.store_result(result, flags);
      return;
    }

    // Otherwise we can just deliver the result.
    self.deliver_result(result, flags);
  }

  void store_result(std::int32_t result, std::uint32_t flags) noexcept {
    result_ = result;
    result_flags_ = flags;
  }

  void deliver_stored_result() noexcept {
    deliver_result(result_, result_flags_);
  }

  void
  deliver_result(std::int32_t result, std::uint32_t result_flags) noexcept {
    if (result == -ECANCELED) {
      squiz::set_stopped(this->get_receiver());
    } else {
      static_cast<Derived*>(this)->on_complete(result, result_flags);
    }
  }

  //
  // Steps for request_stop() path
  //

  // Called when a stop-request submitted from a remote thread gets
  // scheduled on the I/O thread.
  static void
  when_stop_request_on_io_thread(stoppable_operation_base* op) noexcept {
    auto& self = *static_cast<iouring_operation*>(op);

    // Check if the operation completed while the remote stop-request was
    // queued. As we're on the I/O thread there is no race here, so 'relaxed'
    // memory order is safe.
    const state_t state = self.state_.load(std::memory_order_relaxed);
    if ((state & completed_flag) != 0) {
      // Completed before we could submit an SQE to cancel the operation.
      // Just need to deliver the result which was stashed by
      // when_op_completed().
      self.deliver_stored_result();
      return;
    }

    self.request_stop_from_io_thread();
  }

  // Called either from request_stop() (if it was called from I/O thread) or
  // from when_stop_request_on_io_thread() (if a remote-stop-request was
  // enqueued).
  void request_stop_from_io_thread() noexcept {
    if (sqe_submitted_) [[likely]] {
      // Has been issued to io_uring, but hasn't yet completed.
      // Need to send an OP_ASYNC_CANCEL to cancel this operation.
      if (context_.try_submit_cancel_for(user_data_value())) [[likely]] {
        // Successfully submitted a cancel SQE.

        // Update the function that handles completion to something that
        // doesn't need to deal with potential races with request_stop()
        // as request_stop() has already been called.
        this->handle_completion = &when_op_completed_no_races_with_stop;
      } else {
        // Not able to enqueue any SQEs at the moment.
        // Schedule submitting a cancel SQE when SQEs next available.

        // Enqueue into the 'cancel-ops waiting for SQEs queue'
        this->submit_cancel_sqes = &when_sqes_available_for_stop;
        context_.schedule_async_cancel_when_can_submit_more_sqes(this);

        // Also update the completion-handler to something that handles
        // stashing the result if it completes before we obtain an SQE
        // for cancelling and then completing when the submit_cancel_sqes
        // callback is invoked.
        this->handle_completion = &when_op_completed_with_stop_waiting_for_sqes;
      }
      return;
    }

    if (remote_enqueued_) {
      // Has been remote-enqueued but has not yet run the
      // when_on_io_thread() callback (which sets remote_enqueued_ back to
      // false).

      // Ensure that the item is flushed from the remote queue.
      context_.acquire_remote_items();

      // Can just remove the operation before it is issued to the OS.
      context_.remove_local(this);
    } else if (waiting_for_sqe_) {
      // Has been added to the waiting_for_sqes_queue_.

      // Operation still hasn't been submitted to kernel, so can
      // just remove it from the queue and then complete with set_stopped().
      context_.waiting_for_sqes_queue_.remove(this);
    } else {
      // Otherwise, we are in the case where the original start-opperation was
      // enqueued remotely but was processed after the stop-request came in
      // and so the when_on_io_thread() callback just returned without
      // submitting the SQE with the idea that it would complete once the remote
      // stop-request was processed by the I/O thread. i.e. now.
    }

    // This op-state should no longer exist in any queues and so we can just
    // complete with set_stopped.
    squiz::set_stopped(this->get_receiver());
  }

  // Called when the operation completes after a cancel SQE has already been
  // sent.
  //
  // In this case, we know there are no potential races with a call to
  // request_stop() so we can just immediately deliver the result.
  static void when_op_completed_no_races_with_stop(
      operation_base* op, std::int32_t result, std::uint32_t flags) noexcept {
    static_cast<iouring_operation*>(op)->deliver_result(result, flags);
  }

  // Called when the operation completed while waiting for SQE space to become
  // available to submit an OP_ASYNC_CANCEL SQE.
  static void when_op_completed_with_stop_waiting_for_sqes(
      operation_base* op, std::int32_t result, std::uint32_t flags) noexcept {
    auto& self = *static_cast<iouring_operation*>(op);

    // There is still an item in the queue that was waiting to submit an
    // OP_ASYNC_CANCEL SQE. Unfortunately, that queue is a singly-linked list
    // and so cannot support removal of arbitrary elements (removal is only
    // supported from the front).
    //
    // So we just have to stash the result and wait until the SQE-available
    // callback gets run. This is not ideal, but should be a comparatively rare
    // situation as the following needs to have happened:
    // - original operation SQE was submitted
    // - stop-request came in but there was no SQE space to submit the
    // ASYNC_CANCEL sqe.
    // - operation then completes
    // Also, as an operation just completed, this is likely to free up an SQE
    // slot that can unblock the next queued ops waiting for cancel SQEs, so in
    // practice there shouldn't be an unbounded amount of time waiting for the
    // operation to be dequeued.
    //
    // This could be made more deterministic at the cost of an extra pointer in
    // the operation state for every cancellable operation, allowing the
    // stop-queue to be a doubly-linked list.
    self.store_result(result, flags);
    self.submit_cancel_sqes = &when_sqes_available_for_stop_already_completed;
  }

  static std::uint32_t when_sqes_available_for_stop_already_completed(
      stoppable_operation_base* op,
      [[maybe_unused]] std::uint32_t available_sqes) noexcept {
    // Just deliver the previously stashed result
    static_cast<iouring_operation*>(op)->deliver_stored_result();
    return 0;
  }

  // Called when space is available for a cancel SQE to be submitted
  // and we have not yet received notification that the operation has
  // completed.
  static std::uint32_t when_sqes_available_for_stop(
      stoppable_operation_base* op,
      [[maybe_unused]] std::uint32_t available_sqes) noexcept {
    assert(available_sqes > 0);
    auto& self = *static_cast<iouring_operation*>(op);
    self.context_.submit_cancel_for(self.user_data_value());

    // When the eventual completion notification comes in for the
    // operation, it can just complete immediately with that result.
    self.handle_completion = &when_op_completed_no_races_with_stop;

    return 0;
  }

protected:
  std::uint64_t user_data_value() noexcept {
    return reinterpret_cast<std::uintptr_t>(static_cast<operation_base*>(this));
  }

private:
  using state_t = std::uint8_t;

  static constexpr state_t completed_flag = 1;
  static constexpr state_t stop_request_enqueued_flag = 2;

  iouring_context& context_;
  std::int32_t result_;
  std::uint32_t result_flags_;
  std::atomic<state_t> state_{0};
  bool remote_enqueued_ : 1 = false;
  bool waiting_for_sqe_ : 1 = false;
  bool sqe_submitted_ : 1 = false;
};

//
// iouring_context::openat_op
//

template <typename Receiver>
class iouring_context::openat_op final
  : public iouring_operation<openat_op<Receiver>, Receiver> {
  using base = iouring_operation<openat_op<Receiver>, Receiver>;
  friend base;

public:
  openat_op(
      Receiver r,
      iouring_context& context,
      int dirfd,
      const char* path,
      std::uint32_t open_flags,
      mode_t mode) noexcept
    : base(context, std::move(r))
    , path_(path)
    , dirfd_(dirfd)
    , open_flags_(open_flags)
    , mode_(mode) {}

private:
  void populate_sqe(io_uring_sqe* sqe) noexcept {
    io_uring::prepare_openat_op(
        sqe, dirfd_, path_, open_flags_, mode_, this->user_data_value());
  }

  void on_complete(
      std::int32_t result, [[maybe_unused]] std::uint32_t flags) noexcept {
    if (result < 0) {
      squiz::set_error<std::error_code>(
          this->get_receiver(),
          std::error_code(-result, std::system_category()));
    } else {
      squiz::set_value<detail::file_handle>(
          this->get_receiver(), detail::file_handle::adopt(result));
    }
  }

  const char* path_;
  int dirfd_;
  std::uint32_t open_flags_;
  mode_t mode_;
};

//
// iouring_context::openat_sender
//

class iouring_context::openat_sender final {
public:
  template <typename Env>
  static auto
      get_completion_signatures(Env) -> completion_signatures<
                                         squiz::value_t<detail::file_handle>,
                                         squiz::error_t<std::error_code>>;

  template <typename Env>
    requires squiz::is_stop_possible_v<Env>
  static auto
      get_completion_signatures(Env) -> completion_signatures<
                                         squiz::value_t<detail::file_handle>,
                                         squiz::error_t<std::error_code>,
                                         squiz::stopped_t>;

  static auto is_always_nothrow_connectable() -> std::true_type;

  openat_sender(
      iouring_context& context,
      const char* path,
      int dirfd,
      std::uint32_t open_flags,
      mode_t mode) noexcept
    : context_(context)
    , path_(path)
    , dirfd_(dirfd)
    , open_flags_(open_flags)
    , mode_(mode) {}

  template <typename Receiver>
  openat_op<Receiver> connect(Receiver r) noexcept {
    return openat_op<Receiver>{
        std::move(r), context_, dirfd_, path_, open_flags_, mode_};
  }

private:
  iouring_context& context_;
  const char* path_;
  int dirfd_;
  std::uint32_t open_flags_;
  mode_t mode_;
};

//
// iouring_context::close_op
//

template <typename Receiver>
class iouring_context::close_op final
  : public iouring_operation<close_op<Receiver>, Receiver> {
  using base = iouring_operation<close_op<Receiver>, Receiver>;
  friend base;

public:
  close_op(Receiver r, iouring_context& context, int fd) noexcept
    : base(context, std::move(r))
    , fd_(fd) {}

private:
  void populate_sqe(io_uring_sqe* sqe) noexcept {
    io_uring::prepare_close_op(sqe, fd_, this->user_data_value());
  }

  void on_complete(
      std::int32_t result, [[maybe_unused]] std::uint32_t flags) noexcept {
    if (result < 0) {
      squiz::set_error<std::error_code>(
          this->get_receiver(),
          std::error_code(-result, std::system_category()));
    } else {
      squiz::set_value<>(this->get_receiver());
    }
  }

  int fd_;
};

//
// iouring_context::close_sender
//

class iouring_context::close_sender final {
public:
  template <typename Env>
  auto get_completion_signatures(Env) && -> completion_signatures<
                                             squiz::value_t<>,
                                             squiz::error_t<std::error_code>>;
  template <typename Env>
    requires squiz::is_stop_possible_v<Env>
  auto get_completion_signatures(Env) && -> completion_signatures<
                                             squiz::value_t<>,
                                             squiz::error_t<std::error_code>,
                                             squiz::stopped_t>;

  static auto is_always_nothrow_connectable() -> std::true_type;

  close_sender(iouring_context& context, int fd) noexcept
    : context_(context)
    , fd_(fd) {}

  template <typename Receiver>
  close_op<Receiver> connect(Receiver r) && noexcept {
    return close_op<Receiver>{std::move(r), context_, std::exchange(fd_, -1)};
  }

private:
  iouring_context& context_;
  int fd_;
};

//
// iouring_context::scheduler
//

class iouring_context::scheduler {
public:
  scheduler(const scheduler& other) noexcept = default;

  schedule_sender schedule() const noexcept {
    return schedule_sender{*context_};
  }

  monotonic_clock::time_point now() const noexcept {
    return monotonic_clock::now();
  }

  schedule_at_sender
  schedule_at(monotonic_clock::time_point due_time) const noexcept {
    return schedule_at_sender{*context_, due_time};
  }

  template <typename Rep, typename Ratio>
  schedule_after_sender<std::chrono::duration<Rep, Ratio>>
  schedule_after(const std::chrono::duration<Rep, Ratio>& delay) const noexcept(
      std::is_nothrow_copy_constructible_v<std::chrono::duration<Rep, Ratio>>) {
    return schedule_after_sender<std::chrono::duration<Rep, Ratio>>{
        *context_, delay};
  }

  [[nodiscard]] openat_sender
  openat(int dirfd, const char* path, std::uint32_t open_flags, mode_t mode)
      const noexcept {
    return openat_sender{*context_, path, dirfd, open_flags, mode};
  }

  [[nodiscard]] close_sender close(int fd) const noexcept {
    return close_sender{*context_, fd};
  }

  friend bool operator==(const scheduler& a, const scheduler& b) noexcept {
    return a.context_ == b.context_;
  }

private:
  friend iouring_context;

  explicit scheduler(iouring_context& context) noexcept : context_(&context) {}

  iouring_context* context_;
};

inline iouring_context::scheduler iouring_context::get_scheduler() noexcept {
  return scheduler{*this};
}

}  // namespace squiz::linuxos
