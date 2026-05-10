/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "sgx_coroutine_test_setup.h"

#include <gtest/gtest.h>

#include <iostream>

class sgx_coroutine_test_host_fixture : public ::testing::Test
{
protected:
    void SetUp() override { setup_.set_up(); }
    void TearDown() override { setup_.tear_down(); }

    io_uring_test::iouring_noop_measurement get_noop_measurement()
    {
        io_uring_test::iouring_noop_measurement measurement;
        auto err = SYNC_WAIT(setup_.get_test_uring()->get_noop_measurement(measurement));
        EXPECT_EQ(err, rpc::error::OK());
        return measurement;
    }

    void expect_successful_noop_measurement(
        const io_uring_test::iouring_noop_measurement& measurement,
        uint64_t iterations,
        bool use_proactor)
    {
        std::cout << "io_uring no_op measurement"
                  << " strategy=" << (use_proactor ? "proactor" : "cooperative") << " iterations=" << iterations
                  << " calls=" << measurement.no_op_calls << " successes=" << measurement.no_op_successes
                  << " failures=" << measurement.no_op_failures << " submit_attempts=" << measurement.submit_attempts
                  << " backpressure=" << measurement.submit_backpressure
                  << " pump_calls=" << measurement.completion_pump_calls
                  << " completions=" << measurement.completion_entries
                  << " scheduler_yields=" << measurement.scheduler_yields
                  << " local_spins=" << measurement.local_relax_spins << " host_wakes=" << measurement.host_wake_calls
                  << " proactor_starts=" << measurement.proactor_pump_starts
                  << " proactor_iterations=" << measurement.proactor_pump_iterations
                  << " proactor_suspends=" << measurement.proactor_waiter_suspends
                  << " proactor_resumes=" << measurement.proactor_resumes
                  << " proactor_start_failures=" << measurement.proactor_start_failures
                  << " total_ticks=" << measurement.total_no_op_ticks << " max_ticks=" << measurement.max_no_op_ticks
                  << '\n';

        EXPECT_EQ(measurement.no_op_calls, iterations);
        EXPECT_EQ(measurement.no_op_successes, iterations);
        EXPECT_EQ(measurement.no_op_failures, 0);
        EXPECT_GE(measurement.submit_attempts, iterations);
        EXPECT_GE(measurement.completion_pump_calls, 1);
        EXPECT_EQ(measurement.completion_entries, iterations);
        EXPECT_EQ(measurement.local_relax_spins, 0);
        EXPECT_EQ(measurement.proactor_start_failures, 0);
        if (use_proactor)
        {
            EXPECT_GE(measurement.proactor_pump_starts, 1);
            EXPECT_GE(measurement.proactor_pump_iterations, 1);
        }
        else
        {
            EXPECT_EQ(measurement.proactor_pump_starts, 0);
            EXPECT_EQ(measurement.proactor_pump_iterations, 0);
            EXPECT_EQ(measurement.proactor_waiter_suspends, 0);
            EXPECT_EQ(measurement.proactor_resumes, 0);
        }
    }

    sgx_coroutine_test_setup setup_;
};

class sgx_coroutine_small_buffer_test_host_fixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        rpc::io_uring::host_controller::options options;
        // Timed receive needs two host buffers: one for data and one for the
        // linked timeout. This pool is intentionally smaller than two buffers
        // per stream so the test exercises paired reservation under pressure.
        options.buffer_count = 15;
        options.buffer_size = 4096;
        setup_.set_up(options);
    }

    void TearDown() override { setup_.tear_down(); }

    sgx_coroutine_test_setup setup_;
};

class sgx_coroutine_small_ring_test_host_fixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        rpc::io_uring::host_controller::options options;
        // Keep the ring deliberately small so concurrent stream operations
        // exercise FIFO SQ/CQ admission rather than relying on spare depth.
        options.queue_depth = 8;
        options.buffer_count = 64;
        options.buffer_size = 4096;
        setup_.set_up(options);
    }

    void TearDown() override { setup_.tear_down(); }

    sgx_coroutine_test_setup setup_;
};

class sgx_coroutine_small_fixed_file_test_host_fixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        rpc::io_uring::host_controller::options options;
        // A single self-ping needs one listener, one client socket, and one
        // accepted socket. Four slots leaves one spare slot while still proving
        // close returns direct descriptors to the fixed-file table.
        options.fixed_file_count = 4;
        options.queue_depth = 16;
        options.buffer_count = 32;
        options.buffer_size = 4096;
        setup_.set_up(options);
    }

    void TearDown() override { setup_.tear_down(); }

    sgx_coroutine_test_setup setup_;
};

