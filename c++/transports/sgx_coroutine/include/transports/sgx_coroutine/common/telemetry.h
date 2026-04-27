/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rpc/rpc.h>

namespace rpc::sgx::coro::common
{
    inline auto encode_log_payload(
        int level,
        std::vector<char> message) -> std::vector<char>
    {
        std::vector<char> payload;
        payload.reserve(message.size() + 1);
        payload.push_back(static_cast<char>(static_cast<std::uint8_t>(level)));
        payload.insert(payload.end(), message.begin(), message.end());
        return payload;
    }

    inline auto decode_log_payload(const std::vector<char>& payload) -> std::pair<
        int,
        std::string>
    {
        if (payload.empty())
            return {0, {}};

        int level = static_cast<unsigned char>(payload.front());
        return {level, std::string(payload.begin() + 1, payload.end())};
    }

}
