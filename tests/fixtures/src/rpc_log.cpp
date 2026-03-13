/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <chrono>
#include <iostream>
#include <thread>

// RPC headers
#include <rpc/rpc.h>
#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/i_telemetry_service.h>
#include <rpc/telemetry/telemetry_handler.h>
#endif

// Other headers
#include "rpc_global_logger.h"

using namespace std::chrono_literals;

extern std::weak_ptr<rpc::service> current_host_service;

// an ocall for logging the test
extern "C"
{

    void rpc_log(int level, const char* str, size_t sz)
    {
        std::string message(str, sz);
        switch (level)
        {
        case 0:
            rpc_global_logger::debug(message);
            break;
        case 1:
            rpc_global_logger::trace(message);
            break;
        case 2:
            rpc_global_logger::info(message);
            break;
        case 3:
            rpc_global_logger::warn(message);
            break;
        case 4:
            rpc_global_logger::error(message);
            break;
        case 5:
            rpc_global_logger::critical(message);
            break;
        default:
            rpc_global_logger::info(message);
            break;
        }
    }

    void hang()
    {
        std::cerr << "hanging for debugger\n";
        bool loop = true;
        while (loop)
        {
            std::this_thread::sleep_for(1s);
        }
    }
}
