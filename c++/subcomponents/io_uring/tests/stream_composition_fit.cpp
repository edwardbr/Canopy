/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <memory>
#include <type_traits>

#include <io_uring/tcp.h>
#include <rpc/rpc.h>
#include <streaming/tls/stream.h>
#include <streaming/websocket/stream.h>
#include <transports/streaming/transport.h>

static_assert(std::is_base_of_v<
    streaming::stream,
    rpc::io_uring::stream>);
static_assert(std::is_base_of_v<
    streaming::stream,
    streaming::tls::stream>);
static_assert(std::is_base_of_v<
    streaming::stream,
    streaming::websocket::stream>);
static_assert(std::is_constructible_v<
    rpc::io_uring::stream,
    std::shared_ptr<rpc::io_uring::direct_descriptor>,
    uint16_t>);
static_assert(std::is_constructible_v<
    streaming::tls::stream,
    std::shared_ptr<streaming::stream>,
    std::shared_ptr<streaming::tls::client_context>>);
static_assert(std::is_constructible_v<
    streaming::websocket::stream,
    std::shared_ptr<streaming::stream>>);

extern "C" auto canopy_io_uring_stream_composition_fit() -> int
{
    std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor;
    std::shared_ptr<streaming::stream> base
        = std::make_shared<rpc::io_uring::stream>(std::move(descriptor), uint16_t{443});

    auto tls_context = std::make_shared<streaming::tls::client_context>(false);
    std::shared_ptr<streaming::stream> tls
        = std::make_shared<streaming::tls::stream>(std::move(base), std::move(tls_context));

    std::shared_ptr<streaming::stream> websocket = std::make_shared<streaming::websocket::stream>(std::move(tls));

    std::shared_ptr<rpc::service> service;
    auto transport = rpc::stream_transport::make_server(
        "io_uring_stream_composition_fit",
        std::move(service),
        std::move(websocket),
        rpc::stream_transport::transport::connection_handler{},
        {});
    return transport ? 0 : 1;
}

#ifndef FOR_SGX
auto main() -> int
{
    // Keep the composition body linked without running it. The transport fit
    // uses a null service because this target only verifies compile/link shape.
    using fit_function = int (*)();
    volatile fit_function fit = &canopy_io_uring_stream_composition_fit;
    return fit ? 0 : 1;
}
#endif
