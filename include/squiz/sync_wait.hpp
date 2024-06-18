///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

#include <squiz/completion_signatures.hpp>
#include <squiz/empty_env.hpp>
#include <squiz/manual_event_loop.hpp>
#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>
#include <squiz/single_inplace_stop_token.hpp>
#include <squiz/detail/completion_signatures_to_variant_of_tuple.hpp>
#include <squiz/detail/store_result.hpp>

namespace squiz {

namespace sync_wait_detail {

template <typename Result, typename Env>
struct sync_wait_state {
private:
  struct receiver {
    sync_wait_state& state;

    template <typename Tag, typename... Datums>
    void set_result(
        result_t<Tag, Datums...> sig,
        parameter_type<Datums>... datums) noexcept {
      detail::store_result(state.result, sig, datums...);
      state.ss.request_stop();
    }

    Env get_env() const noexcept { return state.env; }
  };

public:
  receiver get_receiver() noexcept { return receiver{*this}; }

  single_inplace_stop_source ss;
  [[no_unique_address]] Result result;
  [[no_unique_address]] Env env;
};

struct transform_results {
  template <typename Tag, typename... Datums>
    requires(std::is_nothrow_move_constructible_v<Datums> && ...)
  static auto apply(result_t<Tag, Datums...>)
      -> completion_signatures<result_t<Tag, Datums...>>;

  template <typename Tag, typename... Datums>
  static auto apply(result_t<Tag, Datums...>) -> completion_signatures<
                                                  result_t<Tag, Datums...>,
                                                  error_t<std::exception_ptr>>;
};

template <typename Src, typename Env>
using result_variant_t = detail::completion_signatures_to_variant_of_tuple_t<
    transform_completion_signatures_t<
        completion_signatures_for_t<Src, Env>,
        transform_results>>;

}  // namespace sync_wait_detail

  template <typename Src, typename DrivableContext>
inline auto sync_wait(Src&& src, DrivableContext& loop) {
  using result_t = sync_wait_detail::result_variant_t<Src, empty_env>;
  sync_wait_detail::sync_wait_state<result_t, empty_env> state;
  auto op = std::forward<Src>(src).connect(state.get_receiver());
  op.start();

  loop.run(state.ss.get_token());
  return std::move(state.result);
}

template <typename Src>
inline auto sync_wait(Src&& src) {
  manual_event_loop loop;
  return squiz::sync_wait(std::forward<Src>(src), loop);
}

}  // namespace squiz
