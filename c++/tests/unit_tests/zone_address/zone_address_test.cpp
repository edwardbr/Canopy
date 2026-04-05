/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <rpc/rpc.h>

TEST(
    zone_address_test,
    zone_only_clears_object_id)
{
    auto result = rpc::zone_address::create(
        rpc::zone_address_args(
            rpc::default_values::version_3,
            rpc::address_type::ipv6,
            0,
            {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0, 0, 0, 0, 0, 0, 0, 0},
            64,
            42,
            56,
            99,
            {}));
    ASSERT_TRUE(result.has_value());
    rpc::zone_address addr = std::move(*result);

    auto zone_only = addr.zone_only();

    EXPECT_EQ(zone_only.get_subnet(), 42u);
    EXPECT_EQ(zone_only.get_object_id(), 0u);
}
