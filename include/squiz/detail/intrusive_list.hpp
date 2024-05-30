///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cassert>

namespace squiz::detail {

/// An intrusive doubly-linked list that supports insertion at the
/// front or back of the list as well as constant-time removal of
/// items from the list.
///
/// Also supports appending or prepending one intrusive_list to another.
template <typename Item, Item* Item::* Next, Item* Item::* Prev>
class intrusive_list {
public:
  intrusive_list() noexcept : head_(nullptr), tail_(nullptr) {}

  intrusive_list(intrusive_list&& other) noexcept
    : head_(std::exchange(other.head_, nullptr))
    , tail_(std::exchange(other.tail_, nullptr)) {}

  intrusive_list(const intrusive_list&) = delete;
  intrusive_list& operator=(const intrusive_list&) = delete;
  intrusive_list& operator=(intrusive_list&&) = delete;

  ~intrusive_list() { assert(empty()); }

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

  Item* front() noexcept { return head_; }

  void push_front(Item* item) noexcept {
    item->*Next = head_;
    item->*Prev = nullptr;
    if (head_ == nullptr) {
      tail_ = item;
    } else {
      head_->*Prev = item;
    }
    head_ = item;
  }

  void push_back(Item* item) noexcept {
    item->*Next = nullptr;
    item->*Prev = tail_;
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
    } else {
      head_->*Prev = nullptr;
    }
    return item;
  }

  [[nodiscard]] Item* pop_back() noexcept {
    assert(!empty());
    Item* item = tail_;
    tail_ = item->*Prev;
    if (tail_ == nullptr) {
      head_ = nullptr;
    } else {
      tail_->*Next = nullptr;
    }
    return item;
  }

  void remove(Item* item) noexcept {
    assert(!empty());
    Item* prev = item->*Prev;
    Item* next = item->*Next;
    if (prev != nullptr) {
      prev->*Next = next;
    } else {
      head_ = next;
    }
    if (next != nullptr) {
      next->*Prev = prev;
    } else {
      tail_ = prev;
    }
  }

  [[nodiscard]] Item* release() noexcept {
    return std::exchange(head_, nullptr);
  }

  void append(intrusive_list other) noexcept {
    if (empty()) {
      head_ = std::exchange(other.head_, nullptr);
      tail_ = std::exchange(other.tail_, nullptr);
    } else if (!other.empty()) {
      tail_->*Next = other.head_;
      tail_->*Next->*Prev = tail_;
      tail_ = other.tail_;
      other.head_ = nullptr;
      other.tail_ = nullptr;
    }
  }

  void prepend(intrusive_list other) noexcept {
    if (empty()) {
      head_ = std::exchange(other.head_, nullptr);
      tail_ = std::exchange(other.tail_, nullptr);
    } else if (!other.empty()) {
      other.tail_->*Next = head_;
      head_->*Prev = other.tail_;
      head_ = other.head_;
      other.head_ = nullptr;
      other.tail_ = nullptr;
    }
  }

private:
  Item* head_;
  Item* tail_;
};

}  // namespace squiz::detail
