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

namespace squiz {

template <typename TrueSource, typename FalseSource, typename Receiver>
struct cond_op final
  : inlinable_operation_state<
        cond_op<TrueSource, FalseSource, Receiver>,
        Receiver>
  , variant_child_operation<
        cond_op<TrueSource, FalseSource, Receiver>,
        indexed_source_tag,
        TrueSource,
        FalseSource> {
private:
  using inlinable_base = inlinable_operation_state<cond_op, Receiver>;
  using variant_base = variant_child_operation<
      cond_op,
      indexed_source_tag,
      TrueSource,
      FalseSource>;

public:
  cond_op(std::true_type, TrueSource&& src, Receiver rcvr) noexcept(
      variant_base::template is_nothrow_constructible<0>)
    : inlinable_base(std::move(rcvr))
    , true_branch_(true) {
    variant_base::template construct<0>(std::forward<TrueSource>(src));
  }

  cond_op(std::false_type, FalseSource&& src, Receiver rcvr) noexcept(
      variant_base::template is_nothrow_constructible<1>)
    : inlinable_base(std::move(rcvr))
    , true_branch_(false) {
    variant_base::template construct<1>(std::forward<FalseSource>(src));
  }

  ~cond_op() {
    if (true_branch_) {
      variant_base::template destroy<0>();
    } else {
      variant_base::template destroy<1>();
    }
  }

  void start() noexcept {
    if (true_branch_) {
      variant_base::template start<0>();
    } else {
      variant_base::template start<1>();
    }
  }

  void request_stop() noexcept
    requires variant_base::is_any_stoppable
  {
    if (true_branch_) {
      if constexpr (variant_base::template is_stoppable<0>) {
        variant_base::template request_stop<0>();
      }
    } else {
      if constexpr (variant_base::template is_stoppable<1>) {
        variant_base::template request_stop<1>();
      }
    }
  }

  template <std::size_t Idx, typename... Vs>
  void set_value(indexed_source_tag<Idx>, Vs&&... vs) noexcept {
    this->get_receiver().set_value(std::forward<Vs>(vs)...);
  }

  template <std::size_t Idx, typename E>
  void set_error(indexed_source_tag<Idx>, E&& e) noexcept {
    this->get_receiver().set_error(std::forward<E>(e));
  }

  template <std::size_t Idx>
  void set_stopped(indexed_source_tag<Idx>) noexcept {
    this->get_receiver().set_stopped();
  }

  template <std::size_t Idx>
  auto get_env(indexed_source_tag<Idx>) noexcept {
    return this->get_receiver().get_env();
  }

private:
  bool true_branch_;
};

/// A cond_sender is a sender that can either be a TrueSender
/// or a FalseSender.
///
/// This is a simple kind of `variant_sender`.
template <typename TrueSource, typename FalseSource>
struct cond_sender {
  template <typename Self, typename... Envs>
  auto get_completion_signatures(this Self&&, Envs...)
      -> merge_completion_signatures_t<
          completion_signatures_for_t<
              detail::member_type_t<Self, TrueSource>,
              Envs...>,
          completion_signatures_for_t<
              detail::member_type_t<Self, FalseSource>,
              Envs...>>;

  template <typename TrueSource2>
    requires std::constructible_from<TrueSource, TrueSource2>
  cond_sender(std::true_type, TrueSource2&& src) noexcept(
      std::is_nothrow_constructible_v<TrueSource, TrueSource2>)
    : true_branch_(true)
    , true_src_(std::forward<TrueSource2>(src)) {}

  template <typename FalseSource2>
    requires std::constructible_from<FalseSource, FalseSource2>
  cond_sender(std::false_type, FalseSource2&& src) noexcept(
      std::is_nothrow_constructible_v<FalseSource, FalseSource2>)
    : true_branch_(false)
    , false_src_(std::forward<FalseSource2>(src)) {}

