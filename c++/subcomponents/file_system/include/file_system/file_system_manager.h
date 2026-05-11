/*
 * Copyright (c) 2026 Edward Boggis-Rolfe
 * All rights reserved.
 */

#pragma once

#include <rpc/rpc.h>

#include <file_system/file_system.h>
#include <io_uring/controller.h>

#include <memory>

namespace rpc
{
    namespace file_system
    {
        inline namespace v1
        {
            // file manager factory
            rpc::shared_ptr<i_manager> create_factory(std::shared_ptr<rpc::io_uring::controller>);
        }
    }
}
