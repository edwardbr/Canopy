/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <rpc/telemetry/i_telemetry_service.h>

namespace rpc::telemetry
{
    class console_telemetry_service : public i_telemetry_service
    {
    public:
        static bool create(
            std::shared_ptr<i_telemetry_service>& service,
            const std::string& test_suite_name,
            const std::string& name,
            const std::filesystem::path& directory);
    };
}

namespace rpc
{
    using console_telemetry_service = telemetry::console_telemetry_service;
}
