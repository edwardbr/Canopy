/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <transports/streaming/factory.h>

namespace rpc::connection_factory
{
    template<class Remote, class Local> using rpc_factory = rpc::stream_transport::rpc_factory<Remote, Local>;

    using rpc_transport_observer = rpc::stream_transport::rpc_transport_observer;

    using stream_result = rpc::stream_transport::stream_result;
    using stream_acceptor_result = rpc::stream_transport::stream_acceptor_result;
    using stream_callback = rpc::stream_transport::stream_callback;
    using stream_accept_handle = rpc::stream_transport::stream_accept_handle;
    using stream_accept_result = rpc::stream_transport::stream_accept_result;
    using listener_handle = rpc::stream_transport::listener_handle;
    using listener_result = rpc::stream_transport::listener_result;
    using rpc_connection_handle = rpc::stream_transport::rpc_connection_handle;
    using rpc_accept_result = rpc::stream_transport::rpc_accept_result;

    using rpc::stream_transport::accept_streams;
    using rpc::stream_transport::keep_owner;
} // namespace rpc::connection_factory
