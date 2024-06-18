///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/linuxos/io_uring.hpp>

#include <atomic>
#include <cerrno>
#include <system_error>

#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <sys/mman.h>
#include <unistd.h>

// HACK:

#if 0
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

// Constants taken from linux/tools/include/uapi/asm-generic/unistd.h
#define __NR_io_uring_setup 425
#define __NR_io_uring_enter 426
#define __NR_io_uring_register 427

#ifndef IORING_SETUP_ATTACH_WQ
#  define IORING_SETUP_ATTACH_WQ (1U << 5)
#endif

#ifndef IORING_SETUP_SUBMIT_ALL
#  define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif

#ifndef IORING_SETUP_COOP_TASKRUN
#  define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif

#ifndef IORING_SETUP_TASKRUN_FLAG
#  define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif

#ifndef IORING_SETUP_SINGLE_ISSUER
#  define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif

#ifndef IORING_SETUP_DEFER_TASKRUN
#  define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif

#ifndef IORING_SETUP_NO_SQARRAY
#  define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif

#ifndef IORING_SQ_TASKRUN
#  define IORING_SQ_TASKRUN (1U << 2)
#endif

namespace squiz::linuxos {

static constexpr std::uint32_t default_submission_queue_size = 128;

namespace syscalls {

int io_uring_register(
    int fd, unsigned opcode, const void* arg, unsigned nr_args) noexcept {
  int ret = syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
  return (ret == -1) ? -errno : ret;
}

int io_uring_setup(unsigned entries, struct io_uring_params* p) noexcept {
  int ret = syscall(__NR_io_uring_setup, entries, p);
  return (ret == -1) ? -errno : ret;
}

int io_uring_enter2(
    int fd,
    unsigned to_submit,
    unsigned min_complete,
    unsigned flags,
    void* arg,
    size_t arg_sz) noexcept {
  int ret = syscall(
      __NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, arg_sz);
  return (ret == -1) ? -errno : ret;
}

}  // namespace syscalls

[[noreturn]] static void throw_last_errno() {
  int error_code = errno;
  throw std::system_error(std::error_code(error_code, std::system_category()));
}

io_uring::io_uring(const setup_args& args) {
  io_uring_params params;
  std::memset(&params, 0, sizeof(params));

  params.flags |= IORING_SETUP_CLAMP;

  const std::uint32_t submission_queue_size =
      args.submission_queue_size.value_or(default_submission_queue_size);

  if (args.completion_queue_size.has_value()) {
    params.cq_entries = args.completion_queue_size.value();
    params.flags |= IORING_SETUP_CQSIZE;
  }

  if (args.share_work_queue_with != nullptr) {
    params.flags |= IORING_SETUP_ATTACH_WQ;
    params.wq_fd = args.share_work_queue_with->fd_.get_fd();
  }

  if (args.submission_queue_thread_idle.has_value()) {
    params.flags |= IORING_SETUP_SQPOLL;
    auto time_in_ms = args.submission_queue_thread_idle->count();
    if (time_in_ms > std::numeric_limits<std::uint32_t>::max()) {
      params.sq_thread_idle = std::numeric_limits<std::uint32_t>::max();
    } else if (time_in_ms <= 0) {
      params.sq_thread_idle = 0;
    } else {
      params.sq_thread_idle = static_cast<std::uint32_t>(time_in_ms);
    }
  }

  if (args.submission_queue_thread_cpu.has_value()) {
    assert(args.submission_queue_thread_idle.has_value());
    params.flags |= IORING_SETUP_SQ_AFF;
    params.sq_thread_cpu = args.submission_queue_thread_cpu.value();
  }

  if (args.use_io_polling) {
    params.flags |= IORING_SETUP_IOPOLL;
  }

  if (args.submit_all) {
    params.flags |= IORING_SETUP_SUBMIT_ALL;
  }

  if (args.use_cooperative_task_scheduling) {
    params.flags |= IORING_SETUP_COOP_TASKRUN;
  }

  if (args.use_cooperative_task_scheduling_flag) {
    assert(args.use_cooperative_task_scheduling);
    params.flags |= IORING_SETUP_TASKRUN_FLAG;
  }

  if (args.single_issuer_thread) {
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
  }

  if (args.defer_tasks_until_get_events) {
    assert(args.single_issuer_thread);
    params.flags |= IORING_SETUP_DEFER_TASKRUN;
  }

  LOG("io_uring_setup");

  int result = syscalls::io_uring_setup(submission_queue_size, &params);
  if (result == -1) {
    int error_code = -result;
    throw std::system_error(
        std::error_code(error_code, std::system_category()));
  }

  fd_ = detail::file_handle::adopt(result);

  setup_flags_ = params.flags;
  available_features_ = params.features;

  const std::size_t sq_array_size = params.sq_entries * sizeof(std::uint32_t);
  const std::size_t sqe_buffer_size = params.sq_entries * sizeof(io_uring_sqe);
  std::size_t sq_ring_size = params.sq_off.array + sq_array_size;
  std::size_t cq_ring_size =
      params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);

