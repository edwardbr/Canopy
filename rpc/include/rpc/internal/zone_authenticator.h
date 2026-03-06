/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

namespace rpc
{
    struct zone_address;

    struct zone_authenticator
    {
        virtual ~zone_authenticator() = default;
        virtual bool update(zone_address& address) const = 0;
        virtual bool validate(const zone_address& address) const = 0;
    };

    inline bool update_zone_address(zone_address& address, const zone_authenticator& authenticator)
    {
        return authenticator.update(address);
    }

    inline bool validate_zone_address(const zone_address& address, const zone_authenticator& authenticator)
    {
        return authenticator.validate(address);
    }

#if defined(CANOPY_HASH_ADDRESS_SIZE) && !defined(CANOPY_FIXED_ADDRESS_SIZE)
    namespace detail
    {
        inline uint64_t mix_zone_address_hash(uint64_t hash, uint64_t value)
        {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6u) + (hash >> 2u);
            return hash;
        }
    } // namespace detail

    inline uint64_t calculate_zone_address_hash(const zone_address& address, uint64_t nonce)
    {
        uint64_t hash = detail::mix_zone_address_hash(0xcbf29ce484222325ULL, nonce);
        hash = detail::mix_zone_address_hash(hash, address.get_object_offset());
        for (auto byte : address.get_host_address())
        {
            hash = detail::mix_zone_address_hash(hash, byte);
        }
        hash = detail::mix_zone_address_hash(hash, address.get_subnet());
        hash = detail::mix_zone_address_hash(hash, address.get_object_id());

        if constexpr (zone_address::get_hash_bits() >= 64u)
            return hash;
        return hash & ((uint64_t(1) << zone_address::get_hash_bits()) - 1u);
    }

    inline bool update_zone_address_hash(zone_address& address, uint64_t nonce)
    {
        return address.set_hash(calculate_zone_address_hash(address, nonce));
    }

    inline bool validate_zone_address_hash(const zone_address& address, uint64_t nonce)
    {
        return address.get_hash() == calculate_zone_address_hash(address, nonce);
    }
#endif
} // namespace rpc
