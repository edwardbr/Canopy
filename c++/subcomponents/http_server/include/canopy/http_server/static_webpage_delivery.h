// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <canopy/http_server/http_client_connection.h>

namespace canopy::http_server
{
    class static_webpage_delivery
    {
    public:
        explicit static_webpage_delivery(std::filesystem::path root_path);

        [[nodiscard]] auto handle(const request& request) const -> std::optional<response>;
        static auto get_content_type(const std::filesystem::path& path) -> std::string;

    private:
        std::filesystem::path root_path_;
    };
} // namespace canopy::http_server