  const bool use_single_map_for_cq_and_sq_regions =
      (available_features_ & IORING_FEAT_SINGLE_MMAP) != 0;
  if (use_single_map_for_cq_and_sq_regions) {
    sq_ring_size = std::max(sq_ring_size, cq_ring_size);
    cq_ring_size = sq_ring_size;
  }

  detail::mmap_region cq_map;
  detail::mmap_region sq_map;
  detail::mmap_region sqe_map;

  LOG("mmap 1");

  // Map the submission queue index region
  {
    void* ptr = ::mmap(
        0,
        sq_ring_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        fd_.get_fd(),
        IORING_OFF_SQ_RING);
    if (ptr == nullptr) {
      throw_last_errno();
    }

    sq_map_ = detail::mmap_region::adopt(ptr, sq_ring_size);
  }

  // Map the completion-queue ring buffer region (if necessary)
  if (!use_single_map_for_cq_and_sq_regions) {
    LOG("mmap 2");

    // Need to map the completion queue ring separately.
    void* ptr = ::mmap(
        0,
        cq_ring_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        fd_.get_fd(),
        IORING_OFF_CQ_RING);
    if (ptr == nullptr) {
      throw_last_errno();
    }

    cq_map_ = detail::mmap_region::adopt(ptr, cq_ring_size);
  }

  LOG("mmap 3");

  // Map the submission-queue entry region
  {
    void* ptr = ::mmap(
        0,
        sqe_buffer_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        fd_.get_fd(),
        IORING_OFF_SQES);
    if (ptr == nullptr) {
      throw_last_errno();
    }

    sqe_map_ = detail::mmap_region::adopt(ptr, sqe_buffer_size);
  }

  LOG("member init");

  // Setup the submission-queue pointers
  {
    unsigned char* char_ptr = reinterpret_cast<unsigned char*>(sq_map_.data());

    sq_entry_count_ = params.sq_entries;
    sqe_tail_ = 0;
    sqe_head_ = 0;
    sq_mask_ =
        *reinterpret_cast<std::uint32_t*>(char_ptr + params.sq_off.ring_mask);
    sq_index_array_ =
        reinterpret_cast<std::uint32_t*>(char_ptr + params.sq_off.array);
    sq_head_ = reinterpret_cast<std::uint32_t*>(char_ptr + params.sq_off.head);
    sq_tail_ = reinterpret_cast<std::uint32_t*>(char_ptr + params.sq_off.tail);
    sq_flags_ =
        reinterpret_cast<std::uint32_t*>(char_ptr + params.sq_off.flags);
    sq_dropped_ =
        reinterpret_cast<std::uint32_t*>(char_ptr + params.sq_off.dropped);
  }

  // Setup the submission-queue-entry pointer
  {
    sq_entries_ = reinterpret_cast<io_uring_sqe*>(sqe_map_.data());
  }

  // Setup the completion-queue pointers
  {
    void* ptr =
        use_single_map_for_cq_and_sq_regions ? sq_map_.data() : cq_map_.data();
    unsigned char* char_ptr = reinterpret_cast<unsigned char*>(ptr);

    cq_entry_count_ = params.cq_entries;
    cq_mask_ = *reinterpret_cast<const std::uint32_t*>(
        char_ptr + params.cq_off.ring_mask);
    cq_entries_ =
        reinterpret_cast<io_uring_cqe*>(char_ptr + params.cq_off.cqes);
    cq_head_ = reinterpret_cast<std::uint32_t*>(char_ptr + params.cq_off.head);
    cq_tail_ = reinterpret_cast<std::uint32_t*>(char_ptr + params.cq_off.tail);
    cq_overflow_ =
        reinterpret_cast<std::uint32_t*>(char_ptr + params.cq_off.overflow);
  }

