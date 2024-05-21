///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include <squiz/operation_state.hpp>
#include <squiz/request_stop.hpp>
#include <squiz/detail/connect_at.hpp>

namespace squiz {

/// CRTP base-class for operation_state types which can be used to store
/// child operation_state objects in a way that enables the child
/// operation_state to avoid having to store the receiver.
///
/// \example
/// \begincode
/// template<typename Receiver>
/// struct some_op :
///     inlinable_operation_state<some_op<Receiver>, Receiver>,
///     child_operation<some_op<Receiver>, some_tag, some_sender> {
///   // ...
/// };
/// \endcode
template <typename ParentOp, typename Tag, typename Sender>
struct child_operation {
private:
  class child_receiver final {
    // Define a wrapper environment type that can be returned from get_env()
    // to avoid needing get_env() deduce its return-type from operations on
    // the ParentOp, which will be incomplete at the point of instantiation
    // of the child_receiver class.
    struct env {
      template <typename Key>
      decltype(auto) query(Key key) const noexcept {
        return parent_op_->get_env(Tag{}).query(key);
      }

    private:
      friend child_receiver;

      explicit env(ParentOp* p) noexcept : parent_op_(p) {}

      ParentOp* parent_op_;
    };

  public:
    template <typename ChildOpState>
    static child_receiver make_receiver(ChildOpState* child_op) noexcept {
      static_assert(std::same_as<ChildOpState, child_operation::child_op_t>);
      auto* child_base = reinterpret_cast<child_operation*>(child_op);
      auto* parent_op = static_cast<ParentOp*>(child_base);
      return child_receiver(parent_op);
    }

    template <typename Signal, typename... Datums>
    void set_result(
        result_t<Signal, Datums...> sig,
        parameter_type<Datums>... datums) noexcept {
      parent_op_->set_result(
          Tag{}, sig, squiz::forward_parameter<Datums>(datums)...);
    }

    env get_env() const noexcept { return env{parent_op_}; }

  private:
    friend child_operation;

    explicit child_receiver(ParentOp* parent_op) noexcept
      : parent_op_(parent_op) {}

    ParentOp* parent_op_;
  };

  using child_op_t = connect_result_t<Sender, child_receiver>;

protected:
  static constexpr bool is_stoppable = stoppable_operation_state<child_op_t>;
  static constexpr bool is_nothrow_connectable =
      squiz::is_nothrow_connectable_v<Sender, child_receiver>;

  /// Constructs the child operation-state from the provide Sender.
  ///
  /// Connects a receiver to it that forwards receiver operations to
  /// member-function calls on the ParentOp class, passing the \c Tag object as
  /// the first parameter to allow distinguishing between operations coming from
  /// different children.
  explicit child_operation(Sender&& sender) noexcept(is_nothrow_connectable) {
    detail::connect_at(
        std::addressof(get()),
        std::forward<Sender>(sender),
        child_receiver(static_cast<ParentOp*>(this)));
  }

  child_operation(child_operation&&) = delete;

  ~child_operation() { get().~child_op_t(); }

  /// Starts the child operation-state
  void start() noexcept { get().start(); }

  /// Sends a stop-request to the child operation-state if it supports it.
  void request_stop() noexcept { squiz::request_stop(get()); }

private:
  child_op_t& get() noexcept {
    if constexpr (std::is_empty_v<child_op_t>) {
      return child_storage_;
    } else {
      return *reinterpret_cast<child_op_t*>(child_storage_);
    }
  }

  // In cases where child_op_t type is empty we ideally don't want to have
  // to store a byte array of size 1 here but would rather have the
  // 'child_operation' class be an empty type as well.
  //
  // As an empty child_op_t type should already be a standard layout type,
  // we can just declare a [[no_unique_address]] member of it inside a union.
  using storage_type = std::conditional_t<
      std::is_empty_v<child_op_t>,
      child_op_t,
      std::byte[sizeof(child_op_t)]>;

  union {
    [[no_unique_address]] alignas(child_op_t) storage_type child_storage_;
  };
};

}  // namespace squiz
