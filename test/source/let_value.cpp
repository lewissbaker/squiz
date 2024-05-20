///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/let_value.hpp>

#include <squiz/cond.hpp>
#include <squiz/empty_env.hpp>
#include <squiz/just.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/then.hpp>
#include <squiz/when_all.hpp>

#include <memory>
#include <semaphore>
#include <stop_token>
#include <string>
#include <thread>

#include <doctest/doctest.h>

TEST_CASE("let_value() simplest case") {
  auto s = squiz::let_value(
      squiz::just_sender(), [] noexcept { return squiz::just_sender(); });

  struct receiver {
    bool& receiver_invoked;
    squiz::empty_env get_env() const noexcept { return {}; }
    void set_value() noexcept { receiver_invoked = true; }
    void set_stopped() noexcept {
      CHECK(false);
      receiver_invoked = true;
    }
  };

  bool receiver_invoked = false;
  auto op = std::move(s).connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}

TEST_CASE("let_value() store a value") {
  auto s = squiz::let_value(
      squiz::just_sender(std::string("hello")), [](std::string& s) noexcept {
        CHECK(s == "hello");
        return squiz::just_sender();
      });

  struct receiver {
    bool& receiver_invoked;
    squiz::empty_env get_env() const noexcept { return {}; }
    void set_value() noexcept { receiver_invoked = true; }
    void set_stopped() noexcept {
      CHECK(false);
      receiver_invoked = true;
    }
  };

  bool receiver_invoked = false;
  auto op = std::move(s).connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}

TEST_CASE("let_value() store multiple values") {
  auto s = squiz::let_value(
      squiz::just_sender(std::string("hello"), std::make_unique<int>(42)),
      [](std::string& s, std::unique_ptr<int>& p) noexcept {
        CHECK(s == "hello");
        CHECK(*p == 42);
        return squiz::just_sender(std::move(p));
      });

  struct receiver {
    bool& receiver_invoked;
    squiz::empty_env get_env() const noexcept { return {}; }
    void set_value(std::unique_ptr<int> p) noexcept {
      CHECK(*p == 42);
      receiver_invoked = true;
    }
    void set_stopped() noexcept {
      CHECK(false);
      receiver_invoked = true;
    }
  };

  bool receiver_invoked = false;
  auto op = std::move(s).connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(receiver_invoked);
}

TEST_CASE("let_value() cancellation of source") {
  squiz::manual_event_loop loop;
  auto sched = loop.get_scheduler();

  bool lambda1_executed = false;
  bool lambda2_executed = false;
  bool lambda3_executed = false;
  auto s = squiz::let_value(
      squiz::then_sender(
          sched.schedule(),
          [&] noexcept {
            lambda1_executed = true;
            return std::make_unique<int>(42);
          }),
      [&](std::unique_ptr<int>& p) noexcept {
        lambda2_executed = true;
        return squiz::then_sender(sched.schedule(), [&] noexcept {
          lambda3_executed = true;
          return std::move(p);
        });
      });

  struct receiver {
    bool& receiver_invoked;
    squiz::empty_env get_env() const noexcept { return {}; }
    void set_value(std::unique_ptr<int>) noexcept {
      CHECK(false);
      receiver_invoked = true;
    }
    void set_stopped() noexcept { receiver_invoked = true; }
  };

  bool receiver_invoked = false;
  auto op = std::move(s).connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(!receiver_invoked);
  CHECK(!lambda1_executed);
  CHECK(!lambda2_executed);
  CHECK(!lambda3_executed);
  op.request_stop();
  CHECK(receiver_invoked);
  CHECK(!lambda1_executed);
  CHECK(!lambda2_executed);
  CHECK(!lambda3_executed);
}

TEST_CASE("let_value() cancellation of body") {
  squiz::manual_event_loop loop1;
  auto sched1 = loop1.get_scheduler();

  squiz::manual_event_loop loop2;
  auto sched2 = loop2.get_scheduler();

  std::stop_source ss1;

  bool lambda1_executed = false;
  bool lambda2_executed = false;
  bool lambda3_executed = false;
  auto s = squiz::let_value(
      squiz::then_sender(
          sched1.schedule(),
          [&] noexcept {
            lambda1_executed = true;
            ss1.request_stop();
            return std::make_unique<int>(42);
          }),
      [&](std::unique_ptr<int>& p) noexcept {
        lambda2_executed = true;
        return squiz::then_sender(sched2.schedule(), [&] noexcept {
          lambda3_executed = true;
          return std::move(p);
        });
      });

  struct receiver {
    bool& receiver_invoked;
    squiz::empty_env get_env() const noexcept { return {}; }
    void set_value(std::unique_ptr<int>) noexcept {
      CHECK(false);
      receiver_invoked = true;
    }
    void set_stopped() noexcept { receiver_invoked = true; }
  };

  bool receiver_invoked = false;
  auto op = std::move(s).connect(receiver{receiver_invoked});
  CHECK(!receiver_invoked);
  op.start();
  CHECK(!receiver_invoked);
  CHECK(!lambda1_executed);
  CHECK(!lambda2_executed);
  CHECK(!lambda3_executed);

  loop1.run(ss1.get_token());
  CHECK(!receiver_invoked);
  CHECK(lambda1_executed);
  CHECK(lambda2_executed);
  CHECK(!lambda3_executed);

  op.request_stop();
  CHECK(receiver_invoked);
  CHECK(lambda1_executed);
  CHECK(lambda2_executed);
  CHECK(!lambda3_executed);
}

TEST_CASE("let_value() cancellation, multiple threads") {
  squiz::manual_event_loop loop1;
  squiz::manual_event_loop loop2;

  std::jthread thread1([&](std::stop_token st) { loop1.run(st); });
  std::jthread thread2([&](std::stop_token st) { loop2.run(st); });

  struct receiver {
    std::binary_semaphore& sem;
    void set_value() noexcept { sem.release(); }
    void set_stopped() noexcept { sem.release(); }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  std::array<std::uint32_t, 5> stage_counts{};

  std::binary_semaphore sem{0};
  for (std::uint32_t i = 0; i < 10'000; ++i) {
    std::atomic<int> stage{0};
    auto op = squiz::then_sender(
                  squiz::let_value(
                      squiz::then_sender(
                          loop1.get_scheduler().schedule(),
                          [&] noexcept { ++stage; }),
                      [&] noexcept {
                        ++stage;
                        return squiz::then_sender(
                            loop2.get_scheduler().schedule(),
                            [&] noexcept { ++stage; });
                      }),
                  [&] noexcept { ++stage; })
                  .connect(receiver{sem});
    op.start();
    for (int j = 0; j < 10; ++j) {
      std::this_thread::yield();
    }
    op.request_stop();
    sem.acquire();
    ++stage_counts[stage.load()];
  }

  // Should see a mixture of different stages being hit here.
  // stage 0 - stopped before source sender could run lambda
  // stage 1 - stopped before body_factory could run
  //           i.e. concurrently with source sender
  // stage 2 - stopped before body sender could run lambda
  // stage 3 - body sender has run, but let_value completed with stopped
  //           (this shouldn't happen)
  // stage 4 - whole operation ran to completion
  for (std::size_t stage = 0; stage < stage_counts.size(); ++stage) {
    std::printf("stage %zu: %u\n", stage, stage_counts[stage]);
  }
  CHECK(stage_counts[3] == 0);
}
