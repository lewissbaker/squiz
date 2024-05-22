///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/stop_when.hpp>

#include <squiz/just.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/sync_wait.hpp>
#include <squiz/then.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

#include <doctest/doctest.h>

TEST_CASE("stop_when synchronous completion") {
  auto s = squiz::stop_when_sender{
      squiz::just_value_sender(), squiz::just_value_sender()};

  auto result = squiz::sync_wait(s);

  CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
}

TEST_CASE("stop_when asynchronous source, synchronous trigger") {
  squiz::manual_event_loop loop;

  auto s = squiz::stop_when_sender{
      loop.get_scheduler().schedule(), squiz::just_value_sender()};

  auto result = squiz::sync_wait(s);

  CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
}

TEST_CASE(
    "stop_when asynchronous source, async trigger - source completes first") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;

  auto result = squiz::sync_wait(
      squiz::stop_when_sender(
          squiz::then_sender(
              loop1.get_scheduler().schedule(), [] noexcept { return 42; }),
          loop2.get_scheduler().schedule()),
      loop1);

  CHECK(std::holds_alternative<std::tuple<squiz::value_tag, int>>(result));
}

TEST_CASE(
    "stop_when asynchronous source, async trigger - trigger completes first") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;

  auto result = squiz::sync_wait(
      squiz::stop_when_sender(
          squiz::then_sender(
              loop1.get_scheduler().schedule(), [] noexcept { return 42; }),
          loop2.get_scheduler().schedule()),
      loop2);

  CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
}

TEST_CASE("stop_when multi-threaded stress-test") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;
  squiz::manual_event_loop loop3;

  std::jthread worker1{[&](std::stop_token st) {
    loop1.run(st);
  }};
  std::jthread worker2{[&](std::stop_token st) {
    loop2.run(st);
  }};
  std::jthread worker3{[&](std::stop_token st) {
    loop3.run(st);
  }};

  std::array<std::uint32_t, 8> outcome_counts{};

  for (std::uint32_t i = 0; i < 10'000; ++i) {
    std::atomic<std::uint8_t> flags{0};
    squiz::sync_wait(squiz::stop_when_sender(
        squiz::stop_when_sender(
            squiz::then_sender(
                loop1.get_scheduler().schedule(), [&] noexcept { flags += 1; }),
            squiz::then_sender(
                loop2.get_scheduler().schedule(),
                [&] noexcept { flags += 2; })),
        squiz::then_sender(
            loop3.get_scheduler().schedule(), [&] noexcept { flags += 4; })));
    ++outcome_counts[flags.load()];

    // At least one of the lambdas should have run.
    CHECK(flags.load() != 0);
  }

  for (std::size_t i = 0; i < outcome_counts.size(); ++i) {
    std::printf("outcome %zu: %u\n", i, outcome_counts[i]);
  }
}
