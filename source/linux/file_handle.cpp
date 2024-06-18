///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/linuxos/detail/file_handle.hpp>

#include <cassert>

#include <unistd.h>

namespace squiz::linuxos::detail {

void file_handle::close() noexcept {
  if (fd_ != invalid_file_handle) {
    if (::close(fd_) == -1) {
      assert(errno != EBADF);
    }
    fd_ = invalid_file_handle;
  }
}

}  // namespace squiz::linuxos::detail