  LOG("ctor done");
}

io_uring::io_uring(io_uring&& other) noexcept
  : fd_(std::move(other.fd_))
  , setup_flags_(std::exchange(other.setup_flags_, 0))
  , available_features_(std::exchange(other.available_features_, 0))
  , cq_map_(std::move(other.cq_map_))
  , sq_map_(std::move(other.sq_map_))
  , sqe_map_(std::move(other.sqe_map_))
  , sq_entry_count_(std::exchange(other.sq_entry_count_, 0))
  , sq_mask_(std::exchange(other.sq_mask_, 0))
  , sqe_head_(std::exchange(other.sqe_head_, 0))
  , sqe_tail_(std::exchange(other.sqe_tail_, 0))
  , sq_entries_(std::exchange(other.sq_entries_, nullptr))
  , sq_index_array_(std::exchange(other.sq_index_array_, nullptr))
  , sq_head_(std::exchange(other.sq_head_, nullptr))
  , sq_tail_(std::exchange(other.sq_tail_, nullptr))
  , sq_flags_(std::exchange(other.sq_flags_, nullptr))
  , sq_dropped_(std::exchange(other.sq_dropped_, nullptr))
  , cq_entry_count_(std::exchange(other.cq_entry_count_, 0))
  , cq_mask_(std::exchange(other.cq_mask_, 0))
  , cq_entries_(std::exchange(other.cq_entries_, nullptr))
  , cq_head_(std::exchange(other.cq_head_, nullptr))
  , cq_tail_(std::exchange(other.cq_tail_, nullptr))
  , cq_overflow_(std::exchange(other.cq_overflow_, nullptr)) {
}

io_uring::~io_uring() {
}

bool io_uring::supports_timed_wait() const noexcept {
  return (available_features_ & IORING_FEAT_EXT_ARG) != 0;
}

bool io_uring::supports_completion_queue_nodrop() const noexcept {
  return (available_features_ & IORING_FEAT_NODROP) != 0;
}

std::uint32_t io_uring::submission_queue_unsubmitted() const noexcept {
  std::uint32_t tail = sqe_tail_;
  std::atomic_ref<const volatile std::uint32_t> head_ref{*sq_head_};
  std::uint32_t head = head_ref.load(std::memory_order_acquire);
  return tail - head;
}

std::uint32_t io_uring::submission_queue_dropped() const noexcept {
  std::atomic_ref<const volatile std::uint32_t> dropped_ref{*sq_dropped_};
  return dropped_ref.load(std::memory_order_relaxed);
}

void io_uring::enqueue_entry_for_submission(io_uring_sqe* sqe) noexcept {
  assert(submission_queue_space_left() > 0);
  std::uint32_t sqe_index = (sqe - sq_entries_);
  std::uint32_t sq_index = sqe_tail_++;
  sq_index_array_[sq_index] = sqe_index;
}

std::uint32_t io_uring::flush_submission_queue() noexcept {
  std::uint32_t tail = sqe_tail_;
  if (tail != sqe_head_) {
    sqe_head_ = tail;
    if ((setup_flags_ & IORING_SETUP_SQPOLL)) {
      std::atomic_ref<std::uint32_t> tail_ref{*sq_tail_};
      tail_ref.store(tail, std::memory_order_release);
    } else {
      *sq_tail_ = tail;
    }
  }

  std::atomic_ref<const volatile std::uint32_t> head_ref{*sq_head_};
  std::uint32_t head = head_ref.load(std::memory_order_acquire);
  return tail - head;
}

std::uint32_t io_uring::completion_queue_available() const noexcept {
  std::uint32_t head = *cq_head_;
  std::atomic_ref<const volatile std::uint32_t> tail_ref{*cq_tail_};
  std::uint32_t tail = tail_ref.load(std::memory_order_acquire);
  return tail - head;
}

