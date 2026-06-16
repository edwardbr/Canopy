/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <connection_factory/handles.h>
#include <connection_factory/options.h>
#include <transports/streaming/factory.h>

namespace rpc::connection_factory
{
    using client_rpc_stream_transport_result = rpc::stream_transport::client_rpc_stream_transport_result;

    using rpc::stream_transport::accept_rpc_listener;
    using rpc::stream_transport::accept_rpc_stream;
    using rpc::stream_transport::connect_rpc_stream;
    using rpc::stream_transport::fixed_factory;
    using rpc::stream_transport::make_client_rpc_stream_transport;
    using rpc::stream_transport::start_rpc_listener;
} // namespace rpc::connection_factory
