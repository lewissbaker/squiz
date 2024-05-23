///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include <squiz/child_operation.hpp>
#include <squiz/completion_signatures.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/receiver.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/set_value_signature.hpp>

namespace squiz {

template <typename Source, typename Func, typename Receiver>
class then_op final
  : public inlinable_operation_state<then_op<Source, Func, Receiver>, Receiver>
  , public child_operation<
        then_op<Source, Func, Receiver>,
        receiver_env_t<Receiver>,
        source_tag,
        Source> {
  using inlinable_base = inlinable_operation_state<then_op, Receiver>;
  using child_base =
      child_operation<then_op, receiver_env_t<Receiver>, source_tag, Source>;

public:
  template <typename Func2>
  then_op(Source&& source, Func2&& func, Receiver receiver) noexcept(
      std::is_nothrow_constructible_v<Func, Func2> &&
      child_base::is_nothrow_connectable)
    : inlinable_base(std::move(receiver))
    , child_base(std::forward<Source>(source))
    , func(std::forward<Func2>(func)) {}

  using func_t = Func;

  template <typename Self>
  void start(this Self& self) noexcept {
    self.child_base::start();
  }

  void request_stop() noexcept
    requires child_base::is_stoppable
  {
    child_base::request_stop();
  }

  template <typename... Vs>
  void
  set_result(source_tag, value_t<Vs...>, parameter_type<Vs>... vs) noexcept {
    using result_t = std::invoke_result_t<Func, Vs...>;
    try {
      if constexpr (std::is_void_v<result_t>) {
        std::invoke(std::move(func), squiz::forward_parameter<Vs>(vs)...);
        squiz::set_value<>(this->get_receiver());
      } else {
        squiz::set_value<result_t>(
            this->get_receiver(),
            std::invoke(std::move(func), squiz::forward_parameter<Vs>(vs)...));
      }
    } catch (...) {
      if constexpr (!std::is_nothrow_invocable_v<Func, Vs...>) {
        squiz::set_error<std::exception_ptr>(
            this->get_receiver(), std::current_exception());
      }
    }
  }

  template <typename SignalTag, typename... Datums>
    requires(!std::same_as<SignalTag, value_tag>)
  void set_result(
      source_tag,
      result_t<SignalTag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    squiz::set_result(
        this->get_receiver(), sig, squiz::forward_parameter<Datums>(datums)...);
  }

  receiver_env_t<Receiver> get_env(source_tag) noexcept {
    return this->get_receiver().get_env();
  }

private:
  [[no_unique_address]] Func func;
};

template <typename Source, typename Func, typename Receiver>
then_op(Source&&, Func, Receiver) -> then_op<Source, Func, Receiver>;

namespace then_detail {

template <typename Func>
struct then_completion_transform {
  template <typename... Args>
    requires std::is_nothrow_invocable_v<Func, Args...>
  static auto apply(value_t<Args...>)
      -> completion_signatures<
          detail::set_value_signature_t<std::invoke_result_t<Func, Args...>>>;

  template <typename... Args>
  static auto apply(value_t<Args...>)
      -> completion_signatures<
          detail::set_value_signature_t<std::invoke_result_t<Func, Args...>>,
          error_t<std::exception_ptr>>;

  template <typename E>
  static auto apply(error_t<E>) -> completion_signatures<error_t<E>>;

  static auto apply(stopped_t) -> completion_signatures<stopped_t>;
};

}  // namespace then_detail

template <typename Source, typename Func>
struct then_sender {
  template <typename Self, typename... Envs>
  auto get_completion_signatures(this Self&&, Envs...)
      -> transform_completion_signatures_t<
          completion_signatures_for_t<
              detail::member_type_t<Self, Source>,
              Envs...>,
          then_detail::then_completion_transform<Func>>;

  template <typename Self, typename... Envs>
  auto is_always_nothrow_connectable(this Self&&, Envs...)
      -> std::bool_constant<
          (std::is_nothrow_constructible_v<
               Func,
               detail::member_type_t<Self, Func>> &&
           is_always_nothrow_connectable_v<
               detail::member_type_t<Self, Source>,
               Envs...>)>;

  [[no_unique_address]] Source source;
  [[no_unique_address]] Func func;

  template <typename Self, typename Receiver>
  auto connect(this Self&& self, Receiver receiver) noexcept(
      std::is_nothrow_constructible_v<
          then_op<detail::member_type_t<Self, Source>, Func, Receiver>,
          detail::member_type_t<Self, Source>,
          detail::member_type_t<Self, Func>,
          Receiver>)
      -> then_op<detail::member_type_t<Self, Source>, Func, Receiver> {
    return then_op<detail::member_type_t<Self, Source>, Func, Receiver>{
        std::forward<Self>(self).source,
        std::forward<Self>(self).func,
        std::move(receiver)};
  }
};

template <typename Source, typename Func>
then_sender(Source, Func) -> then_sender<Source, Func>;

}  // namespace squiz
