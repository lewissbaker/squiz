///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/just.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/sender.hpp>

#include <memory>

#include <doctest/doctest.h>

static_assert(std::same_as<
              decltype(squiz::just_sender().get_completion_signatures()),
              squiz::completion_signatures<squiz::set_value_t()>>);

static_assert(std::same_as<
              decltype(squiz::just_sender(42).get_completion_signatures()),
              squiz::completion_signatures<squiz::set_value_t(int)>>);

static_assert(
    std::same_as<
        decltype(squiz::just_sender{42, true}.get_completion_signatures()),
        squiz::completion_signatures<squiz::set_value_t(int, bool)>>);

static_assert(std::same_as<
              decltype(squiz::just_sender(std::unique_ptr<int>())
                           .get_completion_signatures()),
              squiz::completion_signatures<
                  squiz::set_value_t(std::unique_ptr<int>)>>);

static_assert(squiz::sender_in<squiz::just_sender<std::unique_ptr<int>>>);
static_assert(!squiz::sender_in<squiz::just_sender<std::unique_ptr<int>>&>);

TEST_CASE("squiz::just_sender()") {
  auto s = squiz::just_sender();
  struct dummy_receiver {
    bool& invoked;
    void set_value() noexcept { invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool invoked = false;
  auto op = s.connect(dummy_receiver{invoked});
  CHECK(!invoked);
  op.start();
  CHECK(invoked);
}

TEST_CASE("squiz::just_sender(int) lvalue connect") {
  auto s = squiz::just_sender(42);
  struct dummy_receiver {
    bool& invoked;
    void set_value(int x) noexcept {
      CHECK(x == 42);
      invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool invoked = false;
  auto op = s.connect(dummy_receiver{invoked});
  CHECK(!invoked);
  op.start();
  CHECK(invoked);
}

TEST_CASE("squiz::just_sender(unique_ptr<int>) rvalue connect") {
  auto s = squiz::just_sender(std::make_unique<int>(42));
  struct dummy_receiver {
    bool& invoked;
    void set_value(std::unique_ptr<int> x) noexcept {
      CHECK(*x == 42);
      invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool invoked = false;
  auto op = std::move(s).connect(dummy_receiver{invoked});
  CHECK(!invoked);
  op.start();
  CHECK(invoked);
}

