/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "config_demo_app.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <config_demo_config/config_demo_config.h>
#include <config_demo_config/config_demo_config_schema.h>
#include <json/config.h>
#include <json/convert.h>

namespace config_demo::v1
{
    auto load_demo_settings(
        const std::filesystem::path& config_path,
        demo_settings& settings,
        std::string& error_message) -> demo_error
    {
        try
        {
            settings = json::v1::convert::from_json_object<demo_settings>(json::v1::parse_file(config_path));
            settings.execution.iterations = std::max<uint64_t>(settings.execution.iterations, 1);
            error_message.clear();
            return rpc::error::OK();
        }
        catch (const std::exception& error)
        {
            error_message = error.what();
            return demo_error::INVALID_CONFIGURATION;
        }
    }

    auto make_connection_factory_runtime(
        const demo_settings& settings,
        std::filesystem::path base_directory,
        std::shared_ptr<rpc::connection_factory::application_runtime>& runtime,
        std::string& error_message) -> demo_error
    {
        rpc::connection_factory::configuration factory_config;
        factory_config.spsc_queues = settings.spsc_queues;
        factory_config.attestation_services = settings.attestation_services;
        factory_config.connections = settings.connections;

        auto loaded_runtime
            = rpc::connection_factory::make_application_runtime(std::move(factory_config), std::move(base_directory));
        if (loaded_runtime.error_code != rpc::error::OK() || !loaded_runtime.runtime)
        {
            error_message = loaded_runtime.message;
            if (loaded_runtime.error_code != rpc::error::OK())
                return loaded_runtime.error_code;
            return demo_error::INVALID_CONFIGURATION;
        }

        runtime = std::move(loaded_runtime.runtime);
        error_message.clear();
        return rpc::error::OK();
    }

    auto make_scheduler(uint32_t thread_count) -> std::shared_ptr<coro::scheduler>
    {
        thread_count = std::max<uint32_t>(thread_count, 1);
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = thread_count},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }
} // namespace config_demo::v1
