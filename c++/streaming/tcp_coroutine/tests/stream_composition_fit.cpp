/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <memory>
#include <type_traits>

#include <io_uring/direct_descriptor.h>
#include <streaming/tcp_coroutine/stream.h>
#include <streaming/stream.h>
#include <streaming/secure_stream.h>
#include <streaming/websocket/stream.h>
#include <transports/streaming/transport.h>

static_assert(std::is_base_of_v<
    streaming::stream,
    streaming::coroutine::tcp::stream>);

namespace
{
    void compile_fit_stream_layers()
    {
        std::shared_ptr<rpc::io_uring::controller> controller;
        auto descriptor = std::make_shared<rpc::io_uring::direct_descriptor>(controller, 0);
        std::shared_ptr<streaming::stream> base_stream
            = std::make_shared<streaming::coroutine::tcp::stream>(std::move(descriptor), uint16_t{443});

        // The TCP coroutine stream is just another streaming::stream. TLS,
        // websocket, and streaming transport code should compose with it
        // without requiring the stream adapter to know whether the underlying
        // descriptor is socket-backed, file-backed, or something else.
        std::shared_ptr<streaming::secure::client_context> tls_client_context;
        [[maybe_unused]] std::shared_ptr<streaming::stream> tls_stream
            = std::make_shared<streaming::secure::stream>(base_stream, tls_client_context);
        [[maybe_unused]] std::shared_ptr<streaming::stream> websocket_stream
            = std::make_shared<streaming::websocket::stream>(base_stream);
        [[maybe_unused]] std::shared_ptr<rpc::stream_transport::transport> transport;
    }
} // namespace

int main()
{
    compile_fit_stream_layers();
    return 0;
}
