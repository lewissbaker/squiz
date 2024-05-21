///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <squiz/operation_state.hpp>
#include <squiz/request_stop.hpp>
#include <squiz/sender.hpp>
#include <squiz/detail/connect_at.hpp>
#include <squiz/detail/dispatch_index.hpp>
#include <squiz/detail/index_of.hpp>

namespace squiz {

/// CRTP base-class for operation_state types that allows an implementation
/// to have a union of child operation_state types stored as a sub-object in a
/// way that allows those child operations to avoid storing the receiver by
/// instead computing the receiver on-demand from the address of the child
/// operation-state.
///
/// This class provides the storage for the any of the operation-states of the
/// specified types in the \c Senders pack. The lifetimes of these
/// operation-states must be manually managed by the derived class by calling
/// the construct<Idx>() and destruct<Idx>() methods where Senders...[Idx] is
/// the sender type used to construct that operation-state.
///
/// Child senders will be connected with a receiver that forwards its operations
/// to method-calls on the parent operation-state, passing a \c Tag<Idx> object
/// as the first argument to allow distinguishing which child operation_state
/// the signal came from.
///
/// Use this to store child operation-states that do not have overlapping
/// lifetimes in a way that allows the storage to be reused.
///
/// \tparam ParentOp
/// The type of the operation_state that is inherting from this class.
///
/// \tparam Tag
/// A tag-type template that will be instantiated with Idx and passed to the
///
/// \tparam Senders
/// A list of sender-types that might be used to construct the child
/// operation_state. Only one of these operation_state objects may be
/// constructed at a time.
template <
    typename ParentOp,
    template <std::size_t>
    class Tag,
    typename... Senders>
class variant_child_operation {
  template <std::size_t Idx>
  struct child_receiver {
    using tag_t = Tag<Idx>;

  public:
    template <typename ChildOpState>
    static child_receiver make_receiver(ChildOpState* child_op) noexcept {
      static_assert(std::same_as<ChildOpState, child_op_t<Idx>>);
      static_assert(std::derived_from<ParentOp, variant_child_operation>);
      static_assert(std::is_standard_layout_v<variant_child_operation>);
      auto* child_base = reinterpret_cast<variant_child_operation*>(child_op);
      auto* parent_op = static_cast<ParentOp*>(child_base);
      return child_receiver(parent_op);
    }

    template <typename Signal, typename... Datums>
    void set_result(
        result_t<Signal, Datums...> sig,
        parameter_type<Datums>... datums) noexcept {
      parent_op_->set_result(
          tag_t{}, sig, squiz::forward_parameter<Datums>(datums)...);
    }

    auto get_env() const noexcept { return parent_op_->get_env(tag_t{}); }

  private:
    friend variant_child_operation;

    explicit child_receiver(ParentOp* parent_op) noexcept
      : parent_op_(parent_op) {}

    ParentOp* parent_op_;
  };

  template <std::size_t Idx>
  using child_op_t = connect_result_t<Senders...[Idx], child_receiver<Idx>>;

protected:
  /// Query the number of different operation-state types that this child
  /// operation can hold.
  static constexpr std::size_t sender_count = sizeof...(Senders);

  /// Query if a paricular child operation_state is stoppable.
  template <std::size_t Idx>
  static constexpr bool is_stoppable =
      stoppable_operation_state<child_op_t<Idx>>;

  /// Query the index of the \c operation_state for the specified \c Sender.
  ///
  /// Use the result of this as the \c Idx template parameter for the
  /// construct/destruct/start/request_stop functions.
  template <typename Sender>
    requires one_of<Sender, Senders...>
  static constexpr std::size_t sender_index =
      detail::index_of_v<Sender, Senders...>;

  /// Query if constructing a particular child operation state is potentially
  /// throwing.
  template <std::size_t Idx>
  static constexpr bool is_nothrow_constructible =
      nothrow_connectable_to<Senders...[Idx], child_receiver<Idx>>;

  /// A no-op. Leaves the child operation_state storage uninitialized.
  ///
  /// Call \c construct<Idx>() to construct a particular child operation_state
  /// object.
  variant_child_operation() noexcept = default;
  ~variant_child_operation() noexcept = default;

  template <std::size_t Idx>
  void
  construct(Senders...[Idx] && sender) noexcept(is_nothrow_constructible<Idx>) {
    ParentOp* parent_op = static_cast<ParentOp*>(this);
    detail::connect_at(
        reinterpret_cast<child_op_t<Idx>*>(&op_storage_),
        std::forward<Senders...[Idx]>(sender),
        child_receiver<Idx>(parent_op));
  }

  template <std::size_t Idx>
  void destruct() noexcept {
    get<Idx>().~child_op_t<Idx>();
  }

  void destruct(std::size_t index) noexcept {
    detail::dispatch_index<sender_count>(
        [this]<std::size_t Idx>(
            std::integral_constant<std::size_t, Idx>) noexcept {
          this->destruct<Idx>();
        },
        index);
  }

  template <std::size_t Idx>
  void start() noexcept {
    get<Idx>().start();
  }

  void start(std::size_t index) noexcept {
    detail::dispatch_index<sender_count>(
        [this]<std::size_t Idx>(
            std::integral_constant<std::size_t, Idx>) noexcept {
          this->start<Idx>();
        },
        index);
  }

  template <std::size_t Idx>
  void request_stop() noexcept {
    squiz::request_stop(get<Idx>());
  }

  void request_stop(std::size_t index) noexcept {
    detail::dispatch_index<sender_count>(
        [this]<std::size_t Idx>(
            std::integral_constant<std::size_t, Idx>) noexcept {
          this->request_stop<Idx>();
        },
        index);
  }

private:
  template <std::size_t Idx>
  child_op_t<Idx>& get() noexcept {
    return *reinterpret_cast<child_op_t<Idx>*>(&op_storage_);
  }

  template <std::size_t... Ids>
  static consteval bool is_union_empty(std::index_sequence<Ids...>) noexcept {
    return (std::is_empty_v<child_op_t<Ids>> && ...);
  }

  // Define max() locally to avoid pulling in <algorithm>
  static consteval std::size_t
  max(std::initializer_list<std::size_t> elements) noexcept {
    std::size_t result = 0;
    for (std::size_t x : elements) {
      if (x > result)
        result = x;
    }
    return result;
  }

  template <std::size_t... Ids>
  static consteval std::size_t
  union_size(std::index_sequence<Ids...>) noexcept {
    return max({sizeof(child_op_t<Ids>)...});
  }

  template <std::size_t... Ids>
  static consteval std::size_t
  union_alignment(std::index_sequence<Ids...>) noexcept {
    return max({alignof(child_op_t<Ids>)...});
  }

  template <std::size_t... Ids>
  static consteval bool any_stoppable(std::index_sequence<Ids...>) noexcept {
    return (is_stoppable<Ids> || ...);
  }

  using ids_t = std::index_sequence_for<Senders...>;

  static consteval bool is_union_empty() noexcept {
    return is_union_empty(ids_t{});
  }

  static consteval std::size_t union_size() noexcept {
    return union_size(ids_t{});
  }

  static consteval std::size_t union_alignment() noexcept {
    return union_alignment(ids_t{});
  }

protected:
  static constexpr bool is_any_stoppable = any_stoppable(ids_t{});

private:
  struct empty {};

  using storage_array =
      std::conditional_t<is_union_empty(), empty, std::byte[union_size()]>;

  [[no_unique_address]] alignas(union_alignment()) storage_array op_storage_;
};

}  // namespace squiz
