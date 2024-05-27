///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace squiz::detail {

template <typename Allocator>
struct allocator_memory_deleter {
  using alloc_traits = std::allocator_traits<Allocator>;

  [[no_unique_address]] Allocator allocator;

  void operator()(typename alloc_traits::pointer ptr) noexcept {
    alloc_traits::deallocate(allocator, ptr, 1);
  }
};

/// Allocate memory using an allocator and return a unique_ptr that owns
/// that memory.
///
/// Note that the pointer points to uninitialized memory.
template <typename T, typename Allocator>
  requires std::
      same_as<T, typename std::allocator_traits<Allocator>::value_type>
    constexpr std::unique_ptr<T, allocator_memory_deleter<Allocator>>
    allocate_memory_unique(Allocator allocator) noexcept(
        noexcept(std::allocator_traits<Allocator>::allocate(allocator, 1))) {
  using alloc_traits = std::allocator_traits<Allocator>;
  using deleter = allocator_memory_deleter<Allocator>;

  auto ptr = alloc_traits::allocate(allocator, 1);
  return std::unique_ptr<T, deleter>(ptr, deleter{std::move(allocator)});
}

template <typename Allocator>
struct allocator_deleter {
  using alloc_traits = std::allocator_traits<Allocator>;

  [[no_unique_address]] Allocator allocator;

  void operator()(typename alloc_traits::pointer ptr) noexcept {
    alloc_traits::destroy(allocator, ptr);
    alloc_traits::deallocate(allocator, ptr, 1);
  }
};

/// Allocate and construct a new object of type, T, using the specified
/// allocator and return a std::unique_ptr pointing to that allocated object.
///
/// \tparam T
/// The type of the object to construct.
///
/// \tparam Allocator
/// The type of the allocator to use to allocate storage.
///
/// \tparam Args...
/// The types of arguments to pass to \c T's constructor.
template <typename T, typename Allocator, typename... Args>
  requires std::same_as<
               T,
               typename std::allocator_traits<Allocator>::value_type> &&
    std::constructible_from<T, Args...>
constexpr std::unique_ptr<T, allocator_deleter<Allocator>>
allocate_unique(Allocator allocator, Args&&... args) noexcept(
    noexcept(detail::allocate_memory_unique<T>(allocator)) &&
    std::is_nothrow_constructible_v<T, Args...>) {
  using alloc_traits = std::allocator_traits<Allocator>;
  using deleter = allocator_deleter<Allocator>;

  auto ptr = detail::allocate_memory_unique<T>(allocator);
  Allocator& alloc_ref = ptr.get_deleter().allocator;
  alloc_traits::construct(alloc_ref, ptr, std::forward<Args>(args)...);
  return std::unique_ptr<T, deleter>(
      ptr.release(), deleter{std::move(alloc_ref)});
}

}  // namespace squiz::detail