std::uint32_t io_uring::completion_queue_overflow() const noexcept {
  std::atomic_ref<const volatile std::uint32_t> overflow_ref{*cq_overflow_};
  return overflow_ref.load(std::memory_order_relaxed);
}

void io_uring::completion_queue_advance(std::uint32_t count) noexcept {
  if (count > 0) {
    assert(count <= completion_queue_available());

    std::atomic_ref<std::uint32_t> head_ref{*cq_head_};
    std::uint32_t old_head = head_ref.load(std::memory_order_relaxed);
    head_ref.store(old_head + count, std::memory_order_release);

    LOG("completion_queue_advance - advance head to %u", (old_head + count));
  }
}

io_uring::completion_queue_view
io_uring::get_completion_entries() const noexcept {
  completion_queue_view result;

  // Safe to cast away the 'volatile' here as the values should not be
  // modified by the kernel thread after it publishes them to us.
  result.entries_ = reinterpret_cast<const completion_entry*>(
      const_cast<const io_uring_cqe*>(cq_entries_));
  result.mask_ = cq_mask_;
  result.head_ = *cq_head_;

  std::atomic_ref<const volatile std::uint32_t> tail_ref{*cq_tail_};
  std::uint32_t tail = tail_ref.load(std::memory_order_acquire);

  result.count_ = tail - result.head_;

  LOG("get_completion_entries() -> (entries=%p, mask=0x%08x, head=%u, "
      "count=%u)",
      (void*)result.entries_,
      result.mask_,
      result.head_,
      result.count_);

  return result;
}

std::error_code io_uring::submit() noexcept {
  const std::uint32_t to_submit = flush_submission_queue();

  bool needs_enter = false;

  std::uint32_t enter_flags = 0;
  if (to_submit > 0) {
    if ((setup_flags_ & IORING_SETUP_SQPOLL) != 0) {
      const std::uint32_t wakeup_flag = compute_sq_wakeup_flag();
      enter_flags |= wakeup_flag;
      needs_enter |= (wakeup_flag != 0);
    } else {
      needs_enter = true;
    }
  }

  if ((setup_flags_ & IORING_SETUP_IOPOLL) != 0) {
    needs_enter = true;
    enter_flags |= IORING_ENTER_GETEVENTS;
  } else {
    std::atomic_ref<const volatile std::uint32_t> sq_flags_ref{*sq_flags_};
    std::uint32_t sq_flags = sq_flags_ref.load(std::memory_order_relaxed);
    if ((sq_flags & (IORING_SQ_CQ_OVERFLOW | IORING_SQ_TASKRUN)) != 0) {
      needs_enter = true;
      enter_flags |= IORING_ENTER_GETEVENTS;
    }
  }

  if (needs_enter) {
    LOG("io_uring_enter(to_submit=%u, min_complete=%u, flags=%u)",
        to_submit,
        0u,
        enter_flags);

    int result = syscalls::io_uring_enter2(
        fd_.get_fd(), to_submit, 0, enter_flags, nullptr, 0);
    if (result < 0) {
      return std::error_code{-result, std::system_category()};
    }
  }

  return std::error_code{};
}

std::error_code io_uring::get_events() noexcept {
  const std::uint32_t enter_flags = IORING_ENTER_GETEVENTS;
  bool needs_enter = false;
  if ((setup_flags_ & IORING_SETUP_TASKRUN_FLAG) != 0) {
    std::atomic_ref<const volatile std::uint32_t> sq_flags_ref{*sq_flags_};
    std::uint32_t sq_flags = sq_flags_ref.load(std::memory_order_relaxed);
    if ((sq_flags & IORING_SQ_TASKRUN) != 0) {
      needs_enter = true;
    }
  } else if (
      (setup_flags_ &
       (IORING_SETUP_IOPOLL | IORING_SETUP_DEFER_TASKRUN |
        IORING_SETUP_COOP_TASKRUN)) != 0) {
    needs_enter = true;
  }

  if (needs_enter) {
    LOG("io_uring_enter(to_submit=%u, min_complete=%u, flags=%u)",
        0u,
        0u,
        enter_flags);
    int result =
        syscalls::io_uring_enter2(fd_.get_fd(), 0, 0, enter_flags, nullptr, 0);
    if (result < 0) {
      return std::error_code{-result, std::system_category()};
    }
  }

  return std::error_code{};
}

