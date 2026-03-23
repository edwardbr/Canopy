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
        [[nodiscard]] virtual bool validate(const zone_address& address) const = 0;
    };

    inline bool update_zone_address(zone_address& address, const zone_authenticator& authenticator)
    {
        return authenticator.update(address);
    }

    inline bool validate_zone_address(const zone_address& address, const zone_authenticator& authenticator)
    {
        return authenticator.validate(address);
    }

} // namespace rpc
