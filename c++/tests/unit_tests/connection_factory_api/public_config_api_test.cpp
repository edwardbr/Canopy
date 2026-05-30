/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/connection_factory.h>

#include <gtest/gtest.h>

#if __has_include(<connection_factory/tcp.h>)
#  error "TCP direct factory API must live with the TCP implementation, not connection_factory"
#endif

#if __has_include(<connection_factory/tcp_blocking.h>)
#  error "Blocking TCP direct factory API must live with the blocking TCP implementation"
#endif

#if __has_include(<connection_factory/tcp_coroutine.h>)
#  error "Coroutine TCP direct factory API must live with the coroutine TCP implementation"
#endif

#if __has_include(<connection_factory/spsc_queue.h>)
#  error "SPSC direct factory API must live with the SPSC implementation"
#endif

TEST(
    ConnectionFactoryApi,
    PublicConfigHeaderMaterialisesSparseConnectionSettings)
{
    const auto materialised
        = rpc::connection_factory::materialise_connection_settings(rpc::connection_factory::empty_options());

    ASSERT_EQ(materialised.error_code, rpc::error::OK());
    EXPECT_FALSE(materialised.settings.service.has_value());
    EXPECT_FALSE(materialised.settings.listener.has_value());
    EXPECT_FALSE(materialised.settings.transport.has_value());
    EXPECT_TRUE(materialised.settings.stream_layers.empty());
}
