///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <exception>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

#include <squiz/completion_signatures.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/empty_env.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/sender.hpp>

namespace squiz {

namespace sync_wait_detail {

template <typename Result, typename Env>
struct sync_wait_state {
private:
  struct receiver {
    sync_wait_state& state;

    template <typename... Vs>
    void set_value(Vs&&... vs) noexcept {
      try {
        state.result.template emplace<std::tuple<set_value_t, Vs...>>(
            set_value_t(), std::forward<Vs>(vs)...);
      } catch (...) {
        if constexpr (!(std::is_nothrow_move_constructible_v<Vs> && ...)) {
          state->result
              .template emplace<std::tuple<set_error_t, std::exception_ptr>>(
                  set_error_t(), std::current_exception());
        }
      }
      state.ss.request_stop();
    }

    template <typename E>
    void set_error(E&& e) noexcept {
      state.result.template emplace<std::tuple<set_error_t, E>>(
          set_error_t(), std::forward<E>(e));
      state.ss.request_stop();
    }

    void set_stopped() noexcept {
      state.result.template emplace<std::tuple<set_stopped_t>>(set_stopped_t());
      state.ss.request_stop();
    }

    Env get_env() const noexcept { return state.env; }
  };

public:
  receiver get_receiver() noexcept { return receiver{*this}; }

  Result result;
  std::stop_source ss;
  Env env;
};

}  // namespace sync_wait_detail

template <typename Src>
inline auto sync_wait(Src&& src, manual_event_loop& loop) {
  using result_t = detail::completion_signatures_to_variant_of_tuple_t<
      completion_signatures_for_t<Src, empty_env>>;

  sync_wait_detail::sync_wait_state<result_t, empty_env> state;
  auto op = std::forward<Src>(src).connect(state.get_receiver());
  op.start();

  loop.run(state.ss.get_token());
  return std::move(state.result);
}

template <typename Src>
inline auto sync_wait(Src&& src) {
  manual_event_loop loop;
  return sync_wait(std::forward<Src>(src), loop);
}

}  // namespace squiz
