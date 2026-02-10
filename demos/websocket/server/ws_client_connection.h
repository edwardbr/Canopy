// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_client_connection.h
#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <coro/coro.hpp>
#include <wslay/wslay.h>
#include <rpc/rpc.h>

#include "websocket_service.h"
#include "transport.h"
#include "stream.h"

namespace websocket_demo
{
    namespace v1
    {
        class i_calculator;

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

            // Main coroutine that runs the WebSocket message loop
            auto run() -> coro::task<void>;

        private:
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
        };
    }
}
