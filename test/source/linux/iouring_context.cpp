///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/linuxos/iouring_context.hpp>

// other headers

#include <squiz/let_value.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/single_inplace_stop_token.hpp>
#include <squiz/stop_when.hpp>
#include <squiz/sync_wait.hpp>
#include <squiz/then.hpp>
#include <squiz/unstoppable.hpp>
#include <squiz/when_all.hpp>

#include <chrono>
#include <thread>

#include <fcntl.h>
#include <sys/eventfd.h>

#include <doctest/doctest.h>

TEST_CASE("iouring_context - default construct") {
  squiz::linuxos::iouring_context ctx;
}

TEST_CASE("iouring_context - default args constructor") {
  squiz::linuxos::io_uring::setup_args args;
  squiz::linuxos::iouring_context ctx(args);
}

TEST_CASE("iouring_context - sqe_count+cq_count constructor") {
  squiz::linuxos::io_uring::setup_args args;
  args.submission_queue_size = 128;
  args.completion_queue_size = 512;
  squiz::linuxos::iouring_context ctx(args);
}

TEST_CASE(
    "iouring_context - run() exits immediately if stop  already  requested") {
  squiz::linuxos::iouring_context ctx;
  squiz::single_inplace_stop_source ss;
  ss.request_stop();
  ctx.run(ss.get_token());
}

TEST_CASE("iouring_context - run() for some time and then stop with no work") {
  std::jthread worker{[&](std::stop_token st) {
    squiz::linuxos::iouring_context ctx;
    ctx.run(st);
  }};

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(20ms);

  // Just check that the worker thread completes and doesn't hang.
}

TEST_CASE(
    "iouring_context - run() executes schedule() tasks started on io context") {
  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  {
    auto result = squiz::sync_wait(sched.schedule(), ctx);
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
  }

  {
    bool lambda_ran = false;
    auto result = squiz::sync_wait(
        squiz::let_value(
            sched.schedule(),
            [&] noexcept {
              lambda_ran = true;
              return sched.schedule();
            }),
        ctx);
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
    CHECK(lambda_ran);
  }
}

TEST_CASE("iouring_context - run() with schedule_at() tasks") {
  using namespace std::chrono_literals;

  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  {
    auto due_time = sched.now() + 5ms;
    auto result = squiz::sync_wait(sched.schedule_at(due_time), ctx);
    CHECK(sched.now() >= due_time);
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
  }

  {
    auto start_time = sched.now();
    auto time_elapsed_us = [&] noexcept -> std::int64_t {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 sched.now() - start_time)
          .count();
    };

    auto result = squiz::sync_wait(
        squiz::when_all_sender{
            squiz::then_sender{
                sched.schedule_at(start_time + 10ms),
                [&] noexcept {
                  std::printf("10ms timer done %li\n", time_elapsed_us());
                }},
            squiz::then_sender{
                sched.schedule_at(start_time + 30ms),
                [&] noexcept {
                  std::printf("30ms timer done %li\n", time_elapsed_us());
                }},
            squiz::then_sender{
                sched.schedule_at(start_time + 5ms),
                [&] noexcept {
                  std::printf("5ms timer done %li\n", time_elapsed_us());
                }}},
        ctx);

    CHECK(sched.now() >= (start_time + 30ms));
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
  }
}

TEST_CASE("iouring_context - schedule_at cancellation") {
  using namespace std::chrono_literals;

  squiz::manual_event_loop loop;
  std::jthread other_thread{[&](std::stop_token st) {
    loop.run(st);
  }};

  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  auto start_time = sched.now();

  bool first_lambda_executed = false;
  bool second_lambda_executed = false;
  auto result = squiz::sync_wait(
      squiz::stop_when_sender{
          squiz::then_sender{
              sched.schedule_at(start_time + 1s),
              [&] noexcept {
                first_lambda_executed = true;
              }},
          squiz::then_sender{
              sched.schedule_at(start_time + 5ms),
              [&] noexcept {
                second_lambda_executed = true;
              }}},
      ctx);

  auto end_time = sched.now();

  std::printf(
      "schedule_at(start + 5ms) completed after %llius\n",
      std::chrono::duration_cast<std::chrono::microseconds>(
          end_time - start_time)
          .count());

  CHECK(!first_lambda_executed);
  CHECK(second_lambda_executed);
  CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
  CHECK((end_time - start_time) < 100ms);
}

