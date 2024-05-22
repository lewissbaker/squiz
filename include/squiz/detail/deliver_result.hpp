///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cassert>
#include <tuple>
#include <utility>
#include <variant>

#include <squiz/overload.hpp>
#include <squiz/receiver.hpp>

namespace squiz::detail {

template <typename Receiver, typename ResultVariantOfTuples>
void deliver_result(Receiver&& r, ResultVariantOfTuples& result) noexcept {
  std::visit(
      squiz::overload(
          [](std::monostate) noexcept {
            assert(false);
            std::unreachable();
          },
          [&r]<typename Tag, typename... Datums>(
              std::tuple<Tag, Datums...>& result_tuple) noexcept {
            std::apply(
                [&r](Tag, Datums&... datums) noexcept {
                  squiz::set_result(
                      r,
                      squiz::result<Tag, Datums...>,
                      squiz::forward_parameter<Datums>(datums)...);
                },
                result_tuple);
          }),
      result);
}

}  // namespace squiz::detail
