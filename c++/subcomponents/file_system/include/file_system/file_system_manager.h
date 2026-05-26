/*
 * Copyright (c) 2026 Edward Boggis-Rolfe
 * All rights reserved.
 */

#pragma once

#include <rpc/rpc.h>

#include <file_system/file_system.h>

#include <memory>

#ifdef CANOPY_BUILD_COROUTINE
#  include <io_uring/controller.h>
#endif

namespace rpc
{
    namespace file_system
    {
        inline namespace v1
        {
#ifdef CANOPY_BUILD_COROUTINE
            // Coroutine builds back the manager with an io_uring controller for
            // async file I/O. Caller supplies the controller.
            rpc::shared_ptr<i_manager> create_factory(std::shared_ptr<rpc::io_uring::controller>);
#else
            // Blocking builds back the manager with plain POSIX
            // ::open/::read/::write/::close. No async controller needed.
            // Calls execute synchronously on the calling thread, so dispatch
            // through an rpc::executor if you need to keep your main thread
            // unblocked.
            rpc::shared_ptr<i_manager> create_factory();
#endif
        }
    }
}
