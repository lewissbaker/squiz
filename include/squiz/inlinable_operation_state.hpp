///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <utility>

namespace squiz {

/// CRTP base-class that operation_state types can inherit from if they want to
/// opt-in to the protocol that allows it to avoid storing a receiver in
/// cases where the receiver can be reconstructed from the address of the
/// operation-state.
///
/// In cases where an operation-state is a sub-object of a parent
/// operation-state and the receiver that the parent operation-state passed to
/// the child operation-state just contains the address of the parent
/// operation-state we can potentially compute the address of the parent
/// operation-state by subtracting an offset from the address of the child
/// operation-state and then construct the receiver on-demand rather than having
/// to store it.
///
/// If a parent operation-state also opts-in to the protocol by passing a
/// receiver type that has a static 'make_receiver' function that accepts a
/// pointer to the child operation-state's object, then the receiver does not
/// need to be stored. Otherwise, if the parent operation-state does not opt-in
/// to this protocol then this class falls back to just storing the receiver as
/// a data-member.
///
/// Derived classes obtain the receiver by calling \c get_receiver().
///
/// \tparam Derived
/// The type of the operation-state that is inheriting from this base-class.
///
/// \tparam Receiver
/// The type of the receiver that needs to be stored/reconstructed.
template <typename Derived, typename Receiver>
struct inlinable_operation_state {
protected:
  explicit inlinable_operation_state(Receiver&& r) noexcept
    : receiver_(std::move(r)) {}

  inlinable_operation_state(inlinable_operation_state&&) = delete;

  Receiver& get_receiver() noexcept {
    static_assert(std::derived_from<Derived, inlinable_operation_state>);
    return receiver_;
  }

private:
  [[no_unique_address]] Receiver receiver_;
};

// Specialisation for the case where the receiver can be reconstructed from the
// address of the derived class.
template <typename Derived, typename Receiver>
  requires requires(Derived* d) {
    { Receiver::make_receiver(d) } noexcept -> std::same_as<Receiver>;
  }
struct inlinable_operation_state<Derived, Receiver> {
protected:
  explicit inlinable_operation_state(Receiver&&) noexcept {}

  inlinable_operation_state(inlinable_operation_state&&) = delete;

  Receiver get_receiver() noexcept {
    static_assert(std::derived_from<Derived, inlinable_operation_state>);
    return Receiver::make_receiver(static_cast<Derived*>(this));
  }
};

}  // namespace squiz
