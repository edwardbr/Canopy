/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <rpc/rpc.h>

#if defined(CANOPY_HASH_ADDRESS_SIZE)
namespace
{
    class nonce_zone_authenticator : public rpc::zone_authenticator
    {
        uint64_t nonce_;

    public:
        explicit nonce_zone_authenticator(uint64_t nonce)
            : nonce_(nonce)
        {
        }

        bool update(rpc::zone_address& address) const override
        {
            return rpc::update_zone_address_hash(address, nonce_);
        }

        bool validate(const rpc::zone_address& address) const override
        {
            return rpc::validate_zone_address_hash(address, nonce_);
        }
    };
} // namespace
#endif

TEST(zone_address_test, zone_only_clears_object_id)
{
    rpc::zone_address addr(0x1122334455667788ULL, 42, 99);

    auto zone_only = addr.zone_only();

    EXPECT_EQ(zone_only.get_subnet(), 42u);
    EXPECT_EQ(zone_only.get_object_id(), 0u);
}

#if defined(CANOPY_HASH_ADDRESS_SIZE)
TEST(zone_address_test, hash_uses_reserved_tail_bits)
{
    std::array<uint8_t, rpc::zone_address::host_address_size> host = {};
    host[0] = 0x20;
    host[1] = 0x01;
    host[2] = 0x0d;
    host[3] = 0xb8;

    rpc::zone_address addr(host, 64, 0x1234);
    nonce_zone_authenticator auth(0x123456789abcdef0ULL);

    ASSERT_TRUE(addr.set_subnet(0x55AA));
    auto expected_hash = rpc::calculate_zone_address_hash(addr, 0x123456789abcdef0ULL);
    ASSERT_TRUE(rpc::update_zone_address(addr, auth));

    EXPECT_EQ(addr.get_hash(), expected_hash);
    EXPECT_TRUE(rpc::validate_zone_address(addr, auth));
    EXPECT_EQ(addr.get_subnet(), 0x55AAu);
    EXPECT_EQ(addr.get_object_id(), 0x1234u);

    auto tampered = addr.with_object(0x4321);
    EXPECT_FALSE(rpc::validate_zone_address(tampered, auth));
}
#endif
