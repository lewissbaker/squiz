///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/receiver.hpp>
#include <squiz/stop_possible.hpp>

namespace squiz {

struct manual_event_loop {
private:
  struct task_base {
    void (*execute)(task_base*) noexcept;
    task_base* next = nullptr;

    // A null value for prev_next indicates the item is not currently in the
    // queue.
    task_base** prev_next = nullptr;
  };

  template <typename Receiver>
  class schedule_op
    : public inlinable_operation_state<schedule_op<Receiver>, Receiver>
    , private task_base {
    using inlinable_base = inlinable_operation_state<schedule_op, Receiver>;

  public:
    schedule_op(manual_event_loop& loop, Receiver r) noexcept
      : inlinable_base(std::move(r))
      , loop(loop) {
      this->execute = &execute_impl;
    }

    void start() noexcept { loop.enqueue(this); }

    void request_stop() noexcept
      requires is_stop_possible_v<receiver_env_t<Receiver>>
    {
      if (loop.try_remove(this)) {
        squiz::set_stopped(this->get_receiver());
      }
    }

  private:
    static void execute_impl(task_base* task) noexcept {
      auto& self = *static_cast<schedule_op*>(task);
      squiz::set_value<>(self.get_receiver());
    }

    manual_event_loop& loop;
  };

  class scheduler;

  struct schedule_sender {
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
      return {*loop_, std::move(r)};
    }

  private:
    friend scheduler;

    explicit schedule_sender(manual_event_loop* loop) noexcept : loop_(loop) {}

    manual_event_loop* loop_;
  };

  class scheduler final {
  public:
    schedule_sender schedule() const noexcept { return schedule_sender{loop_}; }

    friend bool operator==(scheduler a, scheduler b) noexcept {
      return a.loop_ == b.loop_;
    }

  private:
    friend manual_event_loop;
    explicit scheduler(manual_event_loop* loop) noexcept : loop_(loop) {}

    manual_event_loop* loop_;
  };

public:
  manual_event_loop();

  ~manual_event_loop();

  scheduler get_scheduler() noexcept { return scheduler{this}; }

  void run(std::stop_token st);

  /// Manually drive the event-loop until a stop-request is made on the provided
  /// stop-token.
  template <typename StopToken>
  void run(StopToken st) {
    std::atomic_flag stop_requested{false};
    auto on_stop = [&] noexcept {
      stop_requested.test_and_set();
      wake_run_threads();
    };

    using callback_t =
        typename StopToken::template callback_type<decltype(on_stop)>;
    callback_t cb(std::move(st), on_stop);
    run_impl(stop_requested);
  }

private:
  /// Wakes up any threads currently executing run_impl() that might be blocked
  /// in a call to cv.wait().
  void wake_run_threads() noexcept;

  /// Executes items enqueued until the stop_requested flag is set.
  ///
  /// Call wake_run_threads() after setting the stop_requested to ensure
  /// the thread wakes up and sees the flag change.
  void run_impl(std::atomic_flag& stop_requested);

  /// Tries to dequeue the first item in the queue.
  ///
  /// \pre
  /// The caller currently holds a lock on \c this->mut.
  ///
  /// \return
  /// A pointer to the dequeued item if the queue was not empty.
  /// Otherwise returns nullptr if the queue was empty.
  task_base* try_dequeue() noexcept;

  /// Enqueue a new task at the end of the queue.
  ///
  /// \pre
  /// Calling thread must not currently hold the mutex lock.
  ///
  /// \param task
  /// The task to insert into the queue.
  void enqueue(task_base* item) noexcept;

  /// Try to remove a specific item from the queue.
  ///
  /// Note that this can potentially race with concurrent execution of the item
  /// in the queue and so might fail if a thread calling \c run() has already
  /// dequeued the item.
  ///
  /// \param item
  /// The item to try to remove from the queue.
  ///
  /// \return
  /// \c true if the item could be dequeued - in this case it guarantees that
  /// the item's \c execute member will not be invoked.
  /// \c false if the item was already dequeued.
  bool try_remove(task_base* item) noexcept;

  std::mutex mut;
  std::condition_variable cv;
  task_base* head;
  task_base** last_next;
};
}  // namespace squiz