TEST_CASE("iouring_context - schedule_after") {
  using namespace std::chrono_literals;

  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  {
    auto start_time = sched.now();
    auto time_elapsed_us = [&] noexcept -> std::int64_t {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 sched.now() - start_time)
          .count();
    };

    auto result = squiz::sync_wait(
        squiz::when_all_sender{
            squiz::then_sender{
                sched.schedule_after(10ms),
                [&] noexcept {
                  std::printf("10ms timer done %li\n", time_elapsed_us());
                }},
            squiz::then_sender{
                sched.schedule_after(30ms),
                [&] noexcept {
                  std::printf("30ms timer done %li\n", time_elapsed_us());
                }},
            squiz::then_sender{
                sched.schedule_after(5ms),
                [&] noexcept {
                  std::printf("5ms timer done %li\n", time_elapsed_us());
                }}},
        ctx);

    CHECK(sched.now() >= (start_time + 30ms));
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
  }
}

TEST_CASE("iouring_context - schedule_after cancellation") {
  using namespace std::chrono_literals;

  squiz::manual_event_loop loop;
  std::jthread other_thread{[&](std::stop_token st) {
    loop.run(st);
  }};

  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  auto start_time = sched.now();

  bool first_lambda_executed = false;
  bool second_lambda_executed = false;
  auto result = squiz::sync_wait(
      squiz::stop_when_sender{
          squiz::then_sender{
              sched.schedule_after(1s),
              [&] noexcept {
                first_lambda_executed = true;
              }},
          squiz::then_sender{
              sched.schedule_after(5ms),
              [&] noexcept {
                second_lambda_executed = true;
              }}},
      ctx);

  auto end_time = sched.now();

  std::printf(
      "schedule_after(5ms) completed after %llius\n",
      std::chrono::duration_cast<std::chrono::microseconds>(
          end_time - start_time)
          .count());

  CHECK(!first_lambda_executed);
  CHECK(second_lambda_executed);
  CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
  CHECK((end_time - start_time) < 100ms);
}

TEST_CASE("iouring_context - openat/close") {
  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  auto result = squiz::sync_wait(
      squiz::let_value_sender{
          sched.openat(-1, "/dev/zero", O_RDONLY | O_CLOEXEC, 0),
          [sched](squiz::linuxos::detail::file_handle& file) noexcept {
            std::puts("opened /dev/zero. closing...");
            return squiz::then_sender{
                squiz::unstoppable_sender(sched.close(file.release())),
                [] noexcept {
                  std::puts("closed");
                }};
          }},
      ctx);
  CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
}

TEST_CASE("iouring_context - openat/close cancellable") {
  squiz::linuxos::iouring_context ctx;

  auto sched = ctx.get_scheduler();

  using namespace std::chrono_literals;

  // Operation completes normally case
  {
    auto result = squiz::sync_wait(
        squiz::stop_when_sender{
            squiz::let_value_sender{
                sched.openat(-1, "/dev/zero", O_RDONLY | O_CLOEXEC, 0),
                [sched](squiz::linuxos::detail::file_handle& file) noexcept {
                  std::puts("opened /dev/zero. closing...");
                  return squiz::then_sender{
                      squiz::unstoppable_sender(sched.close(file.release())),
                      [] noexcept {
                        std::puts("closed");
                      }};
                }},
            squiz::then_sender{
                sched.schedule_after(100ms),
                [] noexcept {
                  std::puts("timedout");
                }}},
        ctx);
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
  }

  // Remote start + remote request_stop
  {
    auto result = squiz::sync_wait(
        squiz::stop_when_sender{
            squiz::let_value_sender{
                sched.openat(-1, "/dev/zero", O_RDONLY | O_CLOEXEC, 0),
                [sched](squiz::linuxos::detail::file_handle& file) noexcept {
                  std::puts("opened /dev/zero. closing...");
                  return squiz::then_sender{
                      squiz::unstoppable_sender(sched.close(file.release())),
                      [] noexcept {
                        std::puts("closed");
                      }};
                }},
            squiz::then_sender{
                sched.schedule_after(0ms),
                [] noexcept {
                  std::puts("timedout");
                }}},
        ctx);
    CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
  }
}
