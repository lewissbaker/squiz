///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Copyright (c) Facebook, Inc. and its affiliates.
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cassert>
#include <utility>

namespace squiz::detail {

template <typename Item, Item* Item::* Next>
struct intrusive_queue {
public:
  intrusive_queue() noexcept : head_(nullptr), tail_(nullptr) {}

  intrusive_queue(intrusive_queue&& other) noexcept
    : head_(std::exchange(other.head_, nullptr))
    , tail_(std::exchange(other.tail_, nullptr)) {}

  ~intrusive_queue() { assert(empty()); }

  intrusive_queue(const intrusive_queue&) = delete;
  intrusive_queue& operator=(const intrusive_queue&) = delete;
  intrusive_queue& operator=(intrusive_queue&&) = delete;

  bool empty() const noexcept { return head_ == nullptr; }

  void push_front(Item* item) noexcept {
    item->*Next = head_;
    head_ = item;
    if (tail_ == nullptr) {
      tail_ = item;
    }
  }

  void push_back(Item* item) noexcept {
    item->*Next = nullptr;
    if (tail_ == nullptr) {
      head_ = item;
    } else {
      tail_->*Next = item;
    }
    tail_ = item;
  }

  [[nodiscard]] Item* pop_front() noexcept {
    assert(!empty());
    Item* item = head_;
    head_ = item->*Next;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    return item;
  }

  void append(intrusive_queue other) noexcept {
    if (empty()) {
      head_ = std::exchange(other.head_, nullptr);
      tail_ = std::exchange(other.tail_, nullptr);
    } else if (!other.empty()) {
      tail_->*Next = std::exchange(other.head_, nullptr);
      tail_ = std::exchange(other.tail_, nullptr);
    }
  }

  void prepend(intrusive_queue other) noexcept {
    if (empty()) {
      head_ = std::exchange(other.head_, nullptr);
      tail_ = std::exchange(other.tail_, nullptr);
    } else if (!other.empty()) {
      std::exchange(other.tail_, nullptr)->*Next = head_;
      head_ = std::exchange(other.head_, nullptr);
    }
  }

private:
  Item* head_;
  Item* tail_;
};

}  // namespace squiz::detail
