/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>
#include <rpc/rpc.h>

#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/i_telemetry_service.h>
#endif

// Other global variables used by the test setup classes
extern std::weak_ptr<rpc::service> current_host_service; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
extern std::string telemetry_config;                     // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef _WIN32                    // windows
extern std::string enclave_path; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#else                            // Linux
extern std::string enclave_path; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#endif
