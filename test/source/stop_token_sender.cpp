///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/stop_token_sender.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/single_inplace_stop_token.hpp>
#include <squiz/stop_when.hpp>
#include <squiz/sync_wait.hpp>
#include <squiz/then.hpp>
#include <squiz/when_all.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>

#include <array>
#include <atomic>
#include <stop_token>
#include <thread>

#include <doctest/doctest.h>

namespace {
using stoppable_env = squiz::detail::env_with_stop_possible<squiz::empty_env>;
}

TEST_CASE("stop_token_sender with single_inplace_stop_token") {
  squiz::single_inplace_stop_source ss;

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = squiz::stop_token_sender(ss.get_token())
                .connect(receiver{receiver_invoked});

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);

  CHECK(!receiver_invoked);
  op.start();
  CHECK(!receiver_invoked);
  ss.request_stop();
  CHECK(receiver_invoked);
}

TEST_CASE("stop_token_sender with single_inplace_stop_token with stop-request "
          "before start") {
  squiz::single_inplace_stop_source ss;

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = squiz::stop_token_sender(ss.get_token())
                .connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  ss.request_stop();
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}

TEST_CASE(
    "stop_token_sender with single_inplace_stop_token and stoppable-env") {
  squiz::single_inplace_stop_source ss;

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(false);
      receiver_invoked = true;
    }
    void set_result(squiz::stopped_t) noexcept { receiver_invoked = true; }
    stoppable_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = squiz::stop_token_sender(ss.get_token())
                .connect(receiver{receiver_invoked});

  static_assert(squiz::stoppable_operation_state<decltype(op)>);
  CHECK(!receiver_invoked);
  op.start();
  CHECK(!receiver_invoked);
  op.request_stop();
  CHECK(receiver_invoked);
}

TEST_CASE("stop_token_sender multi-threaded test") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;
  squiz::manual_event_loop loop3;

  std::jthread worker1{[&](std::stop_token st) {
    loop1.run(st);
  }};
  std::jthread worker2{[&](std::stop_token st) {
    loop2.run(st);
  }};

  std::array<std::uint32_t, 8> outcome_counts{};

  for (std::uint32_t i = 0; i < 10'000; ++i) {
    squiz::single_inplace_stop_source ss;

    std::atomic<std::uint8_t> flags{0};

    auto result = squiz::sync_wait(squiz::when_all(
        squiz::then_sender(
            loop1.get_scheduler().schedule(),
            [&] noexcept {
              flags += 1;
              ss.request_stop();
            }),
        squiz::stop_when_sender(
            squiz::then_sender(
                loop2.get_scheduler().schedule(), [&] noexcept { flags += 2; }),
            squiz::then_sender(
                squiz::stop_token_sender(ss.get_token()),
                [&] noexcept { flags += 4; }))));

    CHECK((flags.load() & 1) == 1);

    if (std::holds_alternative<std::tuple<squiz::value_tag>>(result)) {
      CHECK((flags.load() & 3) == 3);
    } else {
      CHECK(flags.load() == 5);
    }

    ++outcome_counts[flags.load()];
  }

  for (std::size_t i = 3; i < outcome_counts.size(); i += 2) {
    std::printf("outcome %zu: %u\n", i, outcome_counts[i]);
  }
}

TEST_CASE("stop_token_sender with std::stop_token signalled later") {
  std::stop_source ss;

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = squiz::stop_token_sender(ss.get_token())
                .connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(!receiver_invoked);
  ss.request_stop();
  CHECK(receiver_invoked);
}

TEST_CASE("stop_token_sender with std::stop_token already signalled") {
  std::stop_source ss;

  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = squiz::stop_token_sender(ss.get_token())
                .connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  ss.request_stop();
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}
