///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/linuxos/iouring_context.hpp>

#include <cerrno>
#include <numeric>
#include <system_error>

#include <sys/eventfd.h>
#include <unistd.h>

#if 0
#  include <cinttypes>
#  include <cstdio>
#  define LOG(...)              \
    do {                        \
      std::printf(__VA_ARGS__); \
      std::putchar('\n');       \
      std::fflush(stdout);      \
    } while (false)
#else
#  define LOG(...)
#endif

namespace {

[[noreturn]] void throw_system_error(int error) {
  throw std::system_error(std::error_code(error, std::system_category()));
}

[[noreturn]] void throw_last_errno() {
  throw_system_error(std::exchange(errno, 0));
}

squiz::linuxos::detail::file_handle create_eventfd() {
  int result = ::eventfd(0, EFD_CLOEXEC);
  if (result == -1) {
    throw_last_errno();
  }

  return squiz::linuxos::detail::file_handle::adopt(result);
}

squiz::linuxos::io_uring::setup_args
get_default_io_uring_args(std::uint32_t sq_size, std::uint32_t cq_size) {
  squiz::linuxos::io_uring::setup_args args;
  args.submission_queue_size = sq_size;
  args.completion_queue_size = cq_size;
  args.use_cooperative_task_scheduling = true;
  args.use_cooperative_task_scheduling_flag = true;
  return args;
}

}  // namespace

namespace squiz::linuxos {

// Pick some reasonable defaults for the
iouring_context::iouring_context()
  : iouring_context(get_default_io_uring_args(128, 512)) {
}

iouring_context::iouring_context(const io_uring::setup_args& io_uring_args)
  : io_uring_(io_uring_args)
  , max_pending_cqes_(
        io_uring_.supports_completion_queue_nodrop()
            ? std::numeric_limits<std::size_t>::max()
            : io_uring_.completion_queue_size())
  , reindex_timer_op_(*this)
  , timer_queue_(get_due_time(*this)) {
  // TODO: Current implementation relies on timed-wait
  // Need to rework to support earlier kernels that do not support this by
  // issuing OP_TIMEOUT operations.
  if (!io_uring_.supports_timed_wait()) {
    throw std::system_error(std::make_error_code(std::errc::not_supported));
  }

  LOG("create_eventfd");

  // Setup the eventfd for waking up the I/O thread, which should always
  // have a pending read outstanding if it is executing a 'wait' operation.
  wake_up_event_fd_ = create_eventfd();

  LOG("reset_timer_base");

  reset_timer_base();

  LOG("iouring_context ctor done");
}

iouring_context::~iouring_context() {
  LOG("dtor");
  timer_queue_.remove(&reindex_timer_op_);
  if (is_wake_up_read_submitted_) {
    wake_up_io_thread();
    while (is_wake_up_read_submitted_) {
      io_uring_.submit_and_wait(1);
      auto cqes = io_uring_.get_completion_entries();
      for (auto& cqe : cqes) {
        if (cqe.user_data == wake_up_user_data()) {
          LOG("got wake up read completion");
          is_wake_up_read_submitted_ = false;
        } else if (cqe.user_data == cancel_user_data()) {
          LOG("Ignoring OP_ASYNC_CANCEL completion");
        } else {
          LOG("Unexpected CQE (user_data=0x%" PRIx64 ")",
              static_cast<std::uint64_t>(cqe.user_data));
        }
      }
      io_uring_.completion_queue_advance(cqes.size());
    }
  }
}

bool iouring_context::is_on_io_thread() const noexcept {
  return std::this_thread::get_id() ==
      running_thread_id_.load(std::memory_order_relaxed);
}

void iouring_context::wake_up_io_thread() noexcept {
  LOG("wake_up_io_thread");

  const std::uint64_t count = 1;
retry:
  ssize_t result = ::write(wake_up_event_fd_.get_fd(), &count, sizeof(count));
  if (result < 0) {
    if (errno == EINTR) {
      // NOTE: Not sure if this will ever happen as a write to an eventfd
      // should always complete synchronously without blocking.
      goto retry;
    }
    // ignore other errors
  }
}

bool iouring_context::try_submit_wait_for_wake_up() noexcept {
  assert(!is_wake_up_read_submitted_);

  if (io_uring_.submission_queue_space_left() == 0) {
    LOG("no space for submitting wake-up - deferring until later");
    return false;
  }

  auto* sqe = io_uring_.get_next_submission_entry();
  io_uring::prepare_read_op(
      sqe,
      wake_up_event_fd_.get_fd(),
      0 /*offset*/,
      &wake_up_read_storage_,
      sizeof(wake_up_read_storage_),
      wake_up_user_data());
  io_uring_.enqueue_entry_for_submission(sqe);

  is_wake_up_read_submitted_ = true;

  LOG("enqueued wait for wakeup (user_data=0x%" PRIx64 ")",
      wake_up_user_data());

  return true;
}

namespace {

// Helper that sets the current thread id on construction and
// clears the current thread-id on destruction.
struct thread_id_tracker {
  thread_id_tracker(std::atomic<std::thread::id>& tid)
    : active_thread_id_(tid) {
    std::thread::id old_id;
    if (!active_thread_id_.compare_exchange_strong(
            old_id, std::this_thread::get_id(), std::memory_order_relaxed)) {
      // Some other thread was already running
      LOG("error: called  iouring_context::run() concurrently");
      throw std::logic_error(
          "Calling iouring_context::run() concurrently is not suported");
    }
  }

