/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <rpc/rpc.h>

TEST(zone_address_test, zone_only_clears_object_id)
{
    rpc::zone_address addr(rpc::zone_address::construction_args(rpc::zone_address::version_3, rpc::zone_address::address_type::ipv6, 0,
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0, 0, 0, 0, 0, 0, 0, 0}, 64, 42, 56, 99, {}));

    auto zone_only = addr.zone_only();

    EXPECT_EQ(zone_only.get_subnet(), 42u);
    EXPECT_EQ(zone_only.get_object_id(), 0u);
}
