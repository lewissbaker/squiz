///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>

#include <new>

namespace squiz::detail {
template <typename Sender, typename Receiver>
  requires connectable_to<Sender, Receiver>
void connect_at(
    connect_result_t<Sender, Receiver>* op_state,
    Sender&& sender,
    Receiver receiver) noexcept(is_nothrow_connectable_v<Sender, Receiver>) {
  using op_state_t = connect_result_t<Sender, Receiver>;
  ::new (static_cast<void*>(op_state))
      op_state_t(std::forward<Sender>(sender).connect(std::move(receiver)));
}
}  // namespace squiz::detail
