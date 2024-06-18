///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Copyright (c) Facebook, Inc. and its affiliates.
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cassert>

#include <squiz/detail/intrusive_queue.hpp>
#include <squiz/detail/intrusive_stack.hpp>

namespace squiz::detail {

/// An intrusive queue that allows multiple threads to concurrently
/// enqueue items and a single thread to dequeue items.
///
/// The queue has the ability to set an "inactive" state when it is
/// empty which can be used to indicate that the thread that dequeues
/// items is not actively polling for new items and may need to be
/// woken up and/or scheduled by the thread that next enqueues an item.
template <typename Item, Item* Item::* Next>
struct atomic_intrusive_queue {
public:
  atomic_intrusive_queue() : head_(nullptr) {}

  ~atomic_intrusive_queue() {
    assert(
        head_.load(std::memory_order_relaxed) == nullptr ||
        head_.load(std::memory_order_relaxed) == inactive_value());
  }

  atomic_intrusive_queue(atomic_intrusive_queue&&) = delete;
  atomic_intrusive_queue(const atomic_intrusive_queue&) = delete;
  atomic_intrusive_queue& operator=(atomic_intrusive_queue&&) = delete;
  atomic_intrusive_queue& operator=(const atomic_intrusive_queue&) = delete;

  ////////////////////////////////////////////////////////////////////////////
  // Methods to be called by producer threads that enqueue items to the queue.
  //

  /// Enqueue an item unconditionally and return whether or not the
  /// queue was previously inactive.
  ///
  /// \param item
  /// The item to enqueue.
  ///
  /// \return
  /// \c true if the previous state was the "inactive" state.
  /// \c false otherwise.
  [[nodiscard]] bool enqueue(Item* item) noexcept {
    void* const inactive = inactive_value();
    void* old_head = head_.load(std::memory_order_relaxed);
    do {
      item->*Next =
          (old_head == inactive) ? nullptr : static_cast<Item*>(old_head);
    } while (!head_.compare_exchange_weak(
        old_head, item, std::memory_order_release, std::memory_order_relaxed));
    return old_head == inactive;
  }

  /// Try to enqueue an item if there is an active consumer, otherwise try to
  /// become the active consumer by setting the queue state from "inactive"
  /// to "active".
  ///
  /// \return
  /// \c true if the item was successfully enqueued.
  /// \c false if the item was not enqueued because the queue was inactive,
  /// in which case the queue has been marked active and the caller is now
  /// considered the "consumer" and is responsible for ensuring items are
  /// dequeued.
  [[nodiscard]] bool enqueue_or_mark_active(Item* item) noexcept {
    void* old_head = head_.load(std::memory_order_relaxed);
    while (true) {
      if (old_head == inactive_value()) {
        if (head_.compare_exchange_weak(
                old_head,
                nullptr,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
          item->*Next = nullptr;
          return false;
        }
      } else {
        item->*Next = static_cast<Item*>(old_head);
        if (head_.compare_exchange_weak(
                old_head,
                item,
                std::memory_order_release,
                std::memory_order_relaxed)) {
          return true;
        }
      }
    }
  }

  ///////////////////////////////////////////////////////////////////////////
  // Methods to be called by the "consumer" thread responsible for dequeueing
  // items from the queue.
  //
  // The following methods are only valid to be called by the current thread
  // responsible for dequeueing the items from the queue.

  /// Query if the current thread is inactive.
  bool is_inactive() const noexcept {
    return head_.load(std::memory_order_relaxed) == inactive_value();
  }

  /// Dequeue all items from the queue.
  ///
  /// \pre
  /// The queue must not be currently in an inactive state.
  ///
  /// \return
  /// A queue of enqueued items in the order they were enqueued.
  ///
  /// \note
  /// This requires reversing the order of the items in the queue.
  /// If you need to process all of the items anyway, you may be better
  /// off
  [[nodiscard]] intrusive_queue<Item, Next> dequeue_all() noexcept {
    return reverse_items(dequeue_all_reversed());
  }

  /// Dequeue all enqueued items from the queue and return them in
  /// the reverse order from which they were enqueued.
  ///
  /// Use this method instead of \c dequeue_all() if you need to iterate
  /// through all of the items anyway as it will be more efficient as it
  /// does not need to traverse the list.
  ///
  /// \pre
  /// Requires that the queue is not currently in the "inactive" state.
  [[nodiscard]] intrusive_stack<Item, Next> dequeue_all_reversed() noexcept {
    void* head = head_.load(std::memory_order_relaxed);
    if (head != nullptr) {
      head = head_.exchange(nullptr, std::memory_order_acquire);
    }

    assert(head != inactive_value());

    return intrusive_stack<Item, Next>::adopt(static_cast<Item*>(head));
  }

  /// Attempt to put the queue in the inactive state.
  ///
  /// If the queue is empty, puts the queue into an inactive state, requiring
  /// the next enqueue attempt to try to wake up the consumer, otherwise if
  /// the queue is non-empty then has no effect.
  ///
  /// \pre
  /// Requires that the queue is not already in the inactive state.
  ///
  /// \return
  /// \c true if the queue was put into the inactive state.
  /// \c false if the queue was non-empty.
  [[nodiscard]] bool try_mark_inactive() noexcept {
    void* old_head = head_.load(std::memory_order_relaxed);
    bool result = false;
    if (old_head == nullptr) {
      result = head_.compare_exchange_strong(
          old_head,
          inactive_value(),
          std::memory_order_release,
          std::memory_order_relaxed);
    }
    assert(old_head != inactive_value());
    return result;
  }

  /// Attempt to put the queue in an inactive state if the queue
  /// is still empty, otherwise dequeue any items in the queue if
  /// the queue was non-empty.
  ///
  /// \return
  /// The list of items that were dequeued.
  /// If this list of items is empty then the queue was successfully
  /// put into the "inactive" state.
  [[nodiscard]] intrusive_queue<Item, Next>
  try_mark_inactive_or_dequeue_all() noexcept {
    return reverse_items(try_mark_inactive_or_dequeue_reversed());
  }

  /// Attempt to put the queue in an inactive state if the queue
  /// is still empty, otherwise dequeue any items in the queue if
  /// the queue was non-empty.
  ///
  /// \return
  /// The list of items that were dequeued in the reverse order in
  /// which they were enqueued.
  /// If this list of items is empty then the queue was successfully
  /// put into the "inactive" state.
  [[nodiscard]] intrusive_stack<Item, Next>
  try_mark_inactive_or_dequeue_reversed() noexcept {
    if (try_mark_inactive()) {
      return {};
    }

    void* old_head = head_.exchange(nullptr, std::memory_order_acquire);
    assert(old_head != inactive_value());
    return intrusive_stack<Item, Next>::adopt(static_cast<Item*>(old_head));
  }

private:
  /// The value to store in head_ to indicate that the consumer is currently
  /// inactive and may not see any subsequently queued items unless something
  /// is done to actively trigger it to start dequeueing the messages again.
  void* inactive_value() const noexcept {
    return const_cast<void*>(static_cast<const void*>(&head_));
  }

  static intrusive_queue<Item, Next>
  reverse_items(intrusive_stack<Item, Next> items) noexcept {
    intrusive_queue<Item, Next> reversed_items;
    while (!items.empty()) {
      reversed_items.push_front(items.pop_front());
    }
    return reversed_items;
  }

  std::atomic<void*> head_;
};
}  // namespace squiz::detail
