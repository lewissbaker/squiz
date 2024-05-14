///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/manual_event_loop.hpp>

#include <cassert>

namespace squiz {

manual_event_loop::manual_event_loop() : head(nullptr), last_next(&head) {
}

manual_event_loop::~manual_event_loop() {
  assert(head == nullptr);
}

void manual_event_loop::run(std::stop_token st) {
  std::atomic_flag stop_requested{false};
  auto on_stop = [&] noexcept {
    stop_requested.test_and_set();
    wake_run_threads();
  };

  std::stop_callback cb(std::move(st), on_stop);
  run_impl(stop_requested);
}

void manual_event_loop::wake_run_threads() noexcept {
  std::lock_guard lk{mut};
  cv.notify_all();
}

void manual_event_loop::run_impl(std::atomic_flag& stop_requested) {
  std::unique_lock lk{mut};
  while (!stop_requested.test()) {
    auto* item = try_dequeue();
    if (item == nullptr) {
      cv.wait(lk, [&] { return head != nullptr || stop_requested.test(); });

      item = try_dequeue();
      if (item == nullptr)
        break;
    }

    // Mark the item as dequeued normally.
    item->prev_next = nullptr;

    lk.unlock();
    item->execute(item);
    lk.lock();
  }
}

manual_event_loop::task_base* manual_event_loop::try_dequeue() noexcept {
  auto* item = head;
  if (item != nullptr) {
    head = item->next;
    if (head != nullptr) {
      head->prev_next = &head;
    } else {
      last_next = &head;
    }
    item->prev_next = nullptr;
  }
  return item;
}

void manual_event_loop::enqueue(task_base* item) noexcept {
  std::lock_guard lk{mut};
  const bool was_empty = (head == nullptr);
  item->prev_next = last_next;
  *last_next = item;
  last_next = &item->next;
  if (was_empty) {
    cv.notify_one();
  }
}

bool manual_event_loop::try_remove(task_base* item) noexcept {
  std::lock_guard lk{mut};
  if (item->prev_next == nullptr) {
    // Already dequeued - executing concurrently.
    return false;
  }

  *item->prev_next = item->next;
  if (item->next == nullptr) {
    last_next = item->prev_next;
  } else {
    item->next->prev_next = item->prev_next;
  }
  item->prev_next = nullptr;

  return true;
}

}  // namespace squiz
