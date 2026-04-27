/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <rpc/telemetry/i_telemetry_service.h>

namespace rpc::telemetry
{
    class multiplexing_telemetry_service : public i_telemetry_service
    {
    public:
        static bool create(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services);

        explicit multiplexing_telemetry_service(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services);
        ~multiplexing_telemetry_service() override;

        multiplexing_telemetry_service(const multiplexing_telemetry_service&) = delete;
        multiplexing_telemetry_service& operator=(const multiplexing_telemetry_service&) = delete;

        void add_child(std::shared_ptr<i_telemetry_service> child);
        [[nodiscard]] size_t get_child_count() const;
        void clear_children();

        void register_service_config(
            const std::string& type,
            const std::string& output_path = "");
        void start_test(
            const std::string& test_suite_name,
            const std::string& name);
        void reset_for_test();
    };
}

namespace rpc
{
    using multiplexing_telemetry_service = telemetry::multiplexing_telemetry_service;
}
