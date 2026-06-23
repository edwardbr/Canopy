/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/dns_resolver/resolver.h>

#include <gtest/gtest.h>

namespace
{
    bool has_loopback(const canopy::dns_resolver::resolve_result& result)
    {
        for (const auto& endpoint : result.endpoints)
        {
            if (endpoint.family == canopy::dns_resolver::address_family::ipv4 && endpoint.ipv4[0] == 127)
                return true;
            if (endpoint.family == canopy::dns_resolver::address_family::ipv6 && endpoint.ipv6[15] == 1)
                return true;
        }
        return false;
    }
}

TEST(
    DnsResolver,
    ResolvesLocalhostBlocking)
{
    canopy::dns_resolver::resolve_options options;
    options.timeout = std::chrono::milliseconds{1000};

    const auto result = canopy::dns_resolver::resolve_host_blocking("localhost", 80, options);
    ASSERT_EQ(result.error_code, rpc::error::OK()) << result.error_message;
    ASSERT_FALSE(result.endpoints.empty());
    EXPECT_TRUE(has_loopback(result));
}

TEST(
    DnsResolver,
    ResolvesLocalhostViaDualModeApi)
{
    canopy::dns_resolver::resolve_options options;
    options.timeout = std::chrono::milliseconds{1000};

    const auto result = SYNC_WAIT(canopy::dns_resolver::resolve_host("localhost", 80, options));
    ASSERT_EQ(result.error_code, rpc::error::OK()) << result.error_message;
    ASSERT_FALSE(result.endpoints.empty());
    EXPECT_TRUE(has_loopback(result));
}
