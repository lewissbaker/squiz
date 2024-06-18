///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/linuxos/detail/file_handle.hpp>
#include <squiz/linuxos/detail/mmap_region.hpp>
#include <squiz/linuxos/monotonic_clock.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>

#include <linux/time_types.h>  // for __kernel_timespec
#include <time.h>              // for clockid_t

struct io_uring_sqe;
struct io_uring_cqe;

namespace squiz::linuxos {

class io_uring {
public:
  struct setup_args;

  /// Create a new io_uring object using the specified setup arguments.
  ///
  /// \throw std::system_error
  /// If the initialization of the io_uring object fails for any reason.
  explicit io_uring(const setup_args& args);

  /// Move constructor.
  io_uring(io_uring&& other) noexcept;

  ~io_uring();

  //////////////////////////////////
  // Feature Queries

  /// Query whether this io_uring implementation supports the \c
  /// sumbit_and_wait_until() or \c submit_and_wait_for() functions.
  ///
  /// If this returns \c false then you must instead submit an \c OP_TIMEOUT
  /// operation using the \c prepare_timeout_op() function to perform time-based
  /// waiting.
  bool supports_timed_wait() const noexcept;

  /// Query whether this io_uring implementation supports effectively unbounded
  /// numbers of pending completion-queue entries without dropping completion
  /// events.
  ///
  /// If this is \c false, then an application should generally avoid submitting
  /// more operations with pending CQEs than there is space in the
  /// completion-queue buffer. If this is \c true, then the kernel will buffer
  /// additional completion-queue entries internally and move them to the
  /// completion-queue buffer once the application has freed up space in the
  /// completion-queue.
  bool supports_completion_queue_nodrop() const noexcept;

  //////////////////////////////////
  // Submission queue
  //
  // Only a single thread may call these methods at a time.

  /// Query the number of entries in the submission queue.
  ///
  /// This will be a power-of-two.
  [[nodiscard]] std::uint32_t submission_queue_size() const noexcept {
    return sq_entry_count_;
  }

  /// Query the number of available submission queue slots.
  ///
  /// This will never be greater than \c submission_queue_size().
  [[nodiscard]] std::uint32_t submission_queue_space_left() const noexcept {
    return submission_queue_size() - submission_queue_unsubmitted();
  }

  /// Query the number of items in the submission queue which have not
  /// yet been submitted.
  [[nodiscard]] std::uint32_t submission_queue_unsubmitted() const noexcept;

  /// Query the number of submission-queue items that were dropped because
  /// they were invalid.
  [[nodiscard]] std::uint32_t submission_queue_dropped() const noexcept;

  /// The current position of the tail index.
  ///
  /// This is the index of the next SQE to write to.
  [[nodiscard]] std::uint32_t submission_queue_tail_position() const noexcept {
    return sqe_tail_;
  }

  /// The maximum value that the tail position can be advanced to before the
  /// submission queue has no space left.
  [[nodiscard]] std::uint32_t submission_queue_tail_limit() const noexcept;

  /// Enqueue the specified SQE to the ring-buffer.
  ///
  /// This must be the pointer that was retrieved from
  /// \c get_next_submission_entry() which was populated by one of the calls to
  /// \c prepare_*().
  void enqueue_entry_for_submission(io_uring_sqe* sqe) noexcept;

  /// Publishes any enqueued SQEs that have not yet been published to the
  /// kernel side of the ring-buffer.
  ///
  /// \return
  /// The number of entries that have not yet been consumed by the kernel.
  /// This can be used to determine whether or not a syscall is required to
  /// submit the entries.
  std::uint32_t flush_submission_queue() noexcept;

  /// Wait until there is at least one free slot in the submission-queue.
  ///
  /// \return
  /// A zero error-code if successful, otherwise returns an error code
  /// indicating why space was not available.
  std::error_code wait_for_submission_queue_space() noexcept;

  ////////////////////////////////////
  // Completion queue
  //
  // Only one thread may access the completion queue methods at a time.

  /// Query the total size of the completion-queue.
  [[nodiscard]] std::uint32_t completion_queue_size() const noexcept {
    return cq_entry_count_;
  }

  /// Query the number of entries in the completion-queue that are
  /// ready to be processed.
  [[nodiscard]] std::uint32_t completion_queue_available() const noexcept;

