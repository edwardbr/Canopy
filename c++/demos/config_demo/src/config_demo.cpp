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
        auto configuration = load_demo_configuration(config_path);
        auto scheduler_1 = make_scheduler(configuration.settings.scheduler_threads);
        auto scheduler_2 = make_scheduler(configuration.settings.scheduler_threads);

        const bool ok = run_configured_demo(*configuration.runtime, configuration.settings, scheduler_1, scheduler_2);
        configuration.runtime.reset();

        scheduler_1->shutdown();
        scheduler_2->shutdown();

        if (!ok)
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
