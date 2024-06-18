///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <utility>

namespace squiz::linuxos::detail {

class file_handle {
  static constexpr int invalid_file_handle = -1;

public:
  file_handle() noexcept : fd_(invalid_file_handle) {}

  file_handle(file_handle&& other) noexcept
    : fd_(std::exchange(other.fd_, invalid_file_handle)) {}

  ~file_handle() { close(); }

  file_handle& operator=(file_handle&& other) noexcept {
    file_handle tmp{std::move(other)};
    swap(tmp);
    return *this;
  }

  static file_handle adopt(int fd) noexcept {
    file_handle handle;
    handle.fd_ = fd;
    return handle;
  }

  explicit operator bool() const noexcept { return fd_ != invalid_file_handle; }

  void swap(file_handle& other) noexcept { std::swap(fd_, other.fd_); }

  int get_fd() const noexcept { return fd_; }

  void close() noexcept;

  int release() noexcept { return std::exchange(fd_, invalid_file_handle); }

private:
  int fd_;
};

}  // namespace squiz::linuxos::detail
