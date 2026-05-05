/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <rpc/rpc.h>
#include <io_uring/controller.h>
#include <io_uring_test/test.h>

#include <cstdint>
#include <memory>

namespace io_uring_test_enclave
{
    using peer2peer_endpoint_factory = rpc::shared_ptr<io_uring_test::i_peer2peer> (*)(
        std::shared_ptr<rpc::service>,
        rpc::shared_ptr<io_uring_test::i_peer2peer>);

    CORO_TASK(int)
    end_2_end_setup(
        const std::shared_ptr<rpc::io_uring::controller>& controller,
        const std::shared_ptr<rpc::service>& child_service,
        uint32_t iterations,
        peer2peer_endpoint_factory client_factory,
        peer2peer_endpoint_factory server_factory);
} // namespace io_uring_test_enclave