std::error_code io_uring::submit_and_get_events() noexcept {
  return submit_and_wait(0);
}

std::error_code io_uring::submit_and_wait(std::uint32_t min_complete) noexcept {
  std::uint32_t enter_flags = IORING_ENTER_GETEVENTS;

  const std::uint32_t to_submit = flush_submission_queue();
  if (to_submit > 0) {
    enter_flags |= compute_sq_wakeup_flag();
  }

  LOG("io_uring_enter(to_submit=%u, min_complete=%u, flags=%u)",
      to_submit,
      min_complete,
      enter_flags);
  int result = syscalls::io_uring_enter2(
      fd_.get_fd(), to_submit, min_complete, enter_flags, nullptr, 0);
  if (result < 0) {
    return std::error_code{-result, std::system_category()};
  }

  return std::error_code{};
}

std::error_code
io_uring::submit_and_wait_until(monotonic_clock::time_point timeout) noexcept {
  return submit_and_wait_until(1, timeout);
}

std::error_code io_uring::submit_and_wait_until(
    std::uint32_t min_complete, monotonic_clock::time_point timeout) noexcept {
  if ((available_features_ & IORING_FEAT_EXT_ARG) == 0) {
    LOG("submit_and_wait_until() not supported");
    return std::error_code{ENOTSUP, std::system_category()};
  }

  io_uring_getevents_arg ext_arg{};
  std::memset(&ext_arg, 0, sizeof(ext_arg));

  __kernel_timespec ts{};
  void* arg = nullptr;
  std::size_t arg_size = 0;

  std::uint32_t enter_flags = IORING_ENTER_GETEVENTS;

  const std::uint32_t to_submit = flush_submission_queue();

  if (to_submit > 0) {
    enter_flags |= compute_sq_wakeup_flag();
  }

  auto now = monotonic_clock::now();

  if (now < timeout) {
    ts.tv_nsec = timeout.nanoseconds_part() - now.nanoseconds_part();
    ts.tv_sec = timeout.seconds_part() - now.seconds_part();
    if (ts.tv_nsec < 0) {
      ts.tv_nsec += 1'000'000'000;
      ts.tv_sec -= 1;
    }
  } else {
    ts.tv_nsec = 0;
    ts.tv_sec = 0;
  }

  ext_arg.ts = reinterpret_cast<std::uintptr_t>(&ts);
  arg = &ext_arg;
  arg_size = sizeof(ext_arg);
  enter_flags |= IORING_ENTER_EXT_ARG;

  LOG("io_uring_enter(to_submit=%u, min_complete=%u, flags = %u, ts = "
      "%lli.%09llis)",
      to_submit,
      min_complete,
      enter_flags,
      ts.tv_sec,
      ts.tv_nsec);

  LOG("cqe_avail before: %u", completion_queue_available());
  LOG("time before: %li.%09is", now.seconds_part(), now.nanoseconds_part());
  LOG("timeout: %li.%09is", timeout.seconds_part(), timeout.nanoseconds_part());

  int result = syscalls::io_uring_enter2(
      fd_.get_fd(), to_submit, min_complete, enter_flags, arg, arg_size);
  now = monotonic_clock::now();
  LOG("result: %i", result);
  LOG("cqe_avail after: %u", completion_queue_available());
  LOG("time after: %li.%09is", now.seconds_part(), now.nanoseconds_part());

  if (result < 0) {
    return std::error_code{-result, std::system_category()};
  }

  return std::error_code{};
}

std::error_code
io_uring::submit_and_wait_for(std::chrono::nanoseconds timeout) noexcept {
  return submit_and_wait_for(1, timeout);
}

