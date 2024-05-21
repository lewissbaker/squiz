///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/concepts.hpp>
#include <squiz/parameter_type.hpp>

namespace squiz {

/// Tag types used as the return-type in a completion-signature to indicate what
/// kind of signal/method this signature is.
struct value_tag {};
struct error_tag {};
struct stopped_tag {};

template <typename T>
concept completion_tag = one_of<T, value_tag, error_tag, stopped_tag>;

template <typename Tag, typename... Datums>
struct result_t;

///  Specialize result_t only for the combinations that are valid.
template <typename... Vs>
struct result_t<value_tag, Vs...> {};
template <typename E>
struct result_t<error_tag, E> {};
template <>
struct result_t<stopped_tag> {};

template <typename... Vs>
using value_t = result_t<value_tag, Vs...>;
template <typename E>
using error_t = result_t<error_tag, E>;
using stopped_t = result_t<stopped_tag>;

template <typename Tag, typename... Datums>
inline constexpr result_t<Tag, Datums...> result{};
template <typename... Vs>
inline constexpr value_t<Vs...> value{};
template <typename E>
inline constexpr error_t<E> error{};
inline constexpr stopped_t stopped{};

template <typename T>
concept completion_signature = instance_of<T, result_t>;

template <typename... Sigs>
struct completion_signatures {
  template <template <typename...> class F>
  using apply = F<Sigs...>;

  static constexpr std::size_t size = sizeof...(Sigs);
};

template <auto... Sigs>
using completion_signatures_t = completion_signatures<decltype(Sigs)...>;

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
        completion_signatures<Bs...>> {};

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
using transform_completion_signature_t = decltype(Transform::apply(Sig{}));

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
  static auto apply(result_t<Tag, Args...>)
      -> completion_signatures<result_t<Tag, Args...>>;

  template <typename Tag, typename... Args>
  static auto apply(result_t<Tag, Args...>) -> completion_signatures<>;
};
}  // namespace detail

template <typename Sigs>
using value_signatures_t =
    transform_completion_signatures_t<Sigs, detail::only_tags<value_tag>>;

template <typename Sigs>
using error_signatures_t =
    transform_completion_signatures_t<Sigs, detail::only_tags<error_tag>>;

template <typename Sigs>
using stopped_signatures_t =
    transform_completion_signatures_t<Sigs, detail::only_tags<stopped_tag>>;

template <typename Sigs>
using error_or_stopped_signatures_t = transform_completion_signatures_t<
    Sigs,
    detail::only_tags<error_tag, stopped_tag>>;

}  // namespace squiz
