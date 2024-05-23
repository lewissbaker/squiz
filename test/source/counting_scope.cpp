///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/counting_scope.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/let_value.hpp>
#include <squiz/stop_when.hpp>
#include <squiz/sync_wait.hpp>
#include <squiz/then.hpp>
#include <squiz/unstoppable.hpp>
#include <squiz/when_all.hpp>

#include <doctest/doctest.h>

TEST_CASE("counting_scope construct/destruct") {
  squiz::counting_scope scope;
}

TEST_CASE("counting_scope join with no nested work") {
  squiz::counting_scope scope;
  squiz::sync_wait(scope.join());
}

namespace {
auto run(auto&& func) {
  return squiz::then_sender(
      squiz::just_value_sender<>{}, static_cast<decltype(func)>(func));
}

struct join_receiver {
  bool& receiver_invoked;
  void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
  squiz::empty_env get_env() const noexcept { return {}; }
};

struct nest_value_receiver {
  bool& receiver_invoked;
  void set_result(squiz::value_t<>) noexcept { receiver_invoked = true; }
  void set_result(squiz::stopped_t) noexcept { CHECK(false); }
  squiz::empty_env get_env() const noexcept { return {}; }
};

struct nest_stopped_receiver {
  bool& receiver_invoked;
  void set_result(squiz::value_t<>) noexcept { CHECK(false); }
  void set_result(squiz::stopped_t) noexcept { receiver_invoked = true; }
  squiz::empty_env get_env() const noexcept { return {}; }
};

}  // namespace

TEST_CASE("counting_scope nest starts after join starts") {
  squiz::counting_scope scope;

  bool nest_receiver_invoked = false;
  bool join_receiver_invoked = false;
  bool lambda_invoked = false;

  auto join_op = scope.join().connect(join_receiver{join_receiver_invoked});

  {
    auto nest_op = scope.get_token()
                       .nest(run([&] noexcept {
                         CHECK(!nest_receiver_invoked);
                         CHECK(!join_receiver_invoked);
                         lambda_invoked = true;
                       }))
                       .connect(nest_value_receiver{nest_receiver_invoked});
    CHECK(!nest_receiver_invoked);
    CHECK(!join_receiver_invoked);
    CHECK(!lambda_invoked);

    join_op.start();

    CHECK(!nest_receiver_invoked);
    CHECK(!join_receiver_invoked);
    CHECK(!lambda_invoked);

    nest_op.start();

    CHECK(nest_receiver_invoked);
    CHECK(join_receiver_invoked);
    CHECK(lambda_invoked);
  }
}

TEST_CASE("counting_scope - nest after close doesn't run") {
  squiz::counting_scope scope;
  scope.close();

  bool lambda_invoked = false;
  auto result = squiz::sync_wait(
      scope.get_token().nest(run([&] noexcept { lambda_invoked = true; })));

  CHECK(!lambda_invoked);

  CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
}

TEST_CASE(
    "counting_scope - nest before close but started after close still runs") {
  squiz::counting_scope scope;

  bool lambda_invoked = false;
  auto s = scope.get_token().nest(run([&] noexcept { lambda_invoked = true; }));

  scope.close();

  auto result = squiz::sync_wait(std::move(s));

  CHECK(lambda_invoked);

  CHECK(std::holds_alternative<std::tuple<squiz::value_tag>>(result));
}

