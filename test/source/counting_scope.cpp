///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/counting_scope.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/let_value.hpp>
#include <squiz/single_inplace_stop_token.hpp>
#include <squiz/statement.hpp>
#include <squiz/stop_token_sender.hpp>
#include <squiz/stop_when.hpp>
#include <squiz/sync_wait.hpp>
#include <squiz/then.hpp>
#include <squiz/unstoppable.hpp>
#include <squiz/when_all.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>

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

struct destructor_checker {
  explicit destructor_checker(bool& destructor_has_run) noexcept
    : destructor_has_run_(&destructor_has_run) {}

  destructor_checker(destructor_checker&& other) noexcept
    : destructor_has_run_(std::exchange(other.destructor_has_run_, nullptr)) {}

  ~destructor_checker() {
    if (destructor_has_run_ != nullptr) {
      *destructor_has_run_ = true;
    }
  }

private:
  bool* destructor_has_run_;
};

using stoppable_env =
    squiz::detail::make_env_with_stop_possible_t<squiz::empty_env>;

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

TEST_CASE("counting_scope - multi-threaded nest stress test") {
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
        squiz::statement_sender(squiz::stop_when_sender(
            scope.get_token().nest(loop1.get_scheduler().schedule()),
            scope.get_token().nest(loop2.get_scheduler().schedule()))),
        squiz::statement_sender(
            scope.get_token().nest(loop3.get_scheduler().schedule())),
        squiz::unstoppable_sender(
            squiz::let_value(loop4.get_scheduler().schedule(), [&] noexcept {
              return scope.join();
            }))));
  }
}

TEST_CASE("counting_scope - spawn synchronous operation before join") {
  squiz::counting_scope scope;

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  scope.get_token().spawn(
      run([&, x = destructor_checker(destructor_invoked)] noexcept {
        lambda_invoked = true;
      }));
  CHECK(lambda_invoked);
  CHECK(destructor_invoked);

  bool join_receiver_invoked = false;
  auto op = scope.join().connect(join_receiver{join_receiver_invoked});
  CHECK(!join_receiver_invoked);
  op.start();
  CHECK(join_receiver_invoked);
}

