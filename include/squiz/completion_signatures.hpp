///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/concepts.hpp>

#include <tuple>
#include <variant>

namespace squiz {

/// Tag types used as the return-type in a completion-signature to indicate what
/// kind of signal/method this signature is.
struct set_value_t {};
struct set_error_t {};
struct set_stopped_t {};

template <typename T>
concept completion_tag = one_of<T, set_value_t, set_error_t, set_stopped_t>;

namespace detail {
template <typename T>
inline constexpr bool is_completion_signature = false;
template <typename... Vs>
inline constexpr bool is_completion_signature<set_value_t(Vs...)> = true;
template <typename E>
inline constexpr bool is_completion_signature<set_error_t(E)> = true;
template <>
inline constexpr bool is_completion_signature<set_stopped_t()> = true;
}  // namespace detail

/// Concept that matches a valid completion-signature.
///
/// A completion-signature is a function type where the return-type is a
/// 'completion_tag' and the parameter types are the
template <typename T>
concept completion_signature = detail::is_completion_signature<T>;

/// A set of completion-signatures
///
/// \tparam Sigs
/// The completion-signatures.
/// Must not contain any duplicate signatures.
template <completion_signature... Sigs>
struct completion_signatures {
  /// Apply the meta-function F to the completion-signatures.
  template <template <typename...> class F>
  using apply = F<Sigs...>;
};

/// A meta-function for merging a list of completion_signatures types into a
/// single completion_signatures type, taking a union of the sets of individual
/// completion signatures.
template <typename... SigSets>
struct merge_completion_signatures;

/// A convenience alias for getting the result of evaluating the \c
/// merge_completion_signatures meta-function.
template <typename... SigSets>
using merge_completion_signatures_t =
    typename merge_completion_signatures<SigSets...>::type;

// Case: Empty list
template <>
struct merge_completion_signatures<> {
  using type = completion_signatures<>;
};

// Case: One completion_signatures
template <typename SigSet>
struct merge_completion_signatures<SigSet> {
  using type = SigSet;
};

// Case: Two completion signatures
template <typename... As, typename B0, typename... Bs>
requires one_of<B0, As...>
struct merge_completion_signatures<
    completion_signatures<As...>,
    completion_signatures<B0, Bs...>>
  : merge_completion_signatures<
        completion_signatures<As...>,
        completion_signatures<Bs...>> {
};

template <typename... As, typename B0, typename... Bs>
struct merge_completion_signatures<
    completion_signatures<As...>,
    completion_signatures<B0, Bs...>>
  : merge_completion_signatures<
        completion_signatures<As..., B0>,
        completion_signatures<Bs...>> {};

template <typename SigSet>
struct merge_completion_signatures<SigSet, completion_signatures<>> {
  using type = SigSet;
};

// Case: More than two completion signatures
template <typename A, typename B, typename C, typename... Rest>
struct merge_completion_signatures<A, B, C, Rest...>
  : merge_completion_signatures<
        typename merge_completion_signatures<A, B>::type,
        C,
        Rest...> {};

/// Meta-function for applying a transform to a completion_signature type
/// that maps the input completion-signature to an output completion_signatures.
///
/// \tparam Sig
/// The input completion signature.
///
/// \tparam Transform
/// A struct that has a nested static \c Transform::apply() function that takes
/// a function-pointer of each of the input completion-signature types and whose
/// return-type must be a new \c completion_signatures type.
/// Overload-resolution is used as a convenient pattern-matching facility for
/// matching completion-signatures and computing an output set of signatures
/// for each case.
template <completion_signature Sig, typename Transform>
using transform_completion_signature_t =
    decltype(Transform::apply(static_cast<Sig*>(nullptr)));

/// Meta-function for applying a Transform to every element in a set of
/// completion-signatures and then taking the union of the resulting sets of
/// completion_signatures.
template <typename SigSet, typename Transform>
struct transform_completion_signatures {
private:
  template <typename... Sigs>
  using impl = merge_completion_signatures_t<
      transform_completion_signature_t<Sigs, Transform>...>;

public:
  using type = typename SigSet::template apply<impl>;
};

template <typename Sigs, typename Transform>
using transform_completion_signatures_t =
    typename transform_completion_signatures<Sigs, Transform>::type;

namespace detail {
template <typename... Tags>
struct only_tags {
  template <typename Tag, typename... Args>
  requires one_of<Tag, Tags...>
  static auto apply(Tag (*)(Args...)) -> completion_signatures<Tag(Args...)>;

  template <typename Tag, typename... Args>
  static auto apply(Tag (*)(Args...)) -> completion_signatures<>;
};
}  // namespace detail

template <typename Sigs>
using value_signatures_t =
    transform_completion_signatures_t<Sigs, detail::only_tags<set_value_t>>;

template <typename Sigs>
using error_signatures_t =
    transform_completion_signatures_t<Sigs, detail::only_tags<set_error_t>>;

template <typename Sigs>
using stopped_signatures_t =
    transform_completion_signatures_t<Sigs, detail::only_tags<set_stopped_t>>;

template <typename Sigs>
using error_or_stopped_signatures_t = transform_completion_signatures_t<
    Sigs,
    detail::only_tags<set_error_t, set_stopped_t>>;

/// Meta-function that maps a completion-signature of the form 'Tag(Args...)' to
/// the type 'std::tuple<Tag, Args...>'
template <typename Sig>
struct completion_signature_to_tuple;

template <typename Tag, typename... Args>
struct completion_signature_to_tuple<Tag(Args...)> {
  using type = std::tuple<Tag, Args...>;
};

template <typename Sig>
using completion_signature_to_tuple_t =
    typename completion_signature_to_tuple<Sig>::type;

/// Meta-function that maps a set of completion-signatures to a std::variant
/// std::monostate and a std::tuple<Tag, Args...> entry, one for each input
/// completion-signature.
template <typename Sigs>
struct completion_signatures_to_variant_of_tuple;

template <typename... Sigs>
struct completion_signatures_to_variant_of_tuple<
    completion_signatures<Sigs...>> {
  using type =
      std::variant<std::monostate, completion_signature_to_tuple_t<Sigs>...>;
};

template <typename Sigs>
using completion_signatures_to_variant_of_tuple_t =
    typename completion_signatures_to_variant_of_tuple<Sigs>::type;

}  // namespace squiz
