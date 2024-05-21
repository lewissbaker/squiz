#pragma once

#include <type_traits>

namespace squiz {

/// Trait that queries whether a prvalue of a particular type is best
/// passed to a function by value or by rvalue-reference.
///
/// The default trait uses a simple heuristic that does the right thing for most
/// types.
///
/// Users may customize this template for their own types to override the
/// default behaviour as necessary.
template <typename T>
inline constexpr bool enable_pass_by_value =
    std::is_trivially_copy_constructible_v<T> &&
    std::is_trivially_move_constructible_v<T> &&
    std::is_trivially_destructible_v<T> && (sizeof(T) <= (2 * sizeof(void*)));

/// Compute the parameter-type to use when passing a value of type \c T.
template <typename T>
using parameter_type = std::conditional_t<enable_pass_by_value<T>, T, T&&>;

/// Forwards an lvalue reference to a parameter of type parameter_type<T>
/// to a function that expects a parameter of type parameter_type<T>.
///
/// This is similar to \c std::forward<T>(x) but instead of forwarding \c T&&
/// parameters, it is for forwarding \c parameter_type<T> parameters.
template <typename T>
parameter_type<T> forward_parameter(T& x) noexcept {
  return static_cast<T&&>(x);
}

}  // namespace squiz