TEST_CASE(
    "counting_scope - multiple nests all must complete before join completes") {
  squiz::counting_scope scope;

  bool join_receiver_invoked = false;
  auto join_op = scope.join().connect(join_receiver{join_receiver_invoked});

  {
    bool nest1_receiver_invoked = false;
    bool lambda1_invoked = false;
    auto nest1_op = scope.get_token()
                        .nest(run([&] noexcept { lambda1_invoked = true; }))
                        .connect(nest_value_receiver{nest1_receiver_invoked});

    bool nest2_receiver_invoked = false;
    bool lambda2_invoked = false;
    auto nest2_op = scope.get_token()
                        .nest(run([&] noexcept { lambda2_invoked = true; }))
                        .connect(nest_value_receiver{nest2_receiver_invoked});
    CHECK(!join_receiver_invoked);
    CHECK(!nest1_receiver_invoked);
    CHECK(!lambda1_invoked);
    CHECK(!nest1_receiver_invoked);
    CHECK(!lambda1_invoked);

    join_op.start();

    CHECK(!join_receiver_invoked);
    CHECK(!nest1_receiver_invoked);
    CHECK(!lambda1_invoked);
    CHECK(!nest2_receiver_invoked);
    CHECK(!lambda2_invoked);

    nest1_op.start();

    CHECK(!join_receiver_invoked);
    CHECK(nest1_receiver_invoked);
    CHECK(lambda1_invoked);
    CHECK(!nest2_receiver_invoked);
    CHECK(!lambda2_invoked);

    nest2_op.start();

    CHECK(join_receiver_invoked);
    CHECK(nest1_receiver_invoked);
    CHECK(lambda1_invoked);
    CHECK(nest2_receiver_invoked);
    CHECK(lambda2_invoked);
  }
}

TEST_CASE("counting_scope - destroying nest sender without running releases "
          "ref-count") {
  squiz::counting_scope scope;

  bool join_receiver_invoked = false;
  auto join_op = scope.join().connect(join_receiver{join_receiver_invoked});

  {
    auto s = scope.get_token().nest(squiz::just_value_sender{});

    join_op.start();

    CHECK(!join_receiver_invoked);
  }

  CHECK(join_receiver_invoked);
}

TEST_CASE("counting_scope - copying nest sender increments ref-count") {
  squiz::counting_scope scope;

  bool join_receiver_invoked = false;
  auto join_op = scope.join().connect(join_receiver{join_receiver_invoked});

  {
    auto s = scope.get_token().nest(squiz::just_value_sender{});

    join_op.start();

    auto s2 = s;

    bool nest_receiver_invoked = false;
    auto op = std::move(s).connect(nest_value_receiver{nest_receiver_invoked});

    CHECK(!join_receiver_invoked);
    op.start();
    CHECK(!join_receiver_invoked);

    // Let 's2' go out of scope.
  }

  CHECK(join_receiver_invoked);
}

TEST_CASE("counting_scope - nest sender propagates stop-requests") {
  squiz::counting_scope scope;
  squiz::manual_event_loop loop;

  bool lambda1_invoked = false;
  bool lambda2_invoked = false;
  auto result = squiz::sync_wait(squiz::stop_when_sender(
      scope.get_token().nest(squiz::then_sender(
          loop.get_scheduler().schedule(),
          [&] noexcept { lambda1_invoked = true; })),
      run([&] { lambda2_invoked = true; })));

  CHECK(!lambda1_invoked);
  CHECK(lambda2_invoked);
  CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
}

TEST_CASE("counting_scope - multi-threaded stress test") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;
  squiz::manual_event_loop loop3;
  squiz::manual_event_loop loop4;

  std::jthread worker1{[&](std::stop_token st) {
    loop1.run(st);
  }};
  std::jthread worker2{[&](std::stop_token st) {
    loop2.run(st);
  }};
  std::jthread worker3{[&](std::stop_token st) {
    loop3.run(st);
  }};
  std::jthread worker4{[&](std::stop_token st) {
    loop4.run(st);
  }};

  for (std::uint32_t i = 0; i < 10'000; ++i) {
    squiz::counting_scope scope;

    squiz::sync_wait(squiz::when_all_sender(
        squiz::stop_when_sender(
            scope.get_token().nest(loop1.get_scheduler().schedule()),
            scope.get_token().nest(loop2.get_scheduler().schedule())),
        scope.get_token().nest(loop3.get_scheduler().schedule()),
        squiz::unstoppable_sender(
            squiz::let_value(loop4.get_scheduler().schedule(), [&] noexcept {
              return scope.join();
            }))));
  }
}
