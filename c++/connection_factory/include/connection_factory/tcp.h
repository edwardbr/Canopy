/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE
#  include <connection_factory/tcp_coroutine.h>
#else
#  include <connection_factory/tcp_blocking.h>
#endif

namespace rpc::tcp
{
#ifdef CANOPY_BUILD_COROUTINE
    using namespace ::rpc::tcp_coroutine;

    inline const json::v1::object& tcp_default_options()
    {
        return ::rpc::tcp_coroutine::tcp_coroutine_default_options();
    }

    using materialise_tcp_options_result = ::rpc::tcp_coroutine::materialise_tcp_coroutine_options_result;

    inline const json::v1::object& tcp_options_schema()
    {
        return ::rpc::tcp_coroutine::tcp_coroutine_options_schema();
    }

    inline materialise_tcp_options_result materialise_tcp_options(const json::v1::object& client_options)
    {
        return ::rpc::tcp_coroutine::materialise_tcp_coroutine_options(client_options);
    }
#else
    using namespace ::rpc::tcp_blocking;

    inline const json::v1::object& tcp_default_options()
    {
        return ::rpc::tcp_blocking::tcp_blocking_default_options();
    }

    using materialise_tcp_options_result = ::rpc::tcp_blocking::materialise_tcp_blocking_options_result;

    inline const json::v1::object& tcp_options_schema()
    {
        return ::rpc::tcp_blocking::tcp_blocking_options_schema();
    }

    inline materialise_tcp_options_result materialise_tcp_options(const json::v1::object& client_options)
    {
        return ::rpc::tcp_blocking::materialise_tcp_blocking_options(client_options);
    }
#endif
}
