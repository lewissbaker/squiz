///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/then.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/overload.hpp>
#include <squiz/sender.hpp>

#include <memory>
#include <tuple>

#include <doctest/doctest.h>

namespace {

struct empty_receiver {
  static empty_receiver make_receiver(auto*) noexcept { return {}; }
  void set_value() noexcept {}
  squiz::empty_env get_env() const noexcept { return {}; }
};

}  // namespace

TEST_CASE("squiz::then_sender() can have an empty operation-state") {
  auto s0 = squiz::just_sender();
  auto s1 = squiz::then_sender(s0, [] noexcept {});
  auto s2 = squiz::then_sender(s1, [] noexcept {});
  auto s3 = squiz::then_sender(s2, [] noexcept {});
  auto s4 = squiz::then_sender(s3, [] noexcept {});
  auto s5 = squiz::then_sender(s4, [] noexcept {});
  auto s6 = squiz::then_sender(s5, [] noexcept {});
  auto s7 = squiz::then_sender(s6, [] noexcept {});
  auto op = s7.connect(empty_receiver{});
  CHECK(sizeof(op) == 1);
}

TEST_CASE("squiz::then_sender - void -> void") {
  bool lambda_invoked = false;
  bool receiver_invoked = false;
  auto s = squiz::then_sender(squiz::just_sender(), [&] noexcept {
    CHECK(!lambda_invoked);
    CHECK(!receiver_invoked);
    lambda_invoked = true;
  });

  struct receiver {
    bool& lambda_invoked;
    bool& receiver_invoked;
    void set_value() noexcept {
      CHECK(lambda_invoked);
      CHECK(!receiver_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  auto op = s.connect(receiver{lambda_invoked, receiver_invoked});
  CHECK(!lambda_invoked);
  CHECK(!receiver_invoked);
  op.start();
  CHECK(lambda_invoked);
  CHECK(receiver_invoked);
}

TEST_CASE("squiz::then_sender - void -> int") {
  bool lambda_invoked = false;
  bool receiver_invoked = false;
  auto s = squiz::then_sender(squiz::just_sender(), [&] noexcept {
    CHECK(!lambda_invoked);
    CHECK(!receiver_invoked);
    lambda_invoked = true;
    return 101;
  });

  struct receiver {
    bool& lambda_invoked;
    bool& receiver_invoked;
    void set_value(int x) noexcept {
      CHECK(x == 101);
      CHECK(lambda_invoked);
      CHECK(!receiver_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  auto op = s.connect(receiver{lambda_invoked, receiver_invoked});
  CHECK(!lambda_invoked);
  CHECK(!receiver_invoked);
  op.start();
  CHECK(lambda_invoked);
  CHECK(receiver_invoked);
}

namespace {

struct sender_of_int_or_bool {
  static auto get_completion_signatures() -> squiz::
      completion_signatures<squiz::set_value_t(int), squiz::set_value_t(bool)>;

  template <typename Receiver>
  struct op_state {
    bool send_int;
    Receiver receiver;

    void start() noexcept {
      if (send_int) {
        receiver.set_value(99);
      } else {
        receiver.set_value(false);
      }
    }
  };

  bool send_int;

  template <typename Receiver>
  op_state<Receiver> connect(Receiver r) const noexcept {
    return {send_int, std::move(r)};
  }
};

}  // namespace

TEST_CASE("squiz::then_sender handling multiple return types") {
  // Check that a lambda that collapses both types to 'void' ends up with a void
  // result.
  auto a =
      squiz::then_sender(sender_of_int_or_bool(true), [](auto) noexcept {});
  static_assert(std::same_as<
                decltype(a.get_completion_signatures()),
                squiz::completion_signatures<squiz::set_value_t()>>);

  // Check that a lambda that collapses both types to non-void ends up with a
  // single set_value result.
  auto b = squiz::then_sender(
      sender_of_int_or_bool(true), [](auto) noexcept { return 0; });
  static_assert(std::same_as<
                decltype(b.get_completion_signatures()),
                squiz::completion_signatures<squiz::set_value_t(int)>>);

  // Check that a lambda that transforms both types to different results ends
  // up with two set_value results.
  auto c =
      squiz::then_sender(sender_of_int_or_bool(false), [](auto x) noexcept {
        return std::make_tuple(x);
      });
  static_assert(std::same_as<
                decltype(c.get_completion_signatures()),
                squiz::completion_signatures<
                    squiz::set_value_t(std::tuple<int>),
                    squiz::set_value_t(std::tuple<bool>)>>);
}
