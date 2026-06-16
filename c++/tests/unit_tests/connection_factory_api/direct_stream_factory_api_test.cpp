/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <streaming/spsc_queue/factory.h>
#  include <streaming/tcp_coroutine/factory.h>
#else
#  include <streaming/tcp_blocking/factory.h>
#endif

TEST(
    ConnectionFactoryApi,
    DirectStreamFactoriesUseImplementationOwnedTypedSettings)
{
#ifdef CANOPY_BUILD_COROUTINE
    rpc::tcp_coroutine_stream::endpoint endpoint;
    endpoint.port = uint16_t{443};
    EXPECT_EQ(endpoint.host, "127.0.0.1");
    EXPECT_EQ(endpoint.port, uint16_t{443});

    auto queues = rpc::spsc_queue::queue_pair::create();
    EXPECT_NE(queues.connect_to_accept, nullptr);
    EXPECT_NE(queues.accept_to_connect, nullptr);
#else
    rpc::tcp_blocking_stream::endpoint endpoint;
    endpoint.port = uint16_t{443};
    EXPECT_EQ(endpoint.host, "127.0.0.1");
    EXPECT_EQ(endpoint.port, uint16_t{443});
#endif
}
