// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <canopy/http_server/http_client_connection.h>

namespace canopy::http_server
{
    class static_webpage_delivery
    {
    public:
        using async_file_reader = std::function<CORO_TASK(int)(std::string, std::vector<uint8_t>&)>;

        static_webpage_delivery(
            std::string root_path,
            async_file_reader file_reader);

        [[nodiscard]] auto handle(const request& request) const -> CORO_TASK(std::optional<response>);
        static auto content_type_for_path(std::string_view path) -> std::string;

    private:
        std::string root_path_;
        async_file_reader file_reader_;
    };
} // namespace canopy::http_server