  ~thread_id_tracker() {
    LOG("exited run_impl");
    active_thread_id_.store(std::thread::id(), std::memory_order_relaxed);
  }

  thread_id_tracker(thread_id_tracker&&) = delete;

private:
  std::atomic<std::thread::id>& active_thread_id_;
};
}  // namespace

void iouring_context::run_impl(std::atomic<bool>& stop_requested) {
  LOG("run_impl");

  thread_id_tracker tid_tracker{running_thread_id_};

  while (!stop_requested.load(std::memory_order_relaxed)) {
    // Process a finite number of schedule-queue entries before falling
    // through and checking for other items.
    {
      LOG("processing ready queue");
      for (std::uint32_t i = 0; i < 100 && !ready_to_run_queue_.empty(); ++i) {
        LOG("dequeueing ready queue item");
        auto* item = ready_to_run_queue_.pop_front();
        LOG("executing ready queue item");
        item->execute(item);
      }
    }

    // Check if there are any time-based scheduled items.
    {
check_timers:
      auto current_time = monotonic_clock::now();
      auto current_timestamp = time_point_to_timestamp(current_time, false);

      LOG("dequeuing timers schedule before %li", current_timestamp);

      while (auto* item =
                 timer_queue_.try_dequeue_next_due_by(current_timestamp)) {
        LOG("dequeued item scheduled for %li",
            time_point_to_timestamp(
                static_cast<time_operation_base*>(item)->due_time, true));
        item->execute(item);
      }
    }

    // Check if there are any pending completion-queue entries.
    {
      LOG("dequeuing CQEs");

      auto cqes = io_uring_.get_completion_entries();
      LOG("there are %u CQEs available", cqes.size());

      for (auto& cqe : cqes) {
        LOG("processing CQE (user_data=0x%" PRIx64 ")",
            static_cast<std::uint64_t>(cqe.user_data));
        if (cqe.user_data == wake_up_user_data()) {
          LOG("got wake-up signal");
          is_wake_up_read_submitted_ = false;
        } else if (cqe.user_data == cancel_user_data()) {
          LOG("cancel operation completed");
        } else {
          LOG("got iouring_operation_base completion (user_data=0x%" PRIx64
              ", res=%i, "
              "flags=%u)",
              static_cast<std::uint64_t>(cqe.user_data),
              cqe.result,
              cqe.flags);

          if (cqe.user_data != 0) {
            auto* op = reinterpret_cast<operation_base*>(
                static_cast<std::uintptr_t>(cqe.user_data));
            op->handle_completion(op, cqe.result, cqe.flags);
          }
        }
      }
      LOG("marking %u entries in completion queue consumed", cqes.size());
      io_uring_.completion_queue_advance(cqes.size());
    }

    // Only check the remote queue if we haven't issued a
    // read on the eventfd. Otherwise we'll just wait until we receive
    // the completion-event that indicates someone has posted something
    // to the remote queue.
    if (!is_wake_up_read_submitted_) {
      acquire_remote_items();
      execute_remote_stop_items();
    }

    std::error_code ec;

    if (ready_to_run_queue_.empty()) {
      if (is_wake_up_read_submitted_) {
        // Nothing else to do until an event arrives, or until the
        // next scheduled time elapses.
        LOG("submit_and_wait_until()");
        assert(io_uring_.supports_timed_wait());
        ec = io_uring_.submit_and_wait_until(get_earliest_timer());
      } else {
        // Haven't signalled that remote threads should try to wake this
        // thread up yet, so don't do any syscalls that will block here.
        if (io_uring_.submission_queue_unsubmitted() > 0) {
          // Need to make a syscall to submit pending SQEs

          // Submit new items and get new pending events without waiting.
          LOG("submit_and_get_events()");
          ec = io_uring_.submit_and_get_events();
        } else {
          // Only need to poll for new events, no new operations to submit
          LOG("get_events()");
          ec = io_uring_.get_events();
        }

        if (!ec && io_uring_.completion_queue_available() == 0) {
          // Still no completion events.
          // Try to mark the remote queue as 'inactive' and submit a read
          // request on the eventfd that will be triggered when a remote
          // thread enqueues something.
          // Will go around the loop again and check for new work and if
          // none is found then we'll go through to the
          // 'submit_and_wait_until()' code-path.
          if (io_uring_.submission_queue_space_left() > 0) {
            if (remote_queue_.is_inactive() ||
                remote_queue_.try_mark_inactive()) {
              if (remote_stop_queue_.is_inactive() ||
                  remote_stop_queue_.try_mark_inactive()) {
                LOG("marked remote queues inactive");
                [[maybe_unused]] bool ok = try_submit_wait_for_wake_up();
                assert(ok);
              }
            }
          }
        }
      }
    } else {
      // Still things to do in the queue, just submit the operations
      // we have (if any) and go around the loop again.
      LOG("submit");
      ec = io_uring_.submit();
    }

    if (ec) {
      if (ec.value() == ETIME) {
        goto check_timers;
      } else if (
          ec == std::errc::interrupted ||
          ec == std::errc::device_or_resource_busy ||
          ec == std::errc::resource_unavailable_try_again) {
        continue;
      } else {
        std::system_error err{ec};
        LOG("error occurred: %i, %s", ec.value(), err.what());
        throw std::move(err);
      }
    }
  }  // end of while()
}

void iouring_context::schedule_now(operation_base* op) noexcept {
  if (is_on_io_thread()) {
    schedule_now_local(op);
  } else {
    schedule_now_remote(op);
  }
}

void iouring_context::schedule_now_local(operation_base* op) noexcept {
  LOG("schedule_now_local");
  ready_to_run_queue_.push_back(op);
}

void iouring_context::schedule_now_remote(operation_base* op) noexcept {
  LOG("schedule_now_remote");
  bool needs_wake_up = remote_queue_.enqueue(op);
  if (needs_wake_up) {
    wake_up_io_thread();
  }
}

void iouring_context::remove_local(operation_base* op) noexcept {
  LOG("remove_local");
  assert(is_on_io_thread());
  ready_to_run_queue_.remove(op);
}

void iouring_context::schedule_at_local(time_operation_base* op) noexcept {
  assert(is_on_io_thread());
  LOG("schedule_at_local (ts=%lu)",
      time_point_to_timestamp(op->due_time, true));
  timer_queue_.add(op);
}

void iouring_context::remove_time_op_local(time_operation_base* op) noexcept {
  LOG("remove_time_op_local");
  assert(is_on_io_thread());
  timer_queue_.remove(op);
}

void iouring_context::acquire_remote_items() noexcept {
  LOG("acquire_remote_items");
  assert(is_on_io_thread());
  if (!remote_queue_.is_inactive()) {
    auto remote_items = remote_queue_.dequeue_all_reversed();
    operation_list remote_list;
    while (!remote_items.empty()) {
      LOG("appending remote item to ready-to-run queue");
      remote_list.push_front(remote_items.pop_front());
    }
    ready_to_run_queue_.append(std::move(remote_list));
  }
}

void iouring_context::execute_remote_stop_items() noexcept {
  LOG("execute_remote_stop_items");
  assert(is_on_io_thread());
  if (!remote_stop_queue_.is_inactive()) {
    auto remote_items = remote_stop_queue_.dequeue_all_reversed();
    while (!remote_items.empty()) {
      LOG("executing remote_stop item");
      stoppable_operation_base* item = remote_items.pop_front();
      item->execute_stop(item);
    }
  }
}

void iouring_context::schedule_stop_request_remote(
    stoppable_operation_base* op) noexcept {
  LOG("schedule_stop_request");
  bool needs_wakeup = remote_stop_queue_.enqueue(op);
  if (needs_wakeup) {
    wake_up_io_thread();
  }
}

std::uint32_t iouring_context::available_sqe_space() const noexcept {
  if (!waiting_for_sqes_queue_.empty() ||
      !waiting_for_cancel_sqes_queue_.empty()) {
    // Other operations are already queued waiting for SQE space.
    LOG("available_sqe_space: already operations waiting for SQE space");
    return 0;
  }

  const std::uint32_t sqe_space_left = io_uring_.submission_queue_space_left();
  const std::size_t cqes_avail = max_pending_cqes_ - pending_cqes_;

  LOG("available_sqe_space: CQE avail: %zu, SQE avail: %u",
      cqes_avail,
      sqe_space_left);

  if (cqes_avail < sqe_space_left) {
    return static_cast<std::uint32_t>(cqes_avail);
  } else {
    return sqe_space_left;
  }
}

bool iouring_context::try_submit_cancel_for(std::uint64_t user_data) noexcept {
  if (io_uring_.submission_queue_space_left() == 0 ||
      pending_cqes_ >= max_pending_cqes_) {
    return false;
  }

  submit_cancel_for(user_data);
  return true;
}

void iouring_context::submit_cancel_for(std::uint64_t user_data) noexcept {
  auto* sqe = io_uring_.get_next_submission_entry();
  io_uring::prepare_cancel_op(sqe, user_data, cancel_user_data());
  io_uring_.enqueue_entry_for_submission(sqe);
}

void iouring_context::schedule_when_can_submit_more_sqes(
    operation_base* op) noexcept {
  assert(is_on_io_thread());
  waiting_for_sqes_queue_.push_back(op);
}

void iouring_context::schedule_async_cancel_when_can_submit_more_sqes(
    stoppable_operation_base* op) noexcept {
  assert(is_on_io_thread());
  waiting_for_cancel_sqes_queue_.push_back(op);
}

void iouring_context::execute_pending_sqe_submission_tasks() noexcept {
  if (waiting_for_sqes_queue_.empty() && waiting_for_cancel_sqes_queue_.empty())
      [[likely]] {
    return;
  }

  const std::size_t available_cqe_slots = max_pending_cqes_ - pending_cqes_;
  const std::uint32_t available_sqe_slots =
      io_uring_.submission_queue_space_left();

  const std::uint32_t max_sqes = available_sqe_slots < available_cqe_slots
      ? available_sqe_slots
      : static_cast<std::uint32_t>(available_cqe_slots);
  if (max_sqes == 0) {
    return;
  }

  const std::uint32_t sq_tail_limit =
      io_uring_.submission_queue_tail_position() + max_sqes;

  std::uint32_t sqes_avail = max_sqes;

  // Prioritize processing cancellations before starting new operations
  // as this is likely to free up more pending CQE slots.
  while (!waiting_for_cancel_sqes_queue_.empty()) {
    auto* item = waiting_for_cancel_sqes_queue_.pop_front();
    const std::uint32_t sqes_needed =
        item->submit_cancel_sqes(item, sqes_avail);
    if (sqes_needed > 0) {
      waiting_for_cancel_sqes_queue_.push_front(item);
      return;
    }
    sqes_avail = sq_tail_limit - io_uring_.submission_queue_tail_position();
    if (sqes_avail == 0) {
      return;
    }
  }

  while (!waiting_for_sqes_queue_.empty()) {
    auto* item = waiting_for_sqes_queue_.pop_front();
    const std::uint32_t sqes_needed = item->submit_sqes(item, sqes_avail);
    if (sqes_needed > 0) {
      waiting_for_sqes_queue_.push_front(item);
      return;
    }
    sqes_avail = sq_tail_limit - io_uring_.submission_queue_tail_position();
    if (sqes_avail == 0) {
      return;
    }
  }
}

std::uint64_t iouring_context::time_point_to_timestamp(
    const monotonic_clock::time_point& tp, bool round_up) const noexcept {
  if (tp.seconds_part() < timer_base_time_seconds_) [[unlikely]] {
    return 0;
  }

  if (tp.seconds_part() >= timer_max_time_seconds_) [[unlikely]] {
    return max_timestamp;
  }

  auto diff_seconds =
      std::chrono::seconds(tp.seconds_part() - timer_base_time_seconds_);
  auto diff_nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(diff_seconds) +
      std::chrono::nanoseconds(tp.nanoseconds_part());

  if (round_up) {
    auto result =
        std::chrono::ceil<timer_granularity>(diff_nanoseconds).count();
    LOG("time_point_to_timestamp: rounded up %lli ns to %li us",
        diff_nanoseconds.count(),
        result);
    return result;
  } else {
    auto result =
        std::chrono::floor<timer_granularity>(diff_nanoseconds).count();
    LOG("time_point_to_timestamp: rounded down %lli ns to %li us",
        diff_nanoseconds.count(),
        result);
    return result;
  }
}

void iouring_context::reset_timer_base() noexcept {
  constexpr auto max_timestamp_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          timer_granularity(max_timestamp));