  cond_sender(cond_sender&& other) noexcept(
      std::is_nothrow_move_constructible_v<TrueSource> &&
      std::is_nothrow_move_constructible_v<FalseSource>)
    : true_branch_(other.true_branch_) {
    if (true_branch_) {
      std::construct_at(std::addressof(true_src_), std::move(other).true_src_);
    } else {
      std::construct_at(
          std::addressof(false_src_), std::move(other).false_src_);
    }
  }

  cond_sender(const cond_sender&& other)  //
      noexcept(
          std::is_nothrow_copy_constructible_v<TrueSource> &&
          std::is_nothrow_copy_constructible_v<FalseSource>)  //
    requires std::copy_constructible<TrueSource> &&
      std::copy_constructible<FalseSource>  //
    : true_branch_(other.true_branch_) {
    if (true_branch_) {
      std::construct_at(std::addressof(true_src_), other.true_src_);
    } else {
      std::construct_at(std::addressof(false_src_), other.false_src_);
    }
  }

  ~cond_sender() {
    if (true_branch_) {
      true_src_.~TrueSource();
    } else {
      false_src_.~FalseSource();
    }
  }

  template <typename Self, typename Receiver>
  auto connect(this Self&& self, Receiver rcvr) noexcept(
      std::is_nothrow_constructible_v<
          cond_op<
              detail::member_type_t<Self, TrueSource>,
              detail::member_type_t<Self, FalseSource>,
              Receiver>,
          std::true_type,
          detail::member_type_t<Self, TrueSource>> &&
      std::is_nothrow_constructible_v<
          cond_op<
              detail::member_type_t<Self, TrueSource>,
              detail::member_type_t<Self, FalseSource>,
              Receiver>,
          std::false_type,
          detail::member_type_t<Self, FalseSource>>) {
    using op_t = cond_op<
        detail::member_type_t<Self, TrueSource>,
        detail::member_type_t<Self, FalseSource>,
        Receiver>;
    if (self.true_branch_) {
      return op_t(
          std::true_type{},
          std::forward<Self>(self).true_src_,
          std::move(rcvr));
    } else {
      return op_t(
          std::false_type{},
          std::forward<Self>(self).false_src_,
          std::move(rcvr));
    }
  }

private:
  bool true_branch_;
  union {
    [[no_unique_address]] TrueSource true_src_;
    [[no_unique_address]] FalseSource false_src_;
  };
};

/// Helper function for constructing a cond_sender from either
/// a 'true_src' sender or a 'false_src' sender.
template <typename TrueSource, typename FalseSource>
auto cond(
    bool condition,
    TrueSource&& true_src,
    FalseSource&& false_src)  //
    noexcept(
        nothrow_decay_copyable<TrueSource> &&
        nothrow_decay_copyable<FalseSource>)  //
    -> cond_sender<
        std::remove_cvref_t<TrueSource>,
        std::remove_cvref_t<FalseSource>> {
  if (condition) {
    return {std::true_type{}, std::forward<TrueSource>(true_src)};
  } else {
    return {std::false_type{}, std::forward<FalseSource>(false_src)};
  }
}

// Specialisation for the case where both TrueSource and FalseSource
// are the same type and thus we don't need a cond_sender to allow
// holding either type. We can just return the same type as the input,
// and only choose which of the arguments to return.
template <typename TrueSource, typename FalseSource>
  requires std::same_as<
               std::remove_cvref_t<TrueSource>,
               std::remove_cvref_t<FalseSource>>
auto cond(bool condition, TrueSource&& true_src, FalseSource&& false_src) noexcept(
    nothrow_decay_copyable<TrueSource> &&
    nothrow_decay_copyable<FalseSource>) -> std::remove_cvref_t<TrueSource> {
  if (condition) {
    return std::forward<TrueSource>(true_src);
  } else {
    return std::forward<FalseSource>(false_src);
  }
}

}  // namespace squiz
