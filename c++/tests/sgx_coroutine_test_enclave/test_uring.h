/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <rpc/rpc.h>
#include <io_uring/controller.h>
#include <io_uring/tcp.h>
#include <io_uring_test/test.h>
#include <streaming/io_uring_new/stream.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace io_uring_test_enclave
{
    class test_uring : public rpc::base<test_uring, io_uring_test::i_test_uring>,
                       public rpc::enable_shared_from_this<test_uring>
    {
    public:
        explicit test_uring(
            std::shared_ptr<rpc::io_uring::controller> controller,
            std::shared_ptr<rpc::service> child_service);

        CORO_TASK(int)
        test_noop(
            bool schedule,
            uint32_t iterations,
            bool use_proactor) override;
        CORO_TASK(int) self_ping_test() override;
        CORO_TASK(int)
        self_ping_roundtrip_test(
            uint32_t iterations,
            uint32_t payload_size) override;
        CORO_TASK(int) self_ping_receive_timeout_test() override;
        CORO_TASK(int)
        self_ping_multi_stream_test(
            uint32_t connection_count,
            uint32_t iterations,
            uint32_t payload_size) override;
        CORO_TASK(int)
        direct_descriptor_reuse_test(
            uint32_t descriptor_count,
            uint32_t cycles) override;
        CORO_TASK(int) close_during_accept_test() override;
        CORO_TASK(int) close_during_receive_test() override;
        CORO_TASK(int) close_during_send_test() override;
        CORO_TASK(int) controller_shutdown_rejects_new_work_test() override;
        CORO_TASK(int) controller_shutdown_scheduled_noop_test(uint32_t iterations) override;
        CORO_TASK(int) controller_shutdown_pending_accept_test() override;
        CORO_TASK(int) controller_shutdown_pending_receive_test() override;
        CORO_TASK(int) controller_shutdown_pending_send_test() override;
        CORO_TASK(int) get_noop_measurement(io_uring_test::iouring_noop_measurement& measurement) override;
        CORO_TASK(int) peer_to_peer_rpc_test(uint32_t iterations) override;
        CORO_TASK(int)
        stream_benchmark(
            bool send_reply,
            uint32_t iterations,
            uint32_t warmup,
            uint32_t payload_size,
            io_uring_test::stream_benchmark_stats& stats) override;

    private:
        static io_uring_test::iouring_noop_measurement to_test_measurement(
            const rpc::io_uring::controller_measurements& measurement);

        void store_noop_measurement();

        static int make_empty_payload_buffer(
            std::vector<uint8_t>& payload,
            uint32_t payload_size);
        static int make_self_ping_payload(
            std::vector<uint8_t>& payload,
            uint32_t iteration,
            uint32_t payload_size,
            bool response,
            uint32_t stream_id = 0);
        static int match_self_ping_request(
            const std::vector<uint8_t>& payload,
            uint32_t iteration,
            uint32_t payload_size,
            uint32_t max_stream_id,
            uint32_t& stream_id);

        static CORO_TASK(int) receive_exact(
            const std::shared_ptr<streaming::stream>& stream,
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

        std::shared_ptr<rpc::io_uring::controller> controller_;
        std::shared_ptr<rpc::service> child_service_;
        io_uring_test::iouring_noop_measurement last_noop_measurement_{};
    };
} // namespace io_uring_test_enclave
