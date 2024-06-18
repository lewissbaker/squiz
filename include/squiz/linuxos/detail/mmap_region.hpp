///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <utility>

namespace squiz::linuxos::detail {
class mmap_region {
public:
  mmap_region() noexcept : ptr_(nullptr), size_(0) {}

  static mmap_region adopt(void* ptr, std::size_t size) noexcept {
    mmap_region region;
    region.ptr_ = ptr;
    region.size_ = size;
    return region;
  }

  mmap_region(mmap_region&& other) noexcept
    : ptr_(std::exchange(other.ptr_, nullptr))
    , size_(std::exchange(other.size_, 0)) {}

  // Calls munmap() on the region
  ~mmap_region();

  mmap_region& operator=(mmap_region&& other) noexcept {
    mmap_region tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  void swap(mmap_region& other) noexcept {
    using std::swap;
    swap(ptr_, other.ptr_);
    swap(size_, other.size_);
  }

  void* data() const noexcept { return ptr_; }
  std::size_t size() const noexcept { return size_; }

private:
  void* ptr_;
  std::size_t size_;
};

}  // namespace squiz::linuxos::detail
