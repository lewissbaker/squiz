///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <utility>

namespace squiz::detail {

/// Compute the type and value-category of accessing a data member of type
/// 'Member' on an object of type 'Class'.
template <typename Class, typename Member>
using member_type_t =
    decltype(std::declval<Class>().*static_cast<Member std::remove_reference_t<Class>::*>(nullptr));

}  // namespace squiz::detail
