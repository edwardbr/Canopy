// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_client_connection.cpp
#include "ws_client_connection.h"
#include <coro/coro.hpp>
#include <iostream>
#include <cstring>
#include <atomic>

#include <fmt/format.h>
#include <secret_llama/secret_llama.h>
#include <secret_llama/secret_llama_stub.h>
#include <websocket_demo/websocket_demo.h>
#include "transport.h"

namespace websocket_demo
{
    namespace v1
    {
        ws_client_connection::ws_client_connection(std::shared_ptr<stream> stream, std::shared_ptr<websocket_service> service)
            : stream_(std::move(stream))
            , service_(std::move(service))
            , buffer_(4096, '\0')
            , pending_messages_(std::make_shared<std::queue<std::vector<uint8_t>>>())
            , pending_messages_mutex_(std::make_shared<std::mutex>())
        {
            // Set up wslay callbacks
            wslay_event_callbacks callbacks;
            std::memset(&callbacks, 0, sizeof(callbacks));
            callbacks.recv_callback = recv_callback;
            callbacks.send_callback = send_callback;
            callbacks.on_msg_recv_callback = on_msg_recv_callback;

            // Initialize wslay context with 'this' pointer
            int result = wslay_event_context_server_init(&wslay_ctx_, &callbacks, this);
            if (result != 0)
            {
                throw std::runtime_error("Failed to initialize wslay context");
            }
        }

        ws_client_connection::~ws_client_connection()
        {
            if (wslay_ctx_ != nullptr)
            {
                wslay_event_context_free(wslay_ctx_);
                wslay_ctx_ = nullptr;
            }
        }

        coro::task<void> ws_client_connection::run()
        {
            try
            {
                std::cout << "[WS] Creating transport" << std::endl;
                transport_ = std::make_shared<transport>(
                    wslay_ctx_, service_, service_->generate_new_zone_id(), pending_messages_, pending_messages_mutex_);

                // this should occur in attach_remote_zone... later
                service_->add_transport({2}, transport_);

                rpc::interface_descriptor output_descr;

                std::cout << "[WS] Calling attach_remote_zone" << std::endl;
                auto ret
                    = CO_AWAIT service_->attach_remote_zone<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                        "websocket",
                        transport_,
                        rpc::interface_descriptor{1, 2}, // this magically makes sink
                        output_descr,
                        [](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                            rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                            const std::shared_ptr<rpc::service>& svc) -> coro::task<int>
                        {
                            std::cout << "[WS] Inside attach_remote_zone lambda" << std::endl;
                            secret_llama_idl_register_stubs(svc);

                            auto wsrvc = std::static_pointer_cast<websocket_service>(svc);

                            local = wsrvc->get_demo_instance();
                            std::cout << "[WS] Calling set_callback" << std::endl;
                            CO_AWAIT local->set_callback(sink);
                            std::cout << "[WS] set_callback completed" << std::endl;
                            co_return 0;
                        });

                std::cout << "[WS] attach_remote_zone returned: " << ret << std::endl;
                std::cout << "[WS] Entering WebSocket message loop" << std::endl;

                while (true)
                {
                    // Drain pending messages queue into wslay
                    {
                        std::lock_guard<std::mutex> pending_lock(*pending_messages_mutex_);
                        if (!pending_messages_->empty())
                        {
                            std::lock_guard<std::mutex> wslay_lock(wslay_mutex_);
                            while (!pending_messages_->empty())
                            {
                                auto& msg_data = pending_messages_->front();

                                wslay_event_msg msg;
                                msg.opcode = WSLAY_BINARY_FRAME;
                                msg.msg = msg_data.data();
                                msg.msg_length = msg_data.size();

                                int result = wslay_event_queue_msg(wslay_ctx_, &msg);
                                if (result != 0)
                                {
                                    std::cerr << "Failed to queue WebSocket message from pending queue: " << result
                                              << std::endl;
                                }
                                else
                                {
                                    std::cout << "Drained message from pending queue to wslay" << std::endl;
                                }

                                pending_messages_->pop();
                            }
                        }
                    }

                    // Check if wslay wants to read or write (with mutex protection)
                    bool want_read, want_write;
                    {
                        std::lock_guard<std::mutex> lock(wslay_mutex_);
                        want_read = wslay_event_want_read(wslay_ctx_) != 0;
                        want_write = wslay_event_want_write(wslay_ctx_) != 0;
                    }

                    std::cout << "Loop: want_read=" << want_read << ", want_write=" << want_write << std::endl;

                    if (!want_read && !want_write)
                    {
                        std::cout << "WebSocket connection closing normally" << std::endl;
                        break;
                    }

                    // Handle writing first (prioritize outgoing streaming data)
                    if (want_write)
                    {
                        std::cout << "Polling for write..." << std::endl;
                        co_await stream_->poll(coro::poll_op::write);
                        std::cout << "Sending data..." << std::endl;
                        {
                            std::lock_guard<std::mutex> lock(wslay_mutex_);
                            int r = wslay_event_send(wslay_ctx_);
                            if (r != 0)
                            {
                                std::cerr << "wslay_event_send error: " << r << std::endl;
                                break;
                            }
                            std::cout << "Send complete" << std::endl;
                            // Re-check want_write after sending
                            want_write = wslay_event_want_write(wslay_ctx_) != 0;
                        }
                    }

                    // Handle reading (with timeout to check for queued writes)
                    if (want_read)
                    {
                        // Use a short timeout to avoid blocking when async writes are queued
                        // Poll for both read and write to catch any activity
                        auto poll_status
                            = co_await stream_->poll(coro::poll_op::read_write, std::chrono::milliseconds{50});

                        // Only try to receive if the poll succeeded or data is available
                        if (poll_status == coro::poll_status::event)
                        {
                            auto [recv_status, recv_span] = stream_->recv(buffer_);

                            if (recv_status == coro::net::recv_status::closed)
                            {
                                std::cout << "Client disconnected" << std::endl;
                                stream_->set_closed();
                                break;
                            }
                            else if (recv_status == coro::net::recv_status::ok && !recv_span.empty())
                            {
                                // Store the received data in our read buffer
                                read_buffer_.assign(recv_span.begin(), recv_span.end());
                                read_buffer_pos_ = 0;

                                // Let wslay process the received data (with mutex protection)
                                {
                                    std::lock_guard<std::mutex> lock(wslay_mutex_);
                                    int r = wslay_event_recv(wslay_ctx_);
                                    if (r != 0)
                                    {
                                        std::cerr << "wslay_event_recv error: " << r << std::endl;
                                        break;
                                    }
                                }
                            }
                        }
                        // If timeout occurred (poll_status == timeout), loop will continue and check want_write
                    }
                }

                std::cout << "WebSocket connection closed" << std::endl;
            }
            catch (const std::exception& e)
            {
                std::cerr << "Exception in ws_client_connection::run: " << e.what() << std::endl;
            }

