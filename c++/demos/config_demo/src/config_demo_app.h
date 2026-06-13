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
    [[nodiscard]] auto load_demo_settings(const std::filesystem::path& config_path) -> demo_settings;
    [[nodiscard]] auto make_connection_factory_runtime(
        const demo_settings& settings,
        std::filesystem::path base_directory) -> std::shared_ptr<rpc::connection_factory::application_runtime>;
    [[nodiscard]] auto make_scheduler(uint32_t thread_count) -> std::shared_ptr<coro::scheduler>;

    [[nodiscard]] auto run_configured_demo(
        const rpc::connection_factory::application_runtime& runtime,
        const execution_settings& execution,
        const std::shared_ptr<coro::scheduler>& scheduler_1,
        const std::shared_ptr<coro::scheduler>& scheduler_2) -> int;
} // namespace config_demo::v1
