///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/when_all.hpp>

#include <squiz/completion_signatures.hpp>
#include <squiz/cond.hpp>
#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>

#include <concepts>
#include <string>

#include <doctest/doctest.h>

TEST_CASE("when_all() of no arguments") {
  auto s = squiz::when_all();
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

TEST_CASE("when_all() of 1 sender") {
  auto s = squiz::when_all(squiz::just_value_sender(1, 2, 3));
  static_assert(std::same_as<
                decltype(s.get_completion_signatures()),
                squiz::completion_signatures<squiz::value_t<int, int, int>>>);

  struct receiver {
    bool& receiver_invoked;
    void
    set_result(squiz::value_t<int, int, int>, int a, int b, int c) noexcept {
      CHECK(a == 1);
      CHECK(b == 2);
      CHECK(c == 3);
      receiver_invoked = true;
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

TEST_CASE("when_all() of 2 senders") {
  auto s = squiz::when_all(
      squiz::just_value_sender(1, 2),
      squiz::just_value_sender(std::string("hello")));
  static_assert(
      std::same_as<
          decltype(std::move(s).get_completion_signatures()),
          squiz::completion_signatures<squiz::value_t<int, int, std::string>>>);

  struct receiver {
    bool& receiver_invoked;
    void set_result(
        squiz::value_t<int, int, std::string>,
        int a,
        int b,
        std::string&& c) noexcept {
      CHECK(a == 1);
      CHECK(b == 2);
      CHECK(c == "hello");
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool receiver_invoked = false;
  auto op = std::move(s).connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);

  static_assert(!squiz::stoppable_operation_state<decltype(op)>);
}

TEST_CASE("when_all() of 2 senders with multiple value-completions") {
  auto make_sender = [](bool a, bool b) {
    return squiz::when_all(
        squiz::cond(
            a, squiz::just_value_sender(1), squiz::just_value_sender(2, 3)),
        squiz::cond(
            b, squiz::just_value_sender(4), squiz::just_value_sender(5, 6, 7)));
  };

  struct receiver_base {
    bool& receiver_invoked;
    void set_result(squiz::value_t<int, int>, int, int) noexcept {
      CHECK(false);
    }
    void set_result(
        squiz::value_t<int, int, int, int>, int, int, int, int) noexcept {
      CHECK(false);
    }
    void set_result(squiz::value_t<int, int, int>, int, int, int) noexcept {
      CHECK(false);
    }
    void set_result(
        squiz::value_t<int, int, int, int, int>,
        int,
        int,
        int,
        int,
        int) noexcept {
      CHECK(false);
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  {
    struct receiver : receiver_base {
      using receiver_base::set_result;
      void set_result(squiz::value_t<int, int>, int a, int b) noexcept {
        CHECK(a == 1);
        CHECK(b == 4);
        receiver_invoked = true;
      }
    };
    bool receiver_invoked = false;
    auto op = make_sender(true, true).connect(receiver{receiver_invoked});
    CHECK(!receiver_invoked);
    op.start();
    CHECK(receiver_invoked);
  }

  {
    struct receiver : receiver_base {
      using receiver_base::set_result;
      void
      set_result(squiz::value_t<int, int, int>, int a, int b, int c) noexcept {
        CHECK(a == 2);
        CHECK(b == 3);
        CHECK(c == 4);
        receiver_invoked = true;
      }
    };
    bool receiver_invoked = false;
    auto op = make_sender(false, true).connect(receiver{receiver_invoked});
    CHECK(!receiver_invoked);
    op.start();
    CHECK(receiver_invoked);
  }

  {
    struct receiver : receiver_base {
      using receiver_base::set_result;
      void set_result(
          squiz::value_t<int, int, int, int, int>,
          int a,
          int b,
          int c,
          int d,
          int e) noexcept {
        CHECK(a == 2);
        CHECK(b == 3);
        CHECK(c == 5);
        CHECK(d == 6);
        CHECK(e == 7);
        receiver_invoked = true;
      }
    };
    bool receiver_invoked = false;
    auto op = make_sender(false, false).connect(receiver{receiver_invoked});
    CHECK(!receiver_invoked);
    op.start();
    CHECK(receiver_invoked);
  }
}
