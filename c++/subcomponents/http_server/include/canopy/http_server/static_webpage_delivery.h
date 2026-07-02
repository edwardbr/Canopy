// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <canopy/http_server/http_client_connection.h>
#include <file_system/file_system.h>

namespace canopy::http_server
{
    class static_webpage_delivery
    {
    public:
        struct file_read_result
        {
            int error_code{rpc::error::OK()};
            std::vector<uint8_t> data;
        };

        using async_file_reader = std::function<CORO_TASK(file_read_result)(std::string)>;

        static_webpage_delivery(
            std::string root_path,
            async_file_reader file_reader);

        [[nodiscard]] auto handle(request request) const -> CORO_TASK(std::optional<response>);
        static auto content_type_for_path(std::string_view path) -> std::string;

    private:
        std::string root_path_;
        async_file_reader file_reader_;
    };

    struct static_webpage_handler_config
    {
        using cache_predicate = std::function<bool(const request&)>;
        using file_system_manager = rpc::shared_ptr<rpc::file_system::i_manager>;

        std::string root_path;
        file_system_manager file_system;
        cache_predicate disable_cache_for_request;
        std::map<std::string, std::string> disabled_cache_headers{
            {"Cache-Control", "no-store, no-cache, must-revalidate, max-age=0"},
            {"Pragma", "no-cache"},
            {"Expires", "0"},
        };
    };

    [[nodiscard]] auto make_static_webpage_handler(static_webpage_handler_config config) -> coroutine_request_handler;
} // namespace canopy::http_server
