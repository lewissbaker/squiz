///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/child_operation.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/parameter_type.hpp>
#include <squiz/receiver.hpp>
#include <squiz/sender.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/detail/env_with_stop_possible.hpp>
#include <squiz/detail/member_type.hpp>

#include <type_traits>
#include <utility>

namespace squiz {

template <typename Source, typename Receiver>
class unstoppable_op
  : public inlinable_operation_state<unstoppable_op<Source, Receiver>, Receiver>
  , public child_operation<
        unstoppable_op<Source, Receiver>,
        detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>, false>,
        source_tag,
        Source> {
  using env =
      detail::make_env_with_stop_possible_t<receiver_env_t<Receiver>, false>;
  using inlinable_base = inlinable_operation_state<unstoppable_op, Receiver>;
  using child_base = child_operation<unstoppable_op, env, source_tag, Source>;

public:
  unstoppable_op(Source&& source, Receiver r) noexcept(
      child_base::is_nothrow_connectable)
    : inlinable_base(std::move(r))
    , child_base(std::forward<Source>(source)) {}

  void start() noexcept { child_base::start(); }

  template <typename Tag, typename... Datums>
  void set_result(
      source_tag,
      result_t<Tag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    squiz::set_result(
        this->get_receiver(), sig, squiz::forward_parameter<Datums>(datums)...);
  }

  env get_env(source_tag) const noexcept {
    return env{this->get_receiver().get_env()};
  }
};

template <typename Source>
struct unstoppable_sender {
  Source source;

  template <typename Self, typename... Env>
  auto get_completion_signatures(this Self&&, Env...)
      -> completion_signatures_for_t<
          detail::member_type_t<Self, Source>,
          detail::make_env_with_stop_possible_t<Env, false>...>;

  template <typename Self, typename... Env>
  auto is_always_nothrow_connectable(this Self&&, Env...)
      -> std::bool_constant<is_always_nothrow_connectable_v<
          detail::member_type_t<Self, Source>,
          detail::make_env_with_stop_possible_t<Env, false>...>>;

  template <typename Self, typename Receiver>
  unstoppable_op<detail::member_type_t<Self, Source>, Receiver>
  connect(this Self&& self, Receiver r) noexcept(
      std::is_nothrow_constructible_v<
          unstoppable_op<detail::member_type_t<Self, Source>, Receiver>,
          detail::member_type_t<Self, Source>,
          Receiver>) {
    return {std::forward<Self>(self).source, std::move(r)};
  }
};

template <typename Source>
unstoppable_sender(Source) -> unstoppable_sender<Source>;

}  // namespace squiz
