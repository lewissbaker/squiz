///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace squiz::detail {

template <std::uint64_t Max>
using smallest_unsigned_integer_for = std::conditional_t<
    (Max <= std::numeric_limits<std::uint8_t>::max()),
    std::uint8_t,
    std::conditional_t<
        (Max <= std::numeric_limits<std::uint16_t>::max()),
        std::uint16_t,
        std::conditional_t<
            (Max <= std::numeric_limits<std::uint32_t>::max()),
            std::uint32_t,
            std::uint64_t>>>;
}
