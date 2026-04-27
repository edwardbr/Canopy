/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/telemetry/i_telemetry_service.h>
#include <rpc/telemetry/telemetry_service_factory.h>

namespace rpc::telemetry
{
#if defined(CANOPY_USE_TELEMETRY) && !defined(FOR_SGX)
    std::shared_ptr<i_telemetry_service> telemetry_service_ = nullptr;
#endif

#ifndef FOR_SGX
    bool create_test_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& type,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        if (type == "animation")
            return create_animation_telemetry_service(service, test_suite_name, name, directory);
        if (type == "console")
            return create_console_telemetry_service(service, test_suite_name, name, directory);
        if (type == "sequence")
            return create_sequence_diagram_telemetry_service(service, test_suite_name, name, directory);
        return false;
    }
#endif
}