  /// Query the number of dropped completion-events that occurred due to
  /// insufficient space in the completion-queue.
  [[nodiscard]] std::uint32_t completion_queue_overflow() const noexcept;

  /// Mark \c count entries of the completion queue as consumed.
  ///
  /// This advances the completion-queue head by \c count, freeing the space
  /// for those entries to be reused by new completion-events.
  void completion_queue_advance(std::uint32_t count) noexcept;

  // This struct has same layout as io_uring_cqe.
  // Define our own here so we don't have to include <linux/io_uring.h>
  // in the public header.
  struct completion_entry {
    std::uint64_t user_data;
    std::int32_t result;
    std::uint32_t flags;
  };

  class completion_queue_view {
  public:
    completion_queue_view() noexcept
      : entries_(nullptr)
      , mask_(0)
      , head_(0)
      , count_(0) {}

    std::uint32_t size() const noexcept { return count_; }

    const completion_entry& operator[](std::uint32_t index) const noexcept {
      assert(index < size());
      return entries_[(head_ + index) & mask_];
    }

    class iterator {
    public:
      using value_type = completion_entry;
      using reference = const completion_entry&;
      using pointer = const completion_entry*;
      using difference_type = std::int32_t;
      using iterator_category = std::random_access_iterator_tag;

      iterator() noexcept : entries_(nullptr), mask_(0), index_(0) {}

      reference operator*() const noexcept { return entries_[index_ & mask_]; }

      pointer operator->() const noexcept {
        return entries_ + (index_ & mask_);
      }

      iterator& operator++() noexcept {
        ++index_;
        return *this;
      }

      iterator operator++(int) noexcept {
        iterator tmp = *this;
        operator++();
        return tmp;
      }

      iterator& operator--() noexcept {
        --index_;
        return *this;
      }

      iterator operator--(int) noexcept {
        iterator tmp = *this;
        operator--();
        return tmp;
      }

      iterator& operator+=(difference_type offset) noexcept {
        index_ += offset;
        return *this;
      }

      iterator& operator-=(difference_type offset) noexcept {
        index_ -= offset;
        return *this;
      }

      friend iterator
      operator+(const iterator& a, difference_type offset) noexcept {
        iterator tmp = a;
        tmp += offset;
        return tmp;
      }

      friend difference_type
      operator-(const iterator& a, const iterator& b) noexcept {
        return static_cast<difference_type>(a.index_ - b.index_);
      }

      friend bool operator==(const iterator& a, const iterator& b) noexcept {
        return a.index_ == b.index_;
      }

    private:
      friend completion_queue_view;
      iterator(
          const completion_entry* entries,
          std::uint32_t mask,
          std::uint32_t index) noexcept
        : entries_(entries)
        , mask_(mask)
        , index_(index) {}

      const completion_entry* entries_;
      std::uint32_t mask_;
      std::uint32_t index_;
    };

    // Provide iterator-based access so we can use range-based for-loops
    iterator begin() const noexcept { return iterator{entries_, mask_, head_}; }
    iterator end() const noexcept {
      return iterator{entries_, mask_, head_ + count_};
    }

  private:
    friend io_uring;

    const completion_entry* entries_;
    std::uint32_t mask_;
    std::uint32_t head_;
    std::uint32_t count_;
  };

  /// Get a snapshot of the available completion-entries.
  ///
  /// Subsequent calls to this method are not guaranteed to return the same
  /// view as additional items may be added to the completion-queue by the
  /// kernel concurrently.
  ///
  /// The returned view is valid until either the io_uring object is
  /// destroyed or the completion_queue_advance() method is called.
  ///
  /// The size of the view will never be larger than completion_queue_size().
  completion_queue_view get_completion_entries() const noexcept;

  ///////////////////////////////////////////////////////
  // Request submission / Completion event retrieval
  //

  /// Submit any enqueued-but-not-yet-submitted entries to the kernel.
  ///
  /// Don't wait for or poll for any completion-events.
  std::error_code submit() noexcept;

  /// Poll for new completion-events but don't wait for any new ones.
  std::error_code get_events() noexcept;

  /// Submit any unsubmitted requests on the submission queue and
  /// retrieve any completion-events.
  ///
  /// Does not block waiting for completion-events if there are no
  /// completion-events ready.
  ///
  /// \return
  /// An zero error-code if the operation was successful, otherwise
  /// returns an error-code indicating the failure.
  std::error_code submit_and_get_events() noexcept;

