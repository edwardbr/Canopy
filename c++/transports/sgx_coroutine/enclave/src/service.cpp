/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/service.h>

namespace rpc
{
    enclave_service::~enclave_service()
    {
        if (auto controller = get_io_uring_controller())
            controller->request_shutdown();
    }
}
