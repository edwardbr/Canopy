/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <connection_factory/application_config.h>
#include <config_demo_config/config_demo_config.h>
#include <rpc/rpc.h>

namespace config_demo::v1
{
    struct loaded_demo_configuration
    {
        demo_settings settings;
        std::shared_ptr<rpc::connection_factory::application_runtime> runtime;
    };

    [[nodiscard]] auto load_demo_configuration(const std::filesystem::path& config_path) -> loaded_demo_configuration;
    [[nodiscard]] auto make_scheduler(uint32_t thread_count) -> std::shared_ptr<coro::scheduler>;

    [[nodiscard]] auto run_configured_demo(
        const rpc::connection_factory::application_runtime& runtime,
        const demo_settings& settings,
        const std::shared_ptr<coro::scheduler>& scheduler_1,
        const std::shared_ptr<coro::scheduler>& scheduler_2) -> bool;
} // namespace config_demo::v1
