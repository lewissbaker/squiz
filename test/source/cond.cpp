///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/cond.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/overload.hpp>
#include <squiz/sender.hpp>
#include <squiz/then.hpp>

#include <doctest/doctest.h>

TEST_CASE("squiz::cond() with same sender on both branches") {
  struct check_receiver {
    int expected;
    void set_result(squiz::value_t<int>, int x) noexcept {
      CHECK(x == expected);
    }
    squiz::empty_env get_env() noexcept { return {}; }
  };

  squiz::just_value_sender<int> a = squiz::cond(
      true, squiz::just_value_sender(2), squiz::just_value_sender(3));
  auto a_op = a.connect(check_receiver{2});
  a_op.start();

  squiz::just_value_sender<int> b = squiz::cond(
      false, squiz::just_value_sender(2), squiz::just_value_sender(3));
  auto b_op = b.connect(check_receiver{3});
  b_op.start();
}

TEST_CASE("squiz::cond() with different senders") {
  auto a = squiz::just_value_sender(42);
  auto b = squiz::just_value_sender(false, true);

  {
    auto s = squiz::cond(true, a, b);

    struct check_receiver {
      bool& receiver_invoked;
      void set_result(squiz::value_t<int>, int x) noexcept {
        CHECK(x == 42);
        receiver_invoked = true;
      }
      void set_result(squiz::value_t<bool, bool>, bool, bool) noexcept {
        CHECK(false);
      }
      squiz::empty_env get_env() const noexcept { return {}; }
    };

    bool receiver_invoked = false;
    auto op = s.connect(check_receiver{receiver_invoked});
    CHECK(!receiver_invoked);
    op.start();
    CHECK(receiver_invoked);
  }

  {
    auto s = squiz::cond(false, a, b);

    struct check_receiver {
      bool& receiver_invoked;
      void set_result(squiz::value_t<int>, int) noexcept { CHECK(false); }
      void set_result(squiz::value_t<bool, bool>, bool a, bool b) noexcept {
        CHECK(!a);
        CHECK(b);
        receiver_invoked = true;
      }
      squiz::empty_env get_env() const noexcept { return {}; }
    };

    bool receiver_invoked = false;
    auto op = s.connect(check_receiver{receiver_invoked});
    CHECK(!receiver_invoked);
    op.start();
    CHECK(receiver_invoked);
  }
}

TEST_CASE("squiz::cond() wrapped by then()") {
  auto s = squiz::then_sender(
      squiz::cond(
          true, squiz::just_value_sender(1), squiz::just_value_sender(false)),
      squiz::overload(
          [](int x) noexcept { CHECK(x == 1); },
          [](bool) noexcept { CHECK(false); }));

  struct check_receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = s.connect(check_receiver{receiver_invoked});
  op.start();
  CHECK(receiver_invoked);
}
