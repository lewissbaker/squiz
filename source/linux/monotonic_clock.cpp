///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/linuxos/monotonic_clock.hpp>

#include <stdexcept>

#include <sys/types.h>
#include <time.h>

namespace squiz::linuxos {

monotonic_clock::time_point monotonic_clock::now() {
  timespec time{};
  int result = ::clock_gettime(CLOCK_BOOTTIME, &time);
  if (result == -1) {
    int error_code = errno;
    throw std::system_error(std::error_code(error_code, std::system_category()));
  }

  return time_point::from_seconds_and_nanoseconds(time.tv_sec, time.tv_nsec);
}

}  // namespace squiz::linuxos
