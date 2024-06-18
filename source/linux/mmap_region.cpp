///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#include <squiz/linuxos/detail/mmap_region.hpp>

#include <sys/mman.h>

namespace squiz::linuxos::detail {

mmap_region::~mmap_region() {
  if (ptr_ != nullptr) {
    ::munmap(ptr_, size_);
  }
}

}  // namespace squiz::linuxos::detail
