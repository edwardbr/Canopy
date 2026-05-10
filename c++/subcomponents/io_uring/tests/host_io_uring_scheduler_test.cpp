/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <coro/coro.hpp>
#include <io_uring/host_io_uring.h>
#include <rpc/rpc.h>

TEST(
    HostIoUringScheduler,
    CreatesControllerAndSubmitsNoOp)
{
    std::shared_ptr<rpc::io_uring::io_uring_scheduler> scheduler_owner;
    rpc::io_uring::linux_io_uring_handle::options options;
    options.queue_depth = 64;
    options.buffer_count = 8;
    options.buffer_size = 4096;
    options.register_buffers = false;
    options.fixed_file_count = 0;
    options.register_fixed_files = false;

    const auto err = rpc::io_uring::create_host_io_uring_scheduler(scheduler_owner, options);
    if (err == rpc::error::NATIVE_IO_ERROR())
    {
        GTEST_SKIP() << "host io_uring setup is not available on this kernel/runtime";
    }

    ASSERT_EQ(err, rpc::error::OK());
    ASSERT_TRUE(scheduler_owner);

    auto controller = scheduler_owner->get_controller();
    ASSERT_TRUE(controller);
    EXPECT_EQ(coro::sync_wait(controller->no_op()), rpc::error::OK());

    scheduler_owner->shutdown();
}
