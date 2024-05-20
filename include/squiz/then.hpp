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
#include <squiz/source_tag.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/set_value_signature.hpp>

namespace squiz {

template <typename Source, typename Func, typename Receiver>
class then_op final
  : public inlinable_operation_state<then_op<Source, Func, Receiver>, Receiver>
  , public child_operation<
        then_op<Source, Func, Receiver>,
        source_tag,
        Source> {
  using inlinable_base = inlinable_operation_state<then_op, Receiver>;
  using child_base = child_operation<then_op, source_tag, Source>;

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
  void set_value(source_tag, Vs&&... vs) noexcept {
    using result_t = std::invoke_result_t<Func, Vs...>;
    try {
      if constexpr (std::is_void_v<result_t>) {
        std::invoke(std::move(func), std::forward<Vs>(vs)...);
        this->get_receiver().set_value();
      } else {
        this->get_receiver().set_value(
            std::invoke(std::move(func), std::forward<Vs>(vs)...));
      }
    } catch (...) {
      if constexpr (!std::is_nothrow_invocable_v<Func, Vs...>) {
        this->get_receiver().set_error(std::current_exception());
      }
    }
  }

  template <typename E>
  void set_error(source_tag, E&& e) noexcept {
    this->get_receiver().set_error(std::forward<E>(e));
  }

  void set_stopped(source_tag) noexcept { this->get_receiver().set_stopped(); }

  auto get_env(source_tag) noexcept { return this->get_receiver().get_env(); }

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
  static auto apply(set_value_t (*)(Args...))
      -> completion_signatures<
          detail::set_value_signature_t<std::invoke_result_t<Func, Args...>>>;

  template <typename... Args>
  static auto apply(set_value_t (*)(Args...))
      -> completion_signatures<
          detail::set_value_signature_t<std::invoke_result_t<Func, Args...>>,
          set_error_t(std::exception_ptr)>;

  template <typename E>
  static auto
      apply(set_error_t (*)(E)) -> completion_signatures<set_error_t(E)>;

  static auto
      apply(set_stopped_t (*)()) -> completion_signatures<set_stopped_t()>;
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
