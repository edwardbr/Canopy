/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "config_demo_app.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

#include <rpc/rpc.h>

namespace config_demo::v1
{
    namespace
    {
        void print_usage(const char* executable)
        {
            std::cout << "Usage: " << executable << " <config.json>\n\n"
                      << "Sample configs are available at:\n"
                      << "  " << CONFIG_DEMO_SAMPLE_DIR << "\n";
        }
    } // namespace
} // namespace config_demo::v1

int main(
    int argc,
    char* argv[])
{
    using namespace config_demo::v1;

    if (argc != 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")
    {
        print_usage(argv[0]);
        return argc == 2 ? 0 : 1;
    }

    try
    {
        const std::filesystem::path config_path(argv[1]);
        const auto settings = load_demo_settings(config_path);
        const auto& execution = settings.execution;
        auto runtime = make_connection_factory_runtime(settings, config_path.parent_path());
        auto scheduler_1 = make_scheduler(execution.scheduler_threads);
        auto scheduler_2 = make_scheduler(execution.scheduler_threads);

        const auto error_code = run_configured_demo(*runtime, execution, scheduler_1, scheduler_2);
        runtime.reset();

        scheduler_1->shutdown();
        scheduler_2->shutdown();

        if (error_code != rpc::error::OK())
            return 1;

        RPC_INFO("config_demo completed successfully");
        return 0;
    }
    catch (const std::exception& error)
    {
        RPC_ERROR("config_demo failed: {}", error.what());
        return 1;
    }
}
