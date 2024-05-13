///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>

namespace squiz {

template <typename T, typename... Args>
concept nothrow_constructible_from = std::constructible_from<T, Args...> &&
    std::is_nothrow_constructible_v<T, Args...>;

template <typename T>
concept nothrow_move_constructible =
    std::move_constructible<T> && nothrow_constructible_from<T, T>;

template <typename T>
concept decay_copyable = std::constructible_from<std::decay_t<T>, T>;

template <typename T>
concept nothrow_decay_copyable =
    decay_copyable<T> && nothrow_constructible_from<std::decay_t<T>, T>;

template <typename T, typename... Ts>
concept one_of = (std::same_as<T, Ts> || ...);

namespace detail {
template <typename T, template <typename...> class Template>
inline constexpr bool is_instance_of_v = false;

template <template <typename...> class Template, typename... Ts>
inline constexpr bool is_instance_of_v<Template<Ts...>, Template> = true;
}  // namespace detail

template <typename T, template <typename...> class Template>
concept instance_of = detail::is_instance_of_v<T, Template>;

}  // namespace squiz
