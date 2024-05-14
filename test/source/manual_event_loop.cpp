///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/manual_event_loop.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/scheduler.hpp>
#include <squiz/then.hpp>
#include <squiz/when_all.hpp>

#include <chrono>
#include <semaphore>
#include <stop_token>
#include <thread>

#include <doctest/doctest.h>

static_assert(static_cast<bool>(
    squiz::scheduler<
        decltype(std::declval<squiz::manual_event_loop>().get_scheduler())>));

TEST_CASE("manual_event_loop default ctor") {
  squiz::manual_event_loop loop;
}

TEST_CASE("manual_event_loop run in another thread and stop") {
  squiz::manual_event_loop loop;

  {
    std::jthread thread([&](std::stop_token st) { loop.run(st); });
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(50ms);
  }
}

TEST_CASE("manual_event_loop run task in other thread") {
  squiz::manual_event_loop loop;
  {
    std::jthread thread([&](std::stop_token st) { loop.run(st); });

    struct receiver {
      std::binary_semaphore& sem;
      void set_value() noexcept { sem.release(); }
      void set_stopped() noexcept {
        CHECK(false);
        sem.release();
      }
      squiz::empty_env get_env() const noexcept { return {}; }
    };

    std::binary_semaphore sem(0);
    bool has_run = false;
    auto op = squiz::then_sender(loop.get_scheduler().schedule(), [&] noexcept {
                CHECK(std::this_thread::get_id() == thread.get_id());
                has_run = true;
              }).connect(receiver{sem});
    op.start();
    sem.acquire();
    CHECK(has_run);
  }
}

TEST_CASE("manual_event_loop request_stop before executed") {
  squiz::manual_event_loop loop;

  struct receiver {
    bool& receiver_invoked;
    void set_value() noexcept { CHECK(false); }
    void set_stopped() noexcept { receiver_invoked = true; }
  };

  {
    bool receiver_invoked = false;
    auto op =
        loop.get_scheduler().schedule().connect(receiver{receiver_invoked});
    op.start();
    CHECK(!receiver_invoked);
    op.request_stop();
    CHECK(receiver_invoked);
  }

  // Drive the event loop just to make sure it's not going to try to execute a
  // destroyed op.
  {
    std::jthread thread([&](std::stop_token st) { loop.run(st); });
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(50ms);
  }
}

TEST_CASE("manual_event_loop with multiple threads - one at a time") {
  struct receiver {
    std::binary_semaphore& sem;
    void set_value() noexcept { sem.release(); }
    void set_stopped() noexcept {
      CHECK(false);
      sem.release();
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  squiz::manual_event_loop loop;
  {
    std::jthread thread1([&](std::stop_token st) { loop.run(st); });
    std::jthread thread2([&](std::stop_token st) { loop.run(st); });

    std::binary_semaphore sem(0);
    for (unsigned int i = 0; i < 1000; ++i) {
      bool has_run = false;
      auto op =
          squiz::then_sender(loop.get_scheduler().schedule(), [&] noexcept {
            auto id = std::this_thread::get_id();
            CHECK((id == thread1.get_id() || id == thread2.get_id()));
            has_run = true;
          }).connect(receiver{sem});
      op.start();
      sem.acquire();
      CHECK(has_run);
    }
  }
}

TEST_CASE(
    "manual_event_loop with multiple threads - multiple schedules at a time") {
  struct receiver {
    std::binary_semaphore& sem;
    void set_value() noexcept { sem.release(); }
    void set_stopped() noexcept {
      CHECK(false);
      sem.release();
    }
    squiz::empty_env get_env() const noexcept { return {}; }
  };

  squiz::manual_event_loop loop;

  auto sched = loop.get_scheduler();

  {
    std::jthread thread1([&](std::stop_token st) { loop.run(st); });
    std::jthread thread2([&](std::stop_token st) { loop.run(st); });

    std::binary_semaphore sem(0);
    for (unsigned int i = 0; i < 1000; ++i) {
      bool has_run = false;
      auto op = squiz::then_sender(
                    squiz::when_all(
                        sched.schedule(),
                        sched.schedule(),
                        sched.schedule(),
                        sched.schedule(),
                        sched.schedule()),
                    [&] noexcept { has_run = true; })
                    .connect(receiver{sem});
      op.start();
      sem.acquire();
      CHECK(has_run);
    }
  }
}
