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

TEST(
    zone_address_test,
    from_blob_round_trips_packed_bytes)
{
    auto created = rpc::zone_address::create(
        rpc::zone_address_args(
            rpc::default_values::version_3, rpc::address_type::ipv4, 9000, {127, 0, 0, 1}, 24, 0x1234, 20, 0x345, {0xAA, 0x55}));
    ASSERT_TRUE(created.has_value());

    auto raw_blob = created->get_blob();
    auto rebuilt = rpc::zone_address::from_blob(raw_blob);
    ASSERT_TRUE(rebuilt.has_value());

    EXPECT_EQ(rebuilt->get_blob(), raw_blob);
    EXPECT_EQ(rebuilt->get_version(), created->get_version());
    EXPECT_EQ(rebuilt->get_address_type(), created->get_address_type());
    EXPECT_EQ(rebuilt->get_port(), created->get_port());
    EXPECT_EQ(rebuilt->get_routing_prefix(), created->get_routing_prefix());
    EXPECT_EQ(rebuilt->get_subnet(), created->get_subnet());
    EXPECT_EQ(rebuilt->get_object_id(), created->get_object_id());
}