std::error_code io_uring::submit_and_wait_for(
    std::uint32_t min_complete, std::chrono::nanoseconds timeout) noexcept {
  if ((available_features_ & IORING_FEAT_EXT_ARG) == 0) {
    return std::error_code{ENOTSUP, std::system_category()};
  }

  io_uring_getevents_arg ext_arg{};
  __kernel_timespec ts{};
  void* arg = nullptr;
  std::size_t arg_size = 0;

  std::uint32_t enter_flags = IORING_ENTER_GETEVENTS;

  if (timeout.count() > 0) {
    ts.tv_sec = timeout.count() / std::nano::den;
    ts.tv_nsec = timeout.count() % std::nano::den;
  } else {
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
  }

  ext_arg.ts = reinterpret_cast<std::uintptr_t>(&ts);
  arg = &ext_arg;
  arg_size = sizeof(ext_arg);
  enter_flags |= IORING_ENTER_EXT_ARG;

  const std::uint32_t to_submit = flush_submission_queue();

  if (to_submit > 0) {
    enter_flags |= compute_sq_wakeup_flag();
  }

  int result = syscalls::io_uring_enter2(
      fd_.get_fd(), to_submit, min_complete, enter_flags, arg, arg_size);
  if (result < 0) {
    return std::error_code{-result, std::system_category()};
  }

  return std::error_code{};
}

io_uring_sqe* io_uring::get_next_submission_entry() noexcept {
  return &sq_entries_[sqe_tail_ & sq_mask_];
}

void io_uring::prepare_read_op(
    io_uring_sqe* sqe,
    int fd,
    std::uint64_t offset,
    void* buf,
    std::uint32_t sz,
    std::uint64_t user_data) noexcept {
  std::memset(sqe, 0, sizeof(io_uring_sqe));
  sqe->opcode = IORING_OP_READ;
  sqe->fd = fd;
  sqe->off = offset;
  sqe->addr = reinterpret_cast<std::uintptr_t>(buf);
  sqe->len = sz;
  sqe->user_data = user_data;
}

void io_uring::prepare_write_op(
    io_uring_sqe* sqe,
    int fd,
    std::uint64_t offset,
    const void* buf,
    std::uint32_t sz,
    std::uint64_t user_data) noexcept {
  std::memset(sqe, 0, sizeof(io_uring_sqe));
  sqe->opcode = IORING_OP_WRITE;
  sqe->fd = fd;
  sqe->off = offset;
  sqe->addr = reinterpret_cast<std::uintptr_t>(buf);
  sqe->len = sz;
  sqe->user_data = user_data;
}

void io_uring::prepare_close_op(
    io_uring_sqe* sqe, int fd, std::uint64_t user_data) noexcept {
  std::memset(sqe, 0, sizeof(io_uring_sqe));
  sqe->opcode = IORING_OP_CLOSE;
  sqe->fd = fd;
  sqe->user_data = user_data;
}

void io_uring::prepare_timeout_op(
    io_uring_sqe* sqe,
    std::uint32_t min_complete,
    const __kernel_timespec* timeout,
    clockid_t clock_id,
    bool is_absolute_time,
    std::uint64_t user_data) noexcept {
  std::memset(sqe, 0, sizeof(io_uring_sqe));
  sqe->opcode = IORING_OP_TIMEOUT;
  if (clock_id == CLOCK_BOOTTIME) {
    sqe->timeout_flags |= IORING_TIMEOUT_BOOTTIME;
  } else if (clock_id == CLOCK_REALTIME) {
    sqe->timeout_flags |= IORING_TIMEOUT_REALTIME;
  } else {
    assert(clock_id == CLOCK_MONOTONIC);
  }
  if (is_absolute_time) {
    sqe->timeout_flags |= IORING_TIMEOUT_ABS;
  }
  sqe->off = min_complete;
  sqe->len = 1;
  sqe->addr = reinterpret_cast<std::uintptr_t>(timeout);
  sqe->user_data = user_data;
}

std::uint32_t io_uring::compute_sq_wakeup_flag() const noexcept {
  if ((setup_flags_ & IORING_SETUP_SQPOLL) != 0) {
    // SQ polling enabled.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::atomic_ref<const volatile std::uint32_t> sq_flags_ref{*sq_flags_};
    std::uint32_t sq_flags = sq_flags_ref.load(std::memory_order_relaxed);
    if ((sq_flags & IORING_SQ_NEED_WAKEUP) != 0) [[unlikely]] {
      return IORING_ENTER_SQ_WAKEUP;
    }
  }

  return 0;
}

}  // namespace squiz::linuxos
