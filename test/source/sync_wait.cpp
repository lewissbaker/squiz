///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/sync_wait.hpp>

#include <squiz/just.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/then.hpp>

#include <thread>
#include <variant>

#include <doctest/doctest.h>

TEST_CASE("sync_wait just") {
  {
    auto x = squiz::sync_wait(squiz::just_value_sender());
    CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(x));
  }

  {
    auto x = squiz::sync_wait(squiz::just_value_sender(1, 2, 3));
    CHECK(
        std::holds_alternative<std::tuple<squiz::value_tag, int, int, int>>(x));
    auto& t = std::get<std::tuple<squiz::value_tag, int, int, int>>(x);
    CHECK(std::get<1>(t) == 1);
    CHECK(std::get<2>(t) == 2);
    CHECK(std::get<3>(t) == 3);
  }
}

TEST_CASE("sync_wait async operation") {
  squiz::manual_event_loop loop;
  std::jthread worker{[&](std::stop_token st) {
    loop.run(st);
  }};

  auto sched = loop.get_scheduler();

  auto result = squiz::sync_wait(squiz::then_sender(sched.schedule(), [&] {
    CHECK(std::this_thread::get_id() == worker.get_id());
    return 42;
  }));

  CHECK(std::holds_alternative<std::tuple<squiz::value_tag, int>>(result));
  CHECK(std::get<1>(std::get<std::tuple<squiz::value_tag, int>>(result)) == 42);
}

TEST_CASE("sync_wait can drive existing manual_event_loop") {
  squiz::manual_event_loop loop;

  auto sched = loop.get_scheduler();

  auto result = squiz::sync_wait(
      squiz::then_sender(sched.schedule(), [&] { return 42; }), loop);

  CHECK(std::holds_alternative<std::tuple<squiz::value_tag, int>>(result));
  CHECK(std::get<1>(std::get<std::tuple<squiz::value_tag, int>>(result)) == 42);
}
