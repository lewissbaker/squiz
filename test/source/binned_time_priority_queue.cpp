///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/detail/binned_time_priority_queue.hpp>

#include <chrono>
#include <cstdio>
#include <print>
#include <random>

#include <doctest/doctest.h>

namespace {

struct simple_item {
  explicit simple_item(std::uint32_t due_time) noexcept : due_time(due_time) {}
  simple_item* next{nullptr};
  simple_item* prev{nullptr};
  std::uint32_t due_time;
};

struct get_simple_item_due_time {
  static std::uint32_t operator()(const simple_item* item) noexcept {
    return item->due_time;
  }
};

using simple_queue_t = squiz::detail::binned_time_priority_queue<
    simple_item,
    &simple_item::next,
    &simple_item::prev,
    std::uint32_t,
    get_simple_item_due_time,
    65535,
    16>;

}  // namespace

TEST_CASE("binned_time_priority_queue - construct/destruct") {
  simple_queue_t queue;
  auto* item = queue.try_dequeue_next_due_by(0);
  CHECK(item == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == queue.max_timestamp());
}

TEST_CASE("binned_time_priority_queue - add items") {
  simple_queue_t queue;

  simple_item a(0x4);
  simple_item b(0x37);
  simple_item c(0x624);
  simple_item d(0xB391);

  queue.add(&c);
  queue.add(&a);
  queue.add(&d);
  queue.add(&b);

  auto* x = queue.try_dequeue_next_due_by(0);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 4);

  x = queue.try_dequeue_next_due_by(3);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 4);

  x = queue.try_dequeue_next_due_by(4);
  CHECK(x == &a);
  CHECK(queue.earliest_due_time_lower_bound() == 0x30);

  x = queue.try_dequeue_next_due_by(0x30);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0x37);

  x = queue.try_dequeue_next_due_by(0x40);
  CHECK(x == &b);
  CHECK(queue.earliest_due_time_lower_bound() == 0x600);

  x = queue.try_dequeue_next_due_by(0x40);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0x600);

  x = queue.try_dequeue_next_due_by(0x600);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0x620);

  x = queue.try_dequeue_next_due_by(0x600);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0x620);

  x = queue.try_dequeue_next_due_by(0x621);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0x624);

  x = queue.try_dequeue_next_due_by(0x630);
  CHECK(x == &c);
  CHECK(queue.earliest_due_time_lower_bound() == 0xB000);

  x = queue.try_dequeue_next_due_by(0x5000);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0xB000);

  x = queue.try_dequeue_next_due_by(0xB000);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0xB300);

  x = queue.try_dequeue_next_due_by(0xB300);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0xB390);

  x = queue.try_dequeue_next_due_by(0xB300);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0xB390);

  x = queue.try_dequeue_next_due_by(0xB390);
  CHECK(x == nullptr);
  CHECK(queue.earliest_due_time_lower_bound() == 0xB391);

  x = queue.try_dequeue_next_due_by(0xB400);
  CHECK(x == &d);
  CHECK(queue.earliest_due_time_lower_bound() == queue.max_timestamp());
}

TEST_CASE("binned_time_priority_queue - performance") {
  const std::size_t item_count = 1'000'000;
  const std::uint32_t items_per_batch = 20;

  std::vector<simple_item> items;
  items.reserve(item_count);

  using namespace squiz::detail;
  binned_time_priority_queue<
      simple_item,
      &simple_item::next,
      &simple_item::prev,
      std::uint32_t,
      get_simple_item_due_time>
      queue;

  using steady_clock = std::chrono::steady_clock;

  {
    std::minstd_rand rand(steady_clock::now().time_since_epoch().count());
    std::poisson_distribution<std::uint32_t> d(1000);

    for (std::size_t i = 0; i < item_count; ++i) {
      items.emplace_back(d(rand));
    }
  }

  auto start = steady_clock::now();

  std::uint32_t current_time = 0;
  std::uint32_t items_remaining = item_count;
  [[maybe_unused]] std::uint32_t prev_time = 0;

  std::uint32_t max_count = 0;

  std::uint32_t item_idx = 0;
  while (items_remaining > 0) {
    for (std::uint32_t i = 0; i < items_per_batch && item_idx < item_count;
         ++i, ++item_idx) {
      items[item_idx].due_time += current_time;
      queue.add(&items[item_idx]);
    }

    // sleep until next due time lower bound
    current_time = queue.earliest_due_time_lower_bound();

    while (auto* item = queue.try_dequeue_next_due_by(current_time)) {
      --items_remaining;
      assert(item->due_time <= current_time);
      assert(item->due_time >= prev_time);
      prev_time = item->due_time;
    }

    std::uint32_t number_dequeued = item_count - items_remaining;
    std::uint32_t count_remaining = item_idx - number_dequeued;
    max_count = std::max(count_remaining, max_count);
  }

  auto end = steady_clock::now();

  auto time =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  std::print("items: {}\n", item_count);
  std::print("time:  {} ns\n", time);
  std::print("cost:  {} ns/item\n", time / item_count);
  std::print("max_count: {}\n", max_count);
}
