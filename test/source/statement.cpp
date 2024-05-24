///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/statement.hpp>

#include <memory>

#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/operation_state.hpp>
#include <squiz/single_inplace_stop_token.hpp>
#include <squiz/stop_token_sender.hpp>
#include <squiz/then.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>

#include <doctest/doctest.h>

namespace {
using stoppable_env = squiz::detail::env_with_stop_possible<squiz::empty_env>;
}

TEST_CASE("statement() - of just()") {
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = squiz::statement_sender(squiz::just_value_sender())
                .connect(receiver{receiver_invoked});

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}

TEST_CASE("statement() - destroys op-state on completion") {
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  struct state {
    bool& destroyed;
    ~state() { destroyed = true; }
  };

  bool destructor_invoked = false;
  bool receiver_invoked = false;
  bool lambda_invoked = false;
  auto op =
      squiz::statement_sender(
          squiz::then_sender(
              squiz::just_value_sender(),
              [&, x = std::make_unique<state>(destructor_invoked)] noexcept {
                lambda_invoked = true;
              }))
          .connect(receiver{receiver_invoked});

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
  CHECK(!receiver_invoked);
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  op.start();
  CHECK(receiver_invoked);
  CHECK(lambda_invoked);
  CHECK(destructor_invoked);
}

TEST_CASE("statement() - destroys op-state on completion when stoppable") {
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    stoppable_env get_env() const noexcept { return {}; }
  };

  struct state {
    bool& destroyed;
    ~state() { destroyed = true; }
  };

  squiz::single_inplace_stop_source ss;

  bool destructor_invoked = false;
  bool receiver_invoked = false;
  bool lambda_invoked = false;
  auto op =
      squiz::statement_sender(
          squiz::then_sender(
              squiz::stop_token_sender(ss.get_token()),
              [&, x = std::make_unique<state>(destructor_invoked)] noexcept {
                lambda_invoked = true;
              }))
          .connect(receiver{receiver_invoked});

  static_assert(squiz::stoppable_operation_state<decltype(op)>);
  CHECK(!receiver_invoked);
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  op.start();
  CHECK(!receiver_invoked);
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  ss.request_stop();
  CHECK(receiver_invoked);
  CHECK(lambda_invoked);
  CHECK(destructor_invoked);
}

TEST_CASE("statement() - propagates request_stop()") {
  struct receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { CHECK(false); }
    void set_result(squiz::stopped_t) noexcept { receiver_invoked = true; }
    stoppable_env get_env() const noexcept { return {}; }
  };

  struct state {
    bool& destroyed;
    ~state() { destroyed = true; }
  };

  squiz::manual_event_loop loop;

  bool destructor_invoked = false;
  bool receiver_invoked = false;
  bool lambda_invoked = false;

  auto op =
      squiz::statement_sender(
          squiz::then_sender(
              loop.get_scheduler().schedule(),
              [&, x = std::make_unique<state>(destructor_invoked)] noexcept {
                lambda_invoked = true;
              }))
          .connect(receiver{receiver_invoked});

  static_assert(squiz::stoppable_operation_state<decltype(op)>);
  CHECK(!receiver_invoked);
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  op.start();
  CHECK(!receiver_invoked);
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  op.request_stop();
  CHECK(receiver_invoked);
  CHECK(!lambda_invoked);
  CHECK(destructor_invoked);
}
