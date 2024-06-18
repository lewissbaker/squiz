///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <signal.h>
#include <linux/io_uring.h>

namespace squiz::linuxos {

int io_uring_register(
    int fd, unsigned opcode, const void* arg, unsigned nr_args);
int io_uring_setup(unsigned entries, struct io_uring_params* p);
int io_uring_enter(
    int fd,
    unsigned to_submit,
    unsigned min_complete,
    unsigned flags,
    sigset_t* sig);  

}
