/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "config_demo_app.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <config_demo_config/config_demo_config_schema.h>
#include <json/config.h>
#include <json/convert.h>

namespace config_demo::v1
{
    auto load_demo_configuration(const std::filesystem::path& config_path) -> loaded_demo_configuration
    {
        auto settings = json::v1::convert::from_json_object<demo_settings>(json::v1::parse_file(config_path));
        settings.iterations = std::max<uint64_t>(settings.iterations, 1);

        rpc::connection_factory::topology_settings factory_settings;
        factory_settings.rpc_runtime = std::move(settings.rpc_runtime);
        factory_settings.connections = std::move(settings.connections);

        auto loaded_runtime
            = rpc::connection_factory::make_application_runtime(std::move(factory_settings), config_path.parent_path());
        if (loaded_runtime.error_code != rpc::error::OK() || !loaded_runtime.runtime)
            throw std::runtime_error(loaded_runtime.message);

        loaded_demo_configuration result;
        result.settings = std::move(settings);
        result.runtime = std::move(loaded_runtime.runtime);
        return result;
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