  /// Submit any unsubmitted requests on the submission-queue and
  /// wait until at least one completion-event is in the completion-queue.
  std::error_code submit_and_wait(std::uint32_t min_complete = 1) noexcept;

  /// Submit any unsubmitted requests on the submission-queue and wait
  /// until either a timeout elapses or until at least one completion-event
  /// is in the completion-queue.
  ///
  /// \returns
  /// ENOTSUP if this io_uring implementation does not support timed waits.
  std::error_code
  submit_and_wait_until(monotonic_clock::time_point timeout) noexcept;
  std::error_code submit_and_wait_until(
      std::uint32_t min_complete, monotonic_clock::time_point timeout) noexcept;

  /// Submit any unsubmitted requests on the submission-queue and wait until
  /// either a timeout elapses or until at least one completion-event is in
  /// the completion-queue.
  std::error_code
  submit_and_wait_for(std::chrono::nanoseconds timeout) noexcept;
  std::error_code submit_and_wait_for(
      std::uint32_t min_complete, std::chrono::nanoseconds timeout) noexcept;

  /////////////////////////////////////////////////
  /// Submission queue entries

  /// Get a pointer to the next submission entry to fill out.
  ///
  /// The caller must populate this SQE by calling one of the prepare_*()
  /// methods before then passing it to \c enqueue_entry_for_submission().
  ///
  /// \pre
  /// Requires that submission_queue_space_left() is non-zero.
  io_uring_sqe* get_next_submission_entry() noexcept;

  /// Prepare an SQE with an IORING_OP_READ operation.
  static void prepare_read_op(
      io_uring_sqe* sqe,
      int fd,
      std::uint64_t offset,
      void* buf,
      std::uint32_t sz,
      std::uint64_t user_data) noexcept;

  /// Prepare an SQE with an IORING_OP_WRITE operation.
  static void prepare_write_op(
      io_uring_sqe* sqe,
      int fd,
      std::uint64_t offset,
      const void* buf,
      std::uint32_t sz,
      std::uint64_t user_data) noexcept;

  /// Prepare an SQE with an IORING_OP_CLOSE operation.
  static void
  prepare_close_op(io_uring_sqe* sqe, int fd, std::uint64_t user_data) noexcept;

  /// Prepare an SQE with an IORING_OP_ASYNC_CANCEL operation.
  static void prepare_cancel_op(
      io_uring_sqe* sqe,
      std::uint64_t user_data_to_cancel,
      std::uint64_t user_data) noexcept;

  /// Prepare an SQE with an IORING_OP_OPENAT  operation.
  static void prepare_openat_op(
      io_uring_sqe* sqe,
      int dirfd,
      const char* path,
      std::uint32_t open_flags,
      mode_t mode,
      std::uint64_t user_data) noexcept;

  /// Prepare an SQE with an IORING_OP_TIMEOUT operation.
  ///
  /// This operation will complete with -ETIME if the timeout
  /// completed through expiration of the timeout, or 0 if
  /// the timeout completed through \c min_requests completing
  /// on their own.
  static void prepare_timeout_op(
      io_uring_sqe* sqe,
      std::uint32_t min_complete,
      const __kernel_timespec* timeout,
      clockid_t clock_id,
      bool is_absolute_time,
      std::uint64_t user_data) noexcept;

private:
  std::uint32_t compute_sq_wakeup_flag() const noexcept;

  // fields relating to iouring as a whole

  alignas(64) detail::file_handle fd_;
  std::uint32_t setup_flags_;
  std::uint32_t available_features_;
  std::uint32_t pad1_;
  detail::mmap_region cq_map_;
  detail::mmap_region sq_map_;
  detail::mmap_region sqe_map_;

  // submission-queue fields

  alignas(64) std::uint32_t sq_entry_count_;
  std::uint32_t sq_mask_;
  /// Index of first unflushed sqe
  std::uint32_t sqe_head_;
  /// Index of last unflushed sqe
  std::uint32_t sqe_tail_;
  io_uring_sqe* sq_entries_;
  std::uint32_t* sq_index_array_;
  volatile const std::uint32_t* sq_head_;
  std::uint32_t* sq_tail_;
  volatile std::uint32_t* sq_flags_;
  volatile std::uint32_t* sq_dropped_;

  // completion-queue fields

