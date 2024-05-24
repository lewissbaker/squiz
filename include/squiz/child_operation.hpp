///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <utility>

#include <squiz/manual_child_operation.hpp>

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
template <typename ParentOp, typename Env, typename Tag, typename Sender>
struct child_operation
  : public manual_child_operation<ParentOp, Env, Tag, Sender> {
private:
  using manual_base = manual_child_operation<ParentOp, Env, Tag, Sender>;

  // Make these operations private as this class manages calling them
  // in the constructor/destructor.
  using manual_base::construct;
  using manual_base::destruct;

protected:
  /// Constructs the child operation-state from the provide Sender.
  ///
  /// Connects a receiver to it that forwards receiver operations to
  /// member-function calls on the ParentOp class, passing the \c Tag object as
  /// the first parameter to allow distinguishing between operations coming from
  /// different children.
  explicit child_operation(Sender&& sender) noexcept(
      manual_base::is_nothrow_connectable) {
    this->construct(std::forward<Sender>(sender));
  }

  ~child_operation() { this->destruct(); }
};

}  // namespace squiz
