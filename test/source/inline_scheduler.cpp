///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/inline_scheduler.hpp>

#include <squiz/scheduler.hpp>
#include <squiz/sender.hpp>
#include <squiz/empty_env.hpp>

#include <doctest/doctest.h>

static_assert(static_cast<bool>(squiz::scheduler<squiz::inline_scheduler>));

TEST_CASE("squiz::inline_scheduler default construction") {
  squiz::inline_scheduler a;
  squiz::inline_scheduler b{};
  CHECK(a == b);
}

TEST_CASE("squiz::inline_scheduler can schedule") {
  const squiz::inline_scheduler sched;
  squiz::sender auto s = sched.schedule();

  struct receiver{
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}
