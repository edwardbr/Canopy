// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_client_connection.h
#pragma once

#include <memory>
#include <string>

#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <websocket_demo/websocket_demo.h>

#include "websocket_service.h"
#include "ws_stream.h"
#include "transport.h"

#include <canopy/network_config/network_args.h>

namespace websocket_demo
{
    namespace v1
    {
        class i_calculator;

        enum class connection_state
        {
            awaiting_handshake, // waiting for the initial connect_request
            running,            // session active — normal RPC dispatch
            closed              // connection gone
        };

        class ws_client_connection
        {
        public:
            // Constructor takes a WebSocket stream and the shared service
            explicit ws_client_connection(std::shared_ptr<ws_stream> ws, std::shared_ptr<websocket_service> service);

            ~ws_client_connection() = default;

            // Non-copyable and non-movable
            ws_client_connection(const ws_client_connection&) = delete;
            ws_client_connection& operator=(const ws_client_connection&) = delete;
            ws_client_connection(ws_client_connection&&) = delete;
            ws_client_connection& operator=(ws_client_connection&&) = delete;

            // Main coroutine. Waits for the client's connect_request, sets up the
            // zone, then runs the message loop.
            auto run() -> coro::task<void>;

        private:
            // run() helpers
            coro::task<bool> wait_for_handshake();
            coro::task<bool> setup_zone();
            void handle_envelope(const std::span<const char> payload);

            // Member data
            std::shared_ptr<ws_stream> ws_;
            std::shared_ptr<websocket_service> service_;
            std::string msg_buffer_;
            std::shared_ptr<websocket_demo::v1::transport> transport_;

            connection_state state_{connection_state::awaiting_handshake};
            rpc::remote_object inbound_remote_object_;
        };
    }
}
