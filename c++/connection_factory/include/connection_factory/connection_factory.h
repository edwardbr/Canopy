/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <connection_factory/context.h>
#include <connection_factory/handles.h>
#include <connection_factory/options.h>
#include <connection_factory_config/connection_factory_config.h>

namespace rpc::connection_factory
{
    // Shared empty advanced context used by the simple overload defaults.
    // Most applications should not need to create a context.
    [[nodiscard]] const context& default_context();

    // Result for configured RPC accept operations. A stream listener returns
    // listener when it can accept many future connections. Single-shot stream
    // transports such as SPSC return connection after accepting one connection.
    struct accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<listener_handle> listener;
        std::shared_ptr<rpc_connection_handle> connection;
    };

    // Create a fully layered byte stream from connection_settings::stream_layers.
    // The first stream layer is the base stream, for example tcp_blocking,
    // tcp_coroutine, or spsc_queue. Remaining layers are applied in order, for
    // example TLS, WebSocket, attestation, or application-registered layers.
    //
    // This does not create an RPC transport. Use it when an application needs
    // configured stream construction for a protocol other than Canopy RPC, or
    // when building a custom transport on top of the stream factory.
    CORO_TASK(stream_result)
    connect_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context());

    // Accept one fully layered byte stream from connection_settings::stream_layers.
    // This is for single-shot base streams whose two endpoints are already
    // paired by configuration, such as SPSC. For listening sockets that accept
    // many connections, use open_stream_acceptor or accept_streams instead.
    CORO_TASK(stream_result)
    accept_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context());

    // Create a configured base stream acceptor without attaching RPC. This
    // returns the listening acceptor and any owner object needed to keep its
    // runtime alive. Per-connection wrapper layers are not applied here; callers
    // that consume the acceptor directly are responsible for wrapping accepted
    // streams, or can use accept_streams to have the factory do that.
    CORO_TASK(stream_acceptor_result)
    open_stream_acceptor(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context());

    // Start a configured stream listener and invoke callback for each fully
    // layered accepted stream. This is the raw-stream equivalent of accept_rpc:
    // it owns the accept loop through the returned stream_accept_handle, but it
    // does not create Canopy RPC transports.
    CORO_TASK(stream_accept_result)
    accept_streams(
        stream_callback callback,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context());

    // Primary RPC factory API. These are the functions most applications
    // should use when configuration owns the connection topology.

    // Connect to a configured RPC peer and return the generated output
    // interface. For transport type stream_rpc, the factory first creates the
    // configured stream stack and then attaches the stream RPC transport. For
    // native transports such as local, the named transport factory owns its own
    // construction and may ignore stream layers when they do not apply.
    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context());

    // Accept configured RPC connections using a factory callback. For listening
    // base streams, the returned listener handle keeps the listener alive. For
    // single-shot base streams, the returned connection handle keeps the
    // accepted transport alive. The factory callback is invoked with the remote
    // interface and service so the application can create the local interface
    // for each accepted RPC peer.
    template<
        class Remote,
        class Local>
    CORO_TASK(accept_result)
    accept_rpc(
        rpc_factory<
            Remote,
            Local> factory,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context(),
        rpc_transport_observer observe_transport = {});

    // Convenience accept_rpc overload for the common case where every accepted
    // connection should use the same local interface instance. Use the factory
    // callback overload when each peer needs distinct local state.
    template<
        class Remote,
        class Local>
    CORO_TASK(accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        context factory_context = default_context(),
        rpc_transport_observer observe_transport = {});
} // namespace rpc::connection_factory

#include <connection_factory/detail/connection_factory_impl.h>
