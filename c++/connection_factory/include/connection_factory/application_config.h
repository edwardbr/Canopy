/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <connection_factory/context.h>
#include <connection_factory_config/connection_factory_config.h>
#include <rpc/rpc.h>

namespace rpc::connection_factory
{
    class application_runtime;

    struct application_runtime_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<application_runtime> runtime;
        std::string message;
    };

    struct application_context_result
    {
        int error_code{rpc::error::OK()};
        context context;
        std::string message;
    };

    // Owns host-side resources described by configuration and creates
    // connection factory contexts for named connections.
    class application_runtime
    {
    public:
        application_runtime(
            configuration settings,
            std::filesystem::path base_directory);
        ~application_runtime();

        application_runtime(const application_runtime&) = delete;
        auto operator=(const application_runtime&) -> application_runtime& = delete;

        [[nodiscard]] auto settings() const -> const configuration&;
        [[nodiscard]] auto find_connection(std::string_view name) const -> const named_connection_settings*;

        [[nodiscard]] auto context_for(const named_connection_settings& connection) const -> application_context_result;

    private:
        struct impl;
        std::shared_ptr<impl> impl_;
    };

    [[nodiscard]] auto make_application_runtime(
        configuration settings,
        std::filesystem::path base_directory = {}) -> application_runtime_result;

    [[nodiscard]] auto load_application_runtime(const std::filesystem::path& path) -> application_runtime_result;
} // namespace rpc::connection_factory