            co_return;
        }

        // Callback: wslay wants to send data to the peer
        ssize_t ws_client_connection::send_callback(
            wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data)
        {
            auto* self = static_cast<ws_client_connection*>(user_data);

            // Check if connection is already closed to prevent sending on a dead socket
            if (self->stream_->is_closed())
            {
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return -1;
            }

            // Send data through stream
            auto [status, remaining]
                = self->stream_->send(std::span<const char>(reinterpret_cast<const char*>(data), len));

            if (status == coro::net::send_status::ok)
            {
                // Calculate bytes sent: original length - remaining unsent bytes
                size_t bytes_sent = len - remaining.size();
                return static_cast<ssize_t>(bytes_sent);
            }
            else if (status == coro::net::send_status::would_block)
            {
                wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
                return -1;
            }
            else
            {
                // Mark connection as closed to prevent further send attempts
                self->stream_->set_closed();
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return -1;
            }
        }

        // Callback: wslay wants to receive data from the peer
        ssize_t ws_client_connection::recv_callback(
            wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data)
        {
            auto* self = static_cast<ws_client_connection*>(user_data);

            // If we have data in our buffer, return it
            if (self->read_buffer_pos_ < self->read_buffer_.size())
            {
                size_t available = self->read_buffer_.size() - self->read_buffer_pos_;
                size_t to_copy = std::min(len, available);
                std::memcpy(buf, self->read_buffer_.data() + self->read_buffer_pos_, to_copy);
                self->read_buffer_pos_ += to_copy;
                return static_cast<ssize_t>(to_copy);
            }

            // No more data available, signal would block
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }

        // Callback: wslay received a complete message
        void ws_client_connection::on_msg_recv_callback(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data)
        {
            auto* self = static_cast<ws_client_connection*>(user_data);

            if (!wslay_is_ctrl_frame(arg->opcode))
            {
                // Log the message
                std::cout << "Received message (" << arg->msg_length << " bytes): ";
                if (arg->opcode == WSLAY_TEXT_FRAME)
                {
                    std::cout << std::string(reinterpret_cast<const char*>(arg->msg), arg->msg_length);

                    // Echo back the received message
                    wslay_event_msg msg;
                    msg.opcode = arg->opcode; // TEXT or BINARY
                    msg.msg = arg->msg;
                    msg.msg_length = arg->msg_length;

                    // Queue the message for sending
                    wslay_event_queue_msg(ctx, &msg);
                }
                else
                {
                    std::cout << "[binary data]";

                    websocket_demo::v1::envelope envelope;
                    auto error = rpc::from_protobuf<websocket_demo::v1::envelope>(
                        {arg->msg, arg->msg + arg->msg_length}, envelope);
                    if (error.length())
                    {
                        auto reason = fmt::format("invalid message format {}", error);
                        std::cout << "Received message (" << arg->msg_length << " bytes) parsing error: " << error;

                        wslay_event_queue_close(ctx,
                            WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, // 1007
                            reinterpret_cast<const uint8_t*>(reason.data()),
                            reason.size());
                        return; // no reply.
                    }

                    if (envelope.message_type == rpc::id<websocket_demo::v1::request>::get(rpc::get_version()))
                    {
                        self->service_->get_scheduler()->spawn(self->transport_->stub_handle_send(std::move(envelope)));
                    }
                    else if (envelope.message_type == rpc::id<websocket_demo::v1::response>::get(rpc::get_version()))
                    {
                        // process response from client
                        websocket_demo::v1::response response;
                        auto error = rpc::from_protobuf<websocket_demo::v1::response>(envelope.data, response);
                        if (error.length())
                        {
                            auto reason = fmt::format("invalid message format {}", error);
                            std::cout << "Received message (" << arg->msg_length << " bytes) parsing error: " << error;

                            wslay_event_queue_close(ctx,
                                WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, // 1007
                                reinterpret_cast<const uint8_t*>(reason.data()),
                                reason.size());
                            return; // no reply.
                        }
                    }
                    else
                    {
                        auto reason = fmt::format("invalid message format {}", error);
                        std::cout << "Received message (" << arg->msg_length << " bytes) parsing error: " << error;

                        wslay_event_queue_close(ctx,
                            WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, // 1007
                            reinterpret_cast<const uint8_t*>(reason.data()),
                            reason.size());
                        return; // no reply.
                    }
                }

                std::cout << std::endl;
            }
            else if (arg->opcode == WSLAY_CONNECTION_CLOSE)
            {
                std::cout << "Connection close received, status code: " << arg->status_code << std::endl;
            }
        }
    }
}
