///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <concepts>
#include <type_traits>

#include <squiz/concepts.hpp>

namespace squiz {

template <typename... Keys>
struct key_list {
  template <typename Key>
  static constexpr bool contains = one_of<Key, Keys...>;
};

template <typename List, typename T>
struct key_list_add {
  using type = List;
};

template <typename... Ts, typename T>
  requires(!one_of<T, Ts...>)
struct key_list_add<key_list<Ts...>, T> {
  using type = key_list<Ts..., T>;
};

template <typename List, typename T>
using key_list_add_t = typename key_list_add<List, T>::type;

template <typename T>
concept queryable = std::is_object_v<T> && requires {
  typename T::property_keys;
  requires instance_of<typename T::property_keys, key_list>;
};

template <typename T>
concept property_key = std::is_empty_v<T> && std::constructible_from<T> &&
    std::copy_constructible<T>;

template <typename T, typename Key>
concept queryable_for = queryable<T> && property_key<Key> &&
    T::property_keys::template contains<Key> &&
    requires(const T& q, const Key key) {
      q.query(key);
      key(q);
    };

}  // namespace squiz