  alignas(64) std::uint32_t cq_entry_count_;
  std::uint32_t cq_mask_;
  volatile io_uring_cqe* cq_entries_;
  std::uint32_t* cq_head_;
  const volatile std::uint32_t* cq_tail_;
  const volatile std::uint32_t* cq_overflow_;
};

struct io_uring::setup_args {
  /// If set to non-null then passes the IORING_SETUP_ATTACH_WQ flag
  /// and configures the new io_uring object to use the same async worker
  /// thread backend as the pointed-to io_uring object.
  io_uring* share_work_queue_with = nullptr;

  /// The number of entries for the submission queue. This puts a bound on the
  /// number of requests that can be submitted with a single
  /// kernel-transition.
  ///
  /// Must be a power-of-two.
  ///
  /// If larger than IORING_MAX_ENTRIES then value will be clamped to
  /// IORING_MAX_ENTRIES.
  std::optional<std::uint32_t> submission_queue_size;

  /// Set a completion-queue size which differs form the submission-queue
  /// size.
  ///
  /// If unset then the completion-queue size will be the same size as the
  /// submission-queue-size.
  ///
  /// The value must be a power-of-two.
  ///
  /// If larger than IORING_MAX_CQ_ENTRIES then the value is clamped to
  /// IORING_MAX_CQ_ENTRIES.
  std::optional<std::uint32_t> completion_queue_size;

  /// If set then enables kernel-side polling of the submission-queue.
  ///
  /// The value is the number of milliseconds the kernel-thread will continue
  /// to poll for new submissions before going to sleep. Once it goes to
  /// sleep, the kernel-thread will need to be woken up to process any new
  /// items that are subsequently enqueued. This means that even if
  /// submission-queue polling is enabled, you need to periodically call one
  /// of the submit() functions to ensure that any enqueued submission
  /// requests are flushed to the kernel.
  std::optional<std::chrono::milliseconds> submission_queue_thread_idle;

  /// If kernel-side SQ polling is enabled by setting
  /// submission_queue_thread_idle, then
  std::optional<std::uint32_t> submission_queue_thread_cpu;

  /// If true then sets the IORING_SETUP_SUBMIT_ALL flag.
  ///
  /// Submit operations submit all pending submission entries even if one
  /// entry fails submission synchronously. By default a submission call will
  /// stop if one of the submissions fails.
  bool submit_all : 1 = false;

  /// If true then sets the IORING_SETUP_COOP_TASKRUN flag.
  ///
  /// This disables the active interruption of a userspace task when
  /// a completion event arrives in order to process the completion.
  /// If this is set to true then processing of the completions will
  /// wait until the next kernel/user transition. This means that you
  /// cannot just poll the completion-queue for completion-events as
  /// the 'cq_tail' field will not be updated until a syscall is made.
  bool use_cooperative_task_scheduling : 1 = false;

  /// If set, then the kernel signals that there are completion-queue entries
  /// that need to be processed by setting a flag in the submission-queue's
  /// 'flags' field.
  ///
  /// If set to \c true then requires that \c use_cooperative_task_scheduling
  /// is also set to \c true. This can avoid needing to make a call to the
  /// kernel if there are no pending completion-events.
  bool use_cooperative_task_scheduling_flag : 1 = false;

  /// If true then completion-events are not processed until a thread calls
  /// one of the 'wait()' functions that waits for new completion-events.
  ///
  /// By default, completion-tasks will be processed at each kernel/user
  /// transition, which can delay other user-code from running.
  ///
  /// Requires that the \c single_issue_thread field is also set to \c true.
  bool defer_tasks_until_get_events : 1 = false;

  /// A hint that only the thread that created the io_uring object will be
  /// submitting new entries.
  ///
  /// Note valid for use with submission-queue polling, so must not be set
  /// if you have set \c submission_queue_thread_idle.
  bool single_issuer_thread : 1 = false;

  /// If set to true then sets the IORING_SETUP_IOPOLL flag and performs
  /// busy-waiting for I/O completion rather than getting notification via
  /// an asynchronous interrupt-request.
  ///
  /// This can reduce latency but may consume more CPU resources than
  /// interrupt-driven I/O.
  ///
  /// This limits the use of the io_uring object to file-systems and
  /// block devices that support polling and that were opened with the
  /// O_DIRECT flag.
  ///
  /// One of the wait() operations must be called to drive I/O completions
  /// if I/O polling is enabled.
  bool use_io_polling : 1 = false;
};

}  // namespace squiz::linuxos
