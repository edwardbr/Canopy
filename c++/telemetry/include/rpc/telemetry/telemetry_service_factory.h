/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <rpc/telemetry/i_telemetry_service.h>

#ifndef FOR_SGX
#  include <filesystem>
#endif

namespace rpc::telemetry
{
#ifndef FOR_SGX
    /**
     * Creates an animation telemetry sink that writes an HTML report for a single test.
     */
    bool create_animation_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory);

    /**
     * Creates a console/file telemetry sink for a single test.
     */
    bool create_console_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory);

    /**
     * Creates a PlantUML sequence diagram telemetry sink for a single test.
     */
    bool create_sequence_diagram_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory);

    /**
     * Creates one of the named test telemetry sinks: "animation", "console", or "sequence".
     */
    bool create_test_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& type,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory);

    /**
     * Creates a multiplexer sink that forwards events to the supplied child sinks.
     */
    bool create_multiplexing_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        std::vector<std::shared_ptr<i_telemetry_service>>&& child_services);

    /**
     * Creates the process-global telemetry multiplexer used by rpc::telemetry::get_telemetry_service().
     */
    bool create_global_multiplexing_telemetry_service(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services);

    /**
     * Adds a child sink to a multiplexer returned as an i_telemetry_service pointer.
     */
    bool add_telemetry_child(
        const std::shared_ptr<i_telemetry_service>& service,
        std::shared_ptr<i_telemetry_service> child);

    /**
     * Registers a sink type to be recreated for each test when the supplied service is a multiplexer.
     */
    bool register_telemetry_service_config(
        const std::shared_ptr<i_telemetry_service>& service,
        const std::string& type,
        const std::string& output_path = "");

    /**
     * Resets a multiplexer for a new test and creates its configured child sinks.
     */
    bool start_telemetry_test(
        const std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name);

    /**
     * Clears the per-test child sinks owned by a multiplexer.
     */
    bool reset_telemetry_for_test(const std::shared_ptr<i_telemetry_service>& service);
#endif
}
