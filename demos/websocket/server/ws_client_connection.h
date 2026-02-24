// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_client_connection.h
#pragma once

#include <optional>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <coro/coro.hpp>
#include <wslay/wslay.h>
#include <rpc/rpc.h>
#include <websocket_demo/websocket_demo.h>

#include "websocket_service.h"
#include "transport.h"
#include "stream.h"

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
            // Constructor takes a stream (can be plain TCP or TLS)
            explicit ws_client_connection(std::shared_ptr<stream> stream, std::shared_ptr<websocket_service> service);

            // Destructor handles cleanup
            ~ws_client_connection();

            // Non-copyable and non-movable (contains wslay context pointer)
            ws_client_connection(const ws_client_connection&) = delete;
            ws_client_connection& operator=(const ws_client_connection&) = delete;
            ws_client_connection(ws_client_connection&&) = delete;
            ws_client_connection& operator=(ws_client_connection&&) = delete;

            // Main coroutine.  Waits for the client's connect_request (the callback
            // fires this transition), then sets up the zone and runs the message loop.
            auto run() -> coro::task<void>;

        private:
            // run() helpers
            void feed_recv_data(std::span<const char> data);
            coro::task<bool> wait_for_handshake();
            coro::task<bool> setup_zone();
            void drain_pending_messages();
            coro::task<bool> do_write();
            coro::task<bool> do_read();

            // on_msg_recv_callback helpers
            void handle_binary_handshake(wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg);
            void handle_binary_envelope(wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg);
            void close_with_parse_error(wslay_event_context_ptr ctx, size_t msg_length, const std::string& error);

            // wslay callback functions (static members)
            static ssize_t send_callback(
                wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data);

            static ssize_t recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data);

            static void on_msg_recv_callback(
                wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data);

            // Member data
            std::shared_ptr<stream> stream_;
            std::shared_ptr<websocket_service> service_;
            std::string read_buffer_;
            size_t read_buffer_pos_{0};
            wslay_event_context_ptr wslay_ctx_{nullptr};
            std::mutex wslay_mutex_; // Protect wslay_ctx_ from concurrent access
            std::string buffer_;
            std::shared_ptr<websocket_demo::v1::transport> transport_;

            // Separate queue for async messages (avoids deadlock with wslay_mutex_)
            std::shared_ptr<std::queue<std::vector<uint8_t>>> pending_messages_;
            std::shared_ptr<std::mutex> pending_messages_mutex_;

            connection_state state_{connection_state::awaiting_handshake};

            // Set by on_msg_recv_callback when the connect_request arrives;
            // consumed once by run() then left empty for the rest of the session.
            uint64_t client_object_id_;
        };
    }
}
