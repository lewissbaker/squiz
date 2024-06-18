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
class intrusive_stack {
public:
  intrusive_stack() noexcept : head_(nullptr) {}

  intrusive_stack(intrusive_stack&& other) noexcept
    : head_(std::exchange(other.head_, nullptr)) {}

  intrusive_stack(const intrusive_stack&) = delete;
  intrusive_stack& operator=(const intrusive_stack&) = delete;

  ~intrusive_stack() { assert(empty()); }

  [[nodiscard]] static intrusive_stack adopt(Item* list) noexcept {
    intrusive_stack stack;
    stack.head_ = list;
    return stack;
  }

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

  void push_front(Item* item) noexcept {
    item->*Next = head_;
    head_ = item;
  }

  [[nodiscard]] Item* pop_front() noexcept {
    assert(!empty());
    Item* item = head_;
    head_ = item->*Next;
    return item;
  }

  [[nodiscard]] Item* release() noexcept {
    return std::exchange(head_, nullptr);
  }

  void reverse() noexcept {
    intrusive_stack reversed;
    while (!empty()) {
      reversed.push_front(pop_front());
    }
    head_ = reversed.release();
  }

private:
  Item* head_;
};

}  // namespace squiz::detail