TEST_F(
    sgx_coroutine_test_host_fixture,
    connects_to_dedicated_enclave)
{
    ASSERT_TRUE(setup_.get_host());
    ASSERT_TRUE(setup_.get_test_uring());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    test_noop_submits_iouring_nop)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->test_noop(false, 1, true));
    ASSERT_EQ(err, rpc::error::OK());
    expect_successful_noop_measurement(get_noop_measurement(), 1, true);
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    self_ping_test)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    self_ping_roundtrip_stress_test)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_roundtrip_test(32, 8192));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    self_ping_receive_timeout_test)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_receive_timeout_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    self_ping_multi_stream_stress_test)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_multi_stream_test(8, 16, 4096));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    peer_to_peer_rpc_roundtrip_test)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->peer_to_peer_rpc_test(8));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    stream_benchmark_smoke_test)
{
    io_uring_test::stream_benchmark_stats stats;
    auto err = SYNC_WAIT(setup_.get_test_uring()->stream_benchmark(false, 8, 2, 1024, stats));
    ASSERT_EQ(err, rpc::error::OK());
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.blob_size, 1024);
    ASSERT_GT(stats.avg, 0.0);

    err = SYNC_WAIT(setup_.get_test_uring()->stream_benchmark(true, 8, 2, 1024, stats));
    ASSERT_EQ(err, rpc::error::OK());
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.blob_size, 1024);
    ASSERT_GT(stats.avg, 0.0);
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    close_during_accept_completes)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->close_during_accept_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    close_during_receive_completes)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->close_during_receive_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    close_during_send_completes)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->close_during_send_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    controller_shutdown_rejects_new_work)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->controller_shutdown_rejects_new_work_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    controller_shutdown_completes_scheduled_noops)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->controller_shutdown_scheduled_noop_test(1000));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    controller_shutdown_completes_pending_accept)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->controller_shutdown_pending_accept_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    controller_shutdown_completes_pending_receive)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->controller_shutdown_pending_receive_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    controller_shutdown_completes_pending_send)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->controller_shutdown_pending_send_test());
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_small_buffer_test_host_fixture,
    self_ping_multi_stream_small_host_buffer_pool)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_multi_stream_test(10, 2, 4096));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_small_ring_test_host_fixture,
    self_ping_multi_stream_small_submission_ring)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_multi_stream_test(8, 4, 1024));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_small_fixed_file_test_host_fixture,
    direct_descriptor_slots_exhaust_and_reuse)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->direct_descriptor_reuse_test(4, 8));
    ASSERT_EQ(err, rpc::error::OK());
}

TEST_F(
    sgx_coroutine_small_fixed_file_test_host_fixture,
    self_ping_reuses_direct_descriptor_slots)
{
    for (uint32_t iteration = 0; iteration < 8; ++iteration)
    {
        auto err = SYNC_WAIT(setup_.get_test_uring()->self_ping_test());
        ASSERT_EQ(err, rpc::error::OK());
    }
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    test_noop_submits_1000_iouring_nops_cooperative)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->test_noop(false, 1000, false));
    ASSERT_EQ(err, rpc::error::OK());
    expect_successful_noop_measurement(get_noop_measurement(), 1000, false);
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    test_noop_submits_1000_iouring_nops_proactor)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->test_noop(false, 1000, true));
    ASSERT_EQ(err, rpc::error::OK());
    expect_successful_noop_measurement(get_noop_measurement(), 1000, true);
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    test_noop_submits_1000_scheduled_iouring_nops_cooperative)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->test_noop(true, 1000, false));
    ASSERT_EQ(err, rpc::error::OK());
    expect_successful_noop_measurement(get_noop_measurement(), 1000, false);
}

TEST_F(
    sgx_coroutine_test_host_fixture,
    test_noop_submits_1000_scheduled_iouring_nops_proactor)
{
    auto err = SYNC_WAIT(setup_.get_test_uring()->test_noop(true, 1000, true));
    ASSERT_EQ(err, rpc::error::OK());
    expect_successful_noop_measurement(get_noop_measurement(), 1000, true);
}
