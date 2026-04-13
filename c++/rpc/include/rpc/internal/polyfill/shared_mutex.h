/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <mutex>

#ifdef FOR_SGX
namespace rpc
{
    using shared_mutex = std::mutex;
    template<typename Mutex> using shared_lock = std::unique_lock<Mutex>;
}
#else
#  include <shared_mutex>

namespace rpc
{
    using shared_mutex = std::shared_mutex;
    template<typename Mutex> using shared_lock = std::shared_lock<Mutex>;
}
#endif
