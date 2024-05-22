///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include <squiz/concepts.hpp>
#include <squiz/inlinable_operation_state.hpp>
#include <squiz/source_tag.hpp>
#include <squiz/variant_child_operation.hpp>
#include <squiz/detail/member_type.hpp>
#include <squiz/detail/smallest_unsigned_integer.hpp>

namespace squiz {

template <typename Receiver, typename... Senders>
struct variant_op final
  : inlinable_operation_state<variant_op<Receiver, Senders...>, Receiver>
  , variant_child_operation<
        variant_op<Receiver, Senders...>,
        receiver_env_t<Receiver>,
        indexed_source_tag,
        Senders...> {
private:
  using inlinable_base = inlinable_operation_state<variant_op, Receiver>;
  using variant_base = variant_child_operation<
      variant_op,
      receiver_env_t<Receiver>,
      indexed_source_tag,
      Senders...>;

public:
  template <std::size_t Idx>
  variant_op(
      std::in_place_index_t<Idx>,
      Senders...[Idx] && src,
      Receiver
          rcvr) noexcept(variant_base::template is_nothrow_constructible<Idx>)
    : inlinable_base(std::move(rcvr))
    , sender_index_(Idx) {
    variant_base::template construct<Idx>(std::forward<Senders...[Idx]>(src));
  }

  ~variant_op() { variant_base::destruct(sender_index_); }

  void start() noexcept { variant_base::start(sender_index_); }

  void request_stop() noexcept
    requires variant_base::is_any_stoppable
  {
    variant_base::request_stop(sender_index_);
  }

  template <std::size_t Idx, typename SignalTag, typename... Datums>
  void set_result(
      indexed_source_tag<Idx>,
      result_t<SignalTag, Datums...> sig,
      parameter_type<Datums>... datums) noexcept {
    squiz::set_result(
        this->get_receiver(), sig, squiz::forward_parameter<Datums>(datums)...);
  }

  template <std::size_t Idx>
  receiver_env_t<Receiver> get_env(indexed_source_tag<Idx>) noexcept {
    return this->get_receiver().get_env();
  }

private:
  detail::smallest_unsigned_integer_for<sizeof...(Senders) - 1> sender_index_;
};

/// A variant_sender is a sender that can be one of several possible
/// sender types.
///
/// It can potentially complete with the results of any of the alternative
/// sender types.
template <typename... Senders>
  requires(sizeof...(Senders) != 0)
struct variant_sender {
  template <typename Self, typename... Envs>
  auto get_completion_signatures(this Self&&, Envs...)
      -> merge_completion_signatures_t<completion_signatures_for_t<
          detail::member_type_t<Self, Senders>,
          Envs...>...>;

  template <typename Self, typename... Envs>
  auto is_always_nothrow_connectable(this Self&&, Envs...)
      -> std::bool_constant<
          (squiz::is_always_nothrow_connectable_v<
               detail::member_type_t<Self, Senders>,
               Envs...> &&
           ...)>;

  template <
      typename Source,
      std::size_t Idx =
          detail::index_of_v<std::remove_cvref_t<Source>, Senders...>>
    requires std::constructible_from<std::remove_cvref_t<Source>, Source>
  variant_sender(Source&& src) noexcept(
      std::is_nothrow_constructible_v<std::remove_cvref_t<Source>, Source>)
    : sender_index_(Idx) {
    std::construct_at(
        reinterpret_cast<std::remove_cvref_t<Source>*>(&storage_),
        std::forward<Source>(src));
  }

  template <
      std::size_t Idx,
      typename Source2,
      typename Source = Senders...[Idx]>
    requires std::constructible_from<Source, Source2>
  variant_sender(std::in_place_index_t<Idx>, Source2&& src) noexcept(
      std::is_nothrow_constructible_v<Source, Source2>)
    : sender_index_(Idx) {
    std::construct_at(
        reinterpret_cast<Source*>(&storage_), std::forward<Source2>(src));
  }

  variant_sender(variant_sender&& other) noexcept(
      (std::is_nothrow_move_constructible_v<Senders> && ...))
    : sender_index_(other.sender_index_) {
    detail::dispatch_index<sizeof...(Senders)>(
        [&]<std::size_t Idx>(std::integral_constant<std::size_t, Idx>) {
          std::construct_at(
              reinterpret_cast<Senders...[Idx]*>(&storage_),
              std::move(*reinterpret_cast<Senders...[Idx]*>(&other.storage_)));
        },
        sender_index_);
  }

  variant_sender(const variant_sender& other)  //
      noexcept((std::is_nothrow_copy_constructible_v<Senders> && ...))
    requires(std::copy_constructible<Senders> && ...)
    : sender_index_(other.sender_index_) {
    detail::dispatch_index<sizeof...(Senders)>(
        [&]<std::size_t Idx>(std::integral_constant<std::size_t, Idx>) {
          std::construct_at(
              reinterpret_cast<Senders...[Idx]*>(&storage_),
              *reinterpret_cast<const Senders...[Idx]*>(&other.storage_));
        },
        sender_index_);
  }

  ~variant_sender() {
    detail::dispatch_index<sizeof...(Senders)>(
        [&]<std::size_t Idx>(std::integral_constant<std::size_t, Idx>) {
          using sender_t = Senders...[Idx];
          reinterpret_cast<sender_t*>(&storage_)->~sender_t();
        },
        sender_index_);
  }

  template <typename Self, typename Receiver>
  auto connect(this Self&& self, Receiver rcvr) noexcept(
      (std::is_nothrow_constructible_v<
           variant_op<Receiver, detail::member_type_t<Self, Senders>...>,
           detail::member_type_t<Self, Senders>> &&
       ...)) -> variant_op<Receiver, detail::member_type_t<Self, Senders>...> {
    using op_t = variant_op<Receiver, detail::member_type_t<Self, Senders>...>;
    return detail::dispatch_index<sizeof...(Senders)>(
        [&]<std::size_t Idx>(std::integral_constant<std::size_t, Idx>) {
          using sender_t = Senders...[Idx];
          return op_t{
              std::in_place_index<Idx>,
              std::forward<detail::member_type_t<Self, sender_t>>(
                  *reinterpret_cast<sender_t*>(&self.storage_)),
              std::move(rcvr)};
        },
        self.sender_index_);
  }

private:
  detail::smallest_unsigned_integer_for<sizeof...(Senders) - 1> sender_index_;
  alignas(Senders...) std::byte storage_[std::max({sizeof(Senders)...})];
};  // namespace squiz

}  // namespace squiz
