///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <squiz/queryable.hpp>

namespace squiz {

struct empty_env {
  using property_keys = key_list<>;
};

}  // namespace squiz
