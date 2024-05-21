///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/receiver.hpp>

namespace squiz {

/// A \c scheduler type that always completes schedule operations inline
/// inside the \c start() method.
class inline_scheduler {
  template <typename Receiver>
  class schedule_op
    : public inlinable_operation_state<schedule_op<Receiver>, Receiver> {
    using inlinable_base = inlinable_operation_state<schedule_op, Receiver>;

  public:
    explicit schedule_op(Receiver r) noexcept : inlinable_base(std::move(r)) {}

    void start() noexcept { squiz::set_value<>(this->get_receiver()); }
  };

  struct schedule_sender {
    static auto get_completion_signatures() -> completion_signatures<value_t<>>;
    static auto is_always_nothrow_connectable() -> std::true_type;

    template <typename Receiver>
    static schedule_op<Receiver> connect(Receiver r) noexcept {
      return schedule_op<Receiver>(std::move(r));
    }
  };

public:
  constexpr inline_scheduler() noexcept = default;

  schedule_sender schedule() const noexcept { return {}; }

  friend constexpr bool
  operator==(inline_scheduler, inline_scheduler) noexcept {
    return true;
  }
};

}  // namespace squiz
