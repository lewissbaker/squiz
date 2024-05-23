///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/unstoppable.hpp>

#include <squiz/empty_env.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/operation_state.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>

#include <doctest/doctest.h>

TEST_CASE("unstoppable removes ability to request-stop") {
  using stoppable_env = squiz::detail::env_with_stop_possible<squiz::empty_env>;
  struct receiver {
    void set_result(squiz::value_t<>) noexcept {}
    void set_result(squiz::stopped_t) noexcept {}
    stoppable_env get_env() const noexcept { return {}; }
  };

  squiz::manual_event_loop loop;
  auto s = loop.get_scheduler().schedule();
  auto op = s.connect(receiver{});
  static_assert(squiz::stoppable_operation_state<decltype(op)>);
  static_assert(
      std::same_as<
          squiz::completion_signatures_for_t<decltype(s), stoppable_env>,
          squiz::completion_signatures<squiz::value_t<>, squiz::stopped_t>>);

  auto s2 = squiz::unstoppable_sender(s);
  auto op2 = s2.connect(receiver{});
  static_assert(!squiz::stoppable_operation_state<decltype(op2)>);
  static_assert(std::same_as<
                squiz::completion_signatures_for_t<decltype(s2), stoppable_env>,
                squiz::completion_signatures<squiz::value_t<>>>);
}
