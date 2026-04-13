/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once
#include <string>
#include <stdint.h>
#include <functional>
#include <vector>
#include <rpc/internal/polyfill/format.h>

#include "rpc/internal/coroutine_support.h"
#include <rpc/internal/serialiser.h>

namespace std
{
    inline std::string to_string(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty())
            return {};

        std::string result;
        result.reserve(bytes.size() * 3 - 1);
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            if (i != 0)
                result += '.';
            rpc::format_to(std::back_inserter(result), "{:02x}", bytes[i]);
        }
        return result;
    }

    inline std::string to_string(const rpc::zone_address& val)
    {
        auto routing_prefix = val.get_routing_prefix();
        if (routing_prefix.empty() && val.get_object_id() == 0)
            return std::to_string(val.get_subnet());
        return std::to_string(routing_prefix) + ":" + std::to_string(val.get_subnet()) + "/"
               + std::to_string(val.get_object_id());
    }
    inline std::string to_string(const rpc::zone& val)
    {
        return std::to_string(val.get_address());
    }
    inline std::string to_string(const rpc::remote_object& val)
    {
        return std::to_string(val.get_address());
    }
    inline std::string to_string(const rpc::object& val)
    {
        return std::to_string(val.get_val());
    }
    inline std::string to_string(const rpc::interface_ordinal& val)
    {
        return std::to_string(val.get_val());
    }
    inline std::string to_string(const rpc::method& val)
    {
        return std::to_string(val.get_val());
    }

    template<> struct hash<rpc::zone_address>
    {
        auto operator()(const rpc::zone_address& item) const noexcept
        {
            std::size_t h = 0;
            for (auto byte : item.get_blob())
            {
                h ^= std::hash<uint8_t>{}(byte) + 0x9e3779b9 + (h << 6) + (h >> 2);
            }
            return h;
        }
    };

    template<> struct hash<rpc::zone>
    {
        auto operator()(const rpc::zone& item) const noexcept
        {
            return std::hash<rpc::zone_address>{}(item.get_address());
        }
    };

    template<> struct hash<rpc::remote_object>
    {
        auto operator()(const rpc::remote_object& item) const noexcept
        {
            return std::hash<rpc::zone_address>{}(item.get_address());
        }
    };

    template<> struct hash<rpc::interface_ordinal>
    {
        auto operator()(const rpc::interface_ordinal& item) const noexcept { return (std::size_t)item.get_val(); }
    };

    template<> struct hash<rpc::object>
    {
        auto operator()(const rpc::object& item) const noexcept { return (std::size_t)item.get_val(); }
    };

    template<> struct hash<rpc::method>
    {
        auto operator()(const rpc::method& item) const noexcept { return (std::size_t)item.get_val(); }
    };
}
