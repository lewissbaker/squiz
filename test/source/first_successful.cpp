///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/first_successful.hpp>

#include <squiz/completion_signatures.hpp>
#include <squiz/cond.hpp>
#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/let_value.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/single_inplace_stop_token.hpp>
#include <squiz/stop_when.hpp>
#include <squiz/sync_wait.hpp>
#include <squiz/then.hpp>
#include <squiz/unstoppable.hpp>

#include <array>
#include <concepts>
#include <cstdlib>
#include <string>
#include <thread>

#include <doctest/doctest.h>

TEST_CASE("first_successful() of one argument") {
  auto s = squiz::first_successful_sender{squiz::just_value_sender{}};
  static_assert(std::same_as<
                decltype(s.get_completion_signatures()),
                squiz::completion_signatures<squiz::value_t<>>>);

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("first_succssful() of two synchronous senders") {
  auto s = squiz::first_successful_sender{
      squiz::just_value_sender{42}, squiz::just_value_sender{false}};
  static_assert(std::same_as<
                decltype(s.get_completion_signatures()),
                squiz::completion_signatures<
                    squiz::value_t<int>,
                    squiz::value_t<bool>>>);
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<int>, int x) noexcept {
      receiver_invoked = true;
      CHECK(x == 42);
    }
    void set_result(squiz::value_t<bool>, bool) noexcept { CHECK(false); }

    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("first_succssful() of error and value synchronous senders") {
  auto s = squiz::first_successful_sender{
      squiz::just_error_sender{42}, squiz::just_value_sender{false}};
  static_assert(std::same_as<
                decltype(s.get_completion_signatures()),
                squiz::completion_signatures<
                    squiz::error_t<int>,
                    squiz::value_t<bool>>>);
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::error_t<int>, int) noexcept { CHECK(false); }
    void set_result(squiz::value_t<bool>, bool x) noexcept {
      receiver_invoked = true;
      CHECK(x == false);
    }

    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("first_successful() of synchronous and asynchronous values") {
  squiz::manual_event_loop loop;

  auto s = squiz::first_successful_sender{
      squiz::just_value_sender{42},
      squiz::then_sender{loop.get_scheduler().schedule(), [] noexcept {
                           return 99;
                         }}};
  static_assert(
      std::same_as<
          decltype(s.get_completion_signatures(squiz::empty_env{})),
          squiz::completion_signatures<squiz::value_t<int>, squiz::stopped_t>>);
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<int>, int x) noexcept {
      receiver_invoked = true;
      CHECK(x == 42);
    }
    void set_result(squiz::stopped_t) noexcept { CHECK(false); }

    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();

  // Cancellation of schedule operation should have completed synchronously
  // inside start().
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("first_successful() of two asynchronous values") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;

  auto s = squiz::first_successful_sender{
      squiz::then_sender{
          loop1.get_scheduler().schedule(),
          [] noexcept {
            return 11;
          }},
      squiz::then_sender{loop2.get_scheduler().schedule(), [] noexcept {
                           return 22;
                         }}};
  static_assert(
      std::same_as<
          decltype(s.get_completion_signatures(squiz::empty_env{})),
          squiz::completion_signatures<squiz::value_t<int>, squiz::stopped_t>>);

  squiz::single_inplace_stop_source ss;

  struct receiver {
    squiz::single_inplace_stop_source& ss;
    bool& receiver_invoked;
    void set_result(squiz::value_t<int>, int x) noexcept {
      receiver_invoked = true;
      ss.request_stop();
      CHECK(x == 22);
    }
    void set_result(squiz::stopped_t) noexcept { CHECK(false); }

    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{ss, receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();

  // Doesn't complete until we drive one of the event-loops to allow one to
  // complete.
  CHECK(!receiver_invoked);

  // Driving this event-loop causes the second sender to complete with 22.
  // This cancels the first sender, which then completes synchronously inside
  // the call to request_stop(), which then completes the overall
  // first_successful() operation.
  loop2.run(ss.get_token());
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("first_successful() with all errors") {
  auto s = squiz::first_successful_sender{
      squiz::just_error_sender{101},
      squiz::just_error_sender{std::string{"error"}}};
  static_assert(std::same_as<
                decltype(s.get_completion_signatures(squiz::empty_env{})),
                squiz::completion_signatures<
                    squiz::error_t<int>,
                    squiz::error_t<std::string>>>);

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::error_t<int>, int) noexcept { CHECK(false); }
    void set_result(squiz::error_t<std::string>, std::string&& x) noexcept {
      receiver_invoked = true;
      CHECK(x == "error");
    }

    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("first_successful() request_stop() cancels all children") {
  squiz::manual_event_loop loop;

  bool lambda1_invoked = false;
  bool lambda2_invoked = false;

  auto s = squiz::first_successful_sender{
      squiz::then_sender{
          loop.get_scheduler().schedule(),
          [&] noexcept {
            lambda1_invoked = true;
            return 1;
          }},
      squiz::then_sender{loop.get_scheduler().schedule(), [&] noexcept {
                           lambda2_invoked = true;
                           return 2;
                         }}};

  struct receiver {
    bool& receiver_invoked;
    squiz::single_inplace_stop_source& ss;

    void set_result(squiz::value_t<int>, int) noexcept { CHECK(false); }

    void set_result(squiz::stopped_t) noexcept {
      receiver_invoked = true;
      ss.request_stop();
    }

    auto get_env() const noexcept {
      return squiz::detail::env_with_stop_possible{squiz::empty_env{}};
    }
  };

  bool receiver_invoked = false;
  squiz::single_inplace_stop_source ss;
  auto op = s.connect(receiver{receiver_invoked, ss});
  CHECK(!receiver_invoked);
  CHECK(!lambda1_invoked);
  CHECK(!lambda2_invoked);
  op.start();
  CHECK(!receiver_invoked);
  CHECK(!lambda1_invoked);
  CHECK(!lambda2_invoked);
  op.request_stop();
  CHECK(receiver_invoked);
  CHECK(!lambda1_invoked);
  CHECK(!lambda2_invoked);
}

TEST_CASE("first_successful() request_stop() cancels all children 2") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;

  bool lambda1_invoked = false;
  bool lambda2_invoked = false;

  squiz::single_inplace_stop_source ss1;

  auto s = squiz::first_successful_sender{
      squiz::then_sender{
          loop1.get_scheduler().schedule(),
          [&] noexcept {
            ss1.request_stop();
            lambda1_invoked = true;
            return 1;
          }},
      squiz::then_sender{
          squiz::unstoppable_sender{loop2.get_scheduler().schedule()},
          [&] noexcept {
            lambda2_invoked = true;
            return 2;
          }}};

  struct receiver {
    bool& receiver_invoked;
    squiz::single_inplace_stop_source& ss;

    void set_result(squiz::value_t<int>, int x) noexcept {
      receiver_invoked = true;
      CHECK(x == 1);
      ss.request_stop();
    }

    void set_result(squiz::stopped_t) noexcept { CHECK(false); }

    auto get_env() const noexcept {
      return squiz::detail::env_with_stop_possible{squiz::empty_env{}};
    }
  };

  bool receiver_invoked = false;
  squiz::single_inplace_stop_source ss2;
  auto op = s.connect(receiver{receiver_invoked, ss2});
  CHECK(!receiver_invoked);
  CHECK(!lambda1_invoked);
  CHECK(!lambda2_invoked);

  op.start();
  CHECK(!receiver_invoked);
  CHECK(!lambda1_invoked);
  CHECK(!lambda2_invoked);

  loop1.run(ss1.get_token());
  CHECK(!receiver_invoked);
  CHECK(lambda1_invoked);
  CHECK(!lambda2_invoked);

  op.request_stop();
  CHECK(!receiver_invoked);
  CHECK(lambda1_invoked);
  CHECK(!lambda2_invoked);

  loop2.run(ss2.get_token());
  CHECK(receiver_invoked);
  CHECK(lambda1_invoked);
  CHECK(lambda2_invoked);
}

TEST_CASE("first_successful() multi-threaded race conditions") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;
  squiz::manual_event_loop loop3;
  squiz::manual_event_loop loop4;

  std::jthread thread1{[&](std::stop_token st) noexcept {
    loop1.run(st);
  }};
  std::jthread thread2{[&](std::stop_token st) noexcept {
    loop2.run(st);
  }};
  std::jthread thread3{[&](std::stop_token st) noexcept {
    loop3.run(st);
  }};
  std::jthread thread4{[&](std::stop_token st) noexcept {
    loop4.run(st);
  }};

  std::array<std::uint32_t, 4> counts = {0, 0, 0, 0};

  const std::uint32_t iteration_count = 10'000;
  for (std::uint32_t i = 0; i < iteration_count; ++i) {
    auto result = squiz::sync_wait(squiz::first_successful_sender{
        squiz::then_sender{
            loop1.get_scheduler().schedule(),
            [&] noexcept {
              return 1;
            }},
        squiz::then_sender{
            loop2.get_scheduler().schedule(),
            [&] noexcept {
              return 2;
            }},
        squiz::then_sender{loop3.get_scheduler().schedule(), [&] noexcept {
                             return 3;
                           }}});
    if (std::holds_alternative<std::tuple<squiz::value_tag, int>>(result)) {
      auto& tuple = std::get<std::tuple<squiz::value_tag, int>>(result);
      int result = std::get<1>(tuple);
      CHECK(result >= 1);
      CHECK(result <= 3);
      ++counts[result - 1];
    } else {
      CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
      ++counts[3];
    }
  }

  CHECK(counts[3] == 0);

  std::printf(
      "result 1: %u\n"
      "result 2: %u\n"
      "result 3: %u\n",
      counts[0],
      counts[1],
      counts[2]);
}