TEST_CASE("counting_scope - spawn asynchronous operation - complete after join "
          "starts") {
  squiz::counting_scope scope;
  squiz::single_inplace_stop_source ss;

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  scope.get_token().spawn(squiz::then_sender(
      squiz::stop_token_sender(ss.get_token()),
      [&, x = destructor_checker(destructor_invoked)] noexcept {
        lambda_invoked = true;
      }));
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);

  struct receiver {
    bool& destructor_invoked;
    bool& lambda_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      // Check that the op-state is destroyed before the receiver is invoked.
      CHECK(destructor_invoked);
      CHECK(lambda_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool join_receiver_invoked = false;
  auto op = scope.join().connect(
      receiver{destructor_invoked, lambda_invoked, join_receiver_invoked});
  CHECK(!join_receiver_invoked);
  op.start();
  CHECK(!destructor_invoked);
  CHECK(!lambda_invoked);
  CHECK(!join_receiver_invoked);
  ss.request_stop();
  CHECK(join_receiver_invoked);
}

TEST_CASE("counting_scope - spawn_future() synchronous operation before join") {
  squiz::counting_scope scope;

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  {
    auto future = scope.get_token().spawn_future(
        run([&, x = destructor_checker(destructor_invoked)] noexcept {
          lambda_invoked = true;
        }));
    CHECK(lambda_invoked);
    CHECK(!destructor_invoked);
  }
  CHECK(destructor_invoked);

  bool join_receiver_invoked = false;
  auto op = scope.join().connect(join_receiver{join_receiver_invoked});
  CHECK(!join_receiver_invoked);
  op.start();
  CHECK(join_receiver_invoked);
}

TEST_CASE("counting_scope - spawn_future() join does not complete until future "
          "discarded") {
  squiz::counting_scope scope;

  bool lambda_invoked = false;
  bool destructor_invoked = false;

  auto future = scope.get_token().spawn_future(
      run([&, x = destructor_checker(destructor_invoked)] noexcept {
        lambda_invoked = true;
      }));
  CHECK(lambda_invoked);
  CHECK(!destructor_invoked);

  struct receiver {
    bool& destructor_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      // Check that the op-state is destroyed before the join completes.
      CHECK(destructor_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool join_receiver_invoked = false;
  auto op =
      scope.join().connect(receiver{destructor_invoked, join_receiver_invoked});
  CHECK(!join_receiver_invoked);
  op.start();
  CHECK(!join_receiver_invoked);

  // discard the sender
  auto(std::move(future));

  CHECK(destructor_invoked);
  CHECK(join_receiver_invoked);
}

TEST_CASE(
    "counting_scope spawn_future() - consume future before it completes") {
  squiz::counting_scope scope;
  squiz::single_inplace_stop_source ss;

  struct future_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(lambda_invoked);
      CHECK(destructor_invoked);
      receiver_invoked = true;
    }
    void set_result(squiz::stopped_t) noexcept { CHECK(false); }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  struct join_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& future_receiver_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(lambda_invoked);
      CHECK(destructor_invoked);
      CHECK(!future_receiver_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  bool future_receiver_invoked = false;
  bool join_receiver_invoked = false;

  auto future_op =
      scope.get_token()
          .spawn_future(squiz::then_sender(
              squiz::stop_token_sender(ss.get_token()),
              [&, x = destructor_checker(destructor_invoked)] noexcept {
                CHECK(!destructor_invoked);
                lambda_invoked = true;
              }))
          .connect(future_receiver{
              lambda_invoked, destructor_invoked, future_receiver_invoked});
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  future_op.start();
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);

  auto join_op = scope.join().connect(join_receiver{
      lambda_invoked,
      destructor_invoked,
      future_receiver_invoked,
      join_receiver_invoked});
  CHECK(!join_receiver_invoked);
  join_op.start();
  CHECK(!join_receiver_invoked);

  ss.request_stop();
  CHECK(lambda_invoked);
  CHECK(destructor_invoked);
  CHECK(join_receiver_invoked);
  CHECK(future_receiver_invoked);
}

TEST_CASE("counting_scope spawn_future() - consume future after it completes") {
  squiz::counting_scope scope;
  squiz::single_inplace_stop_source ss;

  struct future_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(lambda_invoked);
      CHECK(destructor_invoked);
      receiver_invoked = true;
    }
    void set_result(squiz::stopped_t) noexcept { CHECK(false); }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  struct join_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& future_receiver_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(lambda_invoked);
      CHECK(destructor_invoked);
      CHECK(!future_receiver_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  bool future_receiver_invoked = false;
  bool join_receiver_invoked = false;

  auto future_op =
      scope.get_token()
          .spawn_future(squiz::then_sender(
              squiz::stop_token_sender(ss.get_token()),
              [&, x = destructor_checker(destructor_invoked)] noexcept {
                CHECK(!destructor_invoked);
                lambda_invoked = true;
              }))
          .connect(future_receiver{
              lambda_invoked, destructor_invoked, future_receiver_invoked});

  auto join_op = scope.join().connect(join_receiver{
      lambda_invoked,
      destructor_invoked,
      future_receiver_invoked,
      join_receiver_invoked});
  join_op.start();

  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  CHECK(!join_receiver_invoked);

  ss.request_stop();
  CHECK(lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  CHECK(!join_receiver_invoked);

  future_op.start();
  CHECK(destructor_invoked);
  CHECK(future_receiver_invoked);
  CHECK(join_receiver_invoked);
}

TEST_CASE(
    "counting_scope spawn_future() - discard future before it completes") {
  squiz::counting_scope scope;
  squiz::single_inplace_stop_source ss;

  struct join_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(!lambda_invoked);
      CHECK(destructor_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  bool join_receiver_invoked = false;

  auto future = scope.get_token().spawn_future(squiz::then_sender(
      squiz::stop_token_sender(ss.get_token()),
      [&, x = destructor_checker(destructor_invoked)] noexcept {
        CHECK(!destructor_invoked);
        lambda_invoked = true;
      }));

  auto join_op = scope.join().connect(
      join_receiver{lambda_invoked, destructor_invoked, join_receiver_invoked});
  join_op.start();

  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!join_receiver_invoked);

  // Discard future
  // This should cause a stop-request to propagate to the operation which
  // lets it complete and be destroyed synchronously.
  auto(std::move(future));

  CHECK(!lambda_invoked);
  CHECK(destructor_invoked);
  CHECK(join_receiver_invoked);
}

TEST_CASE("counting_scope spawn_future() - cancel started future before it "
          "completes") {
  squiz::counting_scope scope;
  squiz::single_inplace_stop_source ss;

  struct future_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { CHECK(false); }
    void set_result(squiz::stopped_t) noexcept {
      CHECK(destructor_invoked);
      CHECK(!lambda_invoked);
      receiver_invoked = true;
    }
    stoppable_env get_env() const noexcept { return {}; }
  };

  struct join_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& future_receiver_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(!lambda_invoked);
      CHECK(destructor_invoked);
      CHECK(!future_receiver_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  bool future_receiver_invoked = false;
  bool join_receiver_invoked = false;

  auto future_op =
      scope.get_token()
          .spawn_future(squiz::then_sender(
              squiz::stop_token_sender(ss.get_token()),
              [&, x = destructor_checker(destructor_invoked)] noexcept {
                CHECK(!destructor_invoked);
                lambda_invoked = true;
              }))
          .connect(future_receiver{
              lambda_invoked, destructor_invoked, future_receiver_invoked});

  auto join_op = scope.join().connect(join_receiver{
      lambda_invoked,
      destructor_invoked,
      future_receiver_invoked,
      join_receiver_invoked});
  join_op.start();

  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  CHECK(!join_receiver_invoked);

  future_op.start();
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  CHECK(!join_receiver_invoked);

  future_op.request_stop();
  CHECK(!lambda_invoked);
  CHECK(destructor_invoked);
  CHECK(join_receiver_invoked);
  CHECK(future_receiver_invoked);
}

TEST_CASE("counting_scope spawn_future() - cancel started future before it "
          "completes - unstoppable source") {
  squiz::counting_scope scope;
  squiz::single_inplace_stop_source ss;

  struct future_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { CHECK(false); }
    void set_result(squiz::stopped_t) noexcept {
      CHECK(!destructor_invoked);
      CHECK(!lambda_invoked);
      receiver_invoked = true;
    }
    stoppable_env get_env() const noexcept { return {}; }
  };

  struct join_receiver {
    bool& lambda_invoked;
    bool& destructor_invoked;
    bool& future_receiver_invoked;
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept {
      CHECK(lambda_invoked);
      CHECK(destructor_invoked);
      CHECK(future_receiver_invoked);
      receiver_invoked = true;
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  bool future_receiver_invoked = false;
  bool join_receiver_invoked = false;

  auto future_op =
      scope.get_token()
          .spawn_future(squiz::then_sender(
              squiz::unstoppable_sender(
                  squiz::stop_token_sender(ss.get_token())),
              [&, x = destructor_checker(destructor_invoked)] noexcept {
                CHECK(!destructor_invoked);
                lambda_invoked = true;
              }))
          .connect(future_receiver{
              lambda_invoked, destructor_invoked, future_receiver_invoked});

  auto join_op = scope.join().connect(join_receiver{
      lambda_invoked,
      destructor_invoked,
      future_receiver_invoked,
      join_receiver_invoked});
  join_op.start();

  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  CHECK(!join_receiver_invoked);

  future_op.start();
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!future_receiver_invoked);
  CHECK(!join_receiver_invoked);

  future_op.request_stop();
  CHECK(!lambda_invoked);
  CHECK(!destructor_invoked);
  CHECK(!join_receiver_invoked);
  CHECK(future_receiver_invoked);

  // Let the underlying operation complete
  ss.request_stop();
  CHECK(lambda_invoked);
  CHECK(destructor_invoked);
  CHECK(join_receiver_invoked);
  CHECK(future_receiver_invoked);
}

TEST_CASE("counting_scope - spawn_future() after close immediately completes "
          "with stopped") {
  squiz::counting_scope scope;

  scope.close();

  struct future_receiver {
    bool& receiver_invoked;
    void set_result(squiz::value_t<>) noexcept { CHECK(false); }
    void set_result(squiz::stopped_t) noexcept { receiver_invoked = true; }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  bool lambda_invoked = false;
  bool destructor_invoked = false;
  bool receiver_invoked = false;
  auto op = scope.get_token()
                .spawn_future(run(
                    [&, x = destructor_checker{destructor_invoked}] noexcept {
                      lambda_invoked = true;
                    }))
                .connect(future_receiver{receiver_invoked});
  CHECK(!lambda_invoked);
  CHECK(destructor_invoked);
  CHECK(!receiver_invoked);

  op.start();
  CHECK(!lambda_invoked);
  CHECK(destructor_invoked);
  CHECK(receiver_invoked);
}

TEST_CASE("counting_scope - spawn_future() mult-thread test") {
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

  std::array<std::uint32_t, 8> result_counts{};

  for (std::uint32_t i = 0; i < 20'000; ++i) {
    squiz::counting_scope scope;
    auto tok = scope.get_token();

    auto ptr = std::make_shared<int>(42);

    bool lambda1_invoked = false;
    bool lambda2_invoked = false;
    bool lambda3_invoked = false;
    bool join_lambda_invoked = false;

    auto s = squiz::when_all_sender(
        squiz::statement_sender(squiz::stop_when_sender(
            tok.spawn_future(squiz::then_sender(
                loop1.get_scheduler().schedule(),
                [&, ptr] noexcept { lambda1_invoked = true; })),
            tok.spawn_future(squiz::then_sender(
                loop2.get_scheduler().schedule(),
                [&, ptr] noexcept { lambda2_invoked = true; })))),
        tok.spawn_future(squiz::then_sender(
            loop3.get_scheduler().schedule(),
            [&, ptr] noexcept { lambda3_invoked = true; })),
        squiz::then_sender(
            scope.join(), [&, ptr = std::weak_ptr<int>(ptr)] noexcept {
              CHECK(ptr.lock() == nullptr);
              join_lambda_invoked = true;
            }));

    ptr.reset();

    auto result = squiz::sync_wait(std::move(s));

    CHECK(join_lambda_invoked);
    CHECK((lambda1_invoked || lambda2_invoked));
    CHECK((lambda2_invoked || lambda3_invoked));

    if (std::holds_alternative<std::tuple<squiz::value_tag>>(result)) {
      CHECK(lambda1_invoked);
    } else {
      CHECK(std::holds_alternative<std::tuple<squiz::stopped_tag>>(result));
      CHECK(lambda2_invoked);
    }

    std::uint32_t result_index = 0;
    if (lambda1_invoked)
      result_index += 1;
    if (lambda2_invoked)
      result_index += 2;
    if (lambda3_invoked)
      result_index += 4;

    ++result_counts[result_index];
  }

  for (std::size_t i = 0; i < result_counts.size(); ++i) {
    std::printf("outcome %zu: %u\n", i, result_counts[i]);
  }
}