  constexpr auto half_max_timestamp_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          timer_granularity(max_timestamp / 2));

  auto current_time = monotonic_clock::now();

  // Allow some efficient scheduling of items in the recent past in case
  // some tasks continue scheduling work at their submission time as a form of
  // prioritising work that started earler.
  constexpr std::int64_t base_seconds_in_past = 300;

  timer_base_time_seconds_ = std::sub_sat<std::int64_t>(
      current_time.seconds_part(), base_seconds_in_past);

  timer_max_time_seconds_ = std::add_sat<std::int64_t>(
      timer_base_time_seconds_, max_timestamp_seconds.count());

  LOG("new time (base = %li, max = %li)",
      timer_base_time_seconds_,
      timer_max_time_seconds_);

  auto reindex_time_seconds = std::add_sat<std::int64_t>(
      timer_base_time_seconds_, half_max_timestamp_seconds.count());
  reindex_timer_op_.due_time =
      monotonic_clock::time_point::from_seconds_and_nanoseconds(
          reindex_time_seconds, 0);

  LOG("reindex_items");

  timer_queue_.reindex_items();

  LOG("add reindex_timer_op_");

  // Add/Re-add the queue item that schedules the next reindexing.
  timer_queue_.add(&reindex_timer_op_);
}

monotonic_clock::time_point
iouring_context::get_earliest_timer() const noexcept {
  std::uint64_t timestamp = timer_queue_.earliest_due_time_lower_bound();
  auto rel_time = timer_granularity(timestamp);
  return monotonic_clock::time_point::from_seconds_and_nanoseconds(
             timer_base_time_seconds_, 0) +
      rel_time;
}

}  // namespace squiz::linuxos
