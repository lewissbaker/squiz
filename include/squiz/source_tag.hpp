///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstddef>

namespace squiz {
  struct source_tag {};
  
  /// A simple tag type that can be used as the \c Tag template parameter of
  /// the \c variant_child_operation template in cases where there is only one
  /// such child.
  template<std::size_t Idx>
  struct indexed_source_tag {};

  
}
