/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/connection_factory.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

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

TEST(
    ConnectionFactoryApi,
    PublicConfigHeaderExposesContextAsDependencyBoundary)
{
    rpc::connection_factory::context context;
    auto dependency = std::make_shared<std::string>("runtime-dependency");

    context.set_dependency(dependency, "named");

    EXPECT_EQ(context.get_dependency<std::string>("named"), dependency);
    EXPECT_EQ(context.get_dependency<std::string>(), nullptr);
    EXPECT_NE(&rpc::connection_factory::default_context(), &context);
}

TEST(
    ConnectionFactoryApi,
    PublicConfigHeaderSupportsTypedContextRegistrationWithoutDetailHeaders)
{
    rpc::connection_factory::context context;
    bool builder_called = false;

    context.register_connect_base_stream<rpc::connection_factory::service_settings>(
        "public_api_test_base",
        [&](rpc::connection_factory::service_settings,
            std::shared_ptr<rpc::service>,
            const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
        {
            builder_called = true;
            CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}};
        });

    EXPECT_FALSE(builder_called);
}
