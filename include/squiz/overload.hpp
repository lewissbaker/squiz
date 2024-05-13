///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

namespace squiz {

  /// Helper for constructing an overload-set from a set of lambdas.
  ///
  /// \example
  /// \begincode
  /// // Calling with select between overloads using normal overload-resolution.
  /// squiz::overload([](int x) {},
  ///                 [](float x) {},
  ///                 [](auto x) {})
  /// \endcode
  template <typename... Fs> struct overload : Fs... { using Fs::operator()...; };
  template <typename... Fs> overload(Fs...) -> overload<Fs...>;
}  // namespace squiz
