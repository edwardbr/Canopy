// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_client_connection.cpp
#include "ws_client_connection.h"
#include <coro/coro.hpp>
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
            wslay_event_callbacks callbacks;
            std::memset(&callbacks, 0, sizeof(callbacks));
            callbacks.recv_callback = recv_callback;
            callbacks.send_callback = send_callback;
            callbacks.on_msg_recv_callback = on_msg_recv_callback;

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

        // -----------------------------------------------------------------------
        // run() helpers
        // -----------------------------------------------------------------------

        // Populate the read buffer so the wslay recv_callback can consume it.
        void ws_client_connection::feed_recv_data(std::span<const char> data)
        {
            read_buffer_.assign(data.begin(), data.end());
            read_buffer_pos_ = 0;
        }

        // Wait for the client's initial connect_request binary frame.
        // Returns false if the connection closed before the handshake completed.
        coro::task<bool> ws_client_connection::wait_for_handshake()
        {
            RPC_INFO("[WS] Waiting for connect_request handshake");
            while (state_ == connection_state::awaiting_handshake)
            {
                auto poll_status = co_await stream_->poll(coro::poll_op::read, std::chrono::milliseconds{100});
                if (poll_status != coro::poll_status::event)
                    continue;

                auto [recv_status, recv_span] = stream_->recv(buffer_);
                if (recv_status == coro::net::recv_status::closed)
                {
                    state_ = connection_state::closed;
                    co_return false;
                }
                if (recv_status == coro::net::recv_status::ok && !recv_span.empty())
                {
                    feed_recv_data(recv_span);
                    std::lock_guard<std::mutex> lock(wslay_mutex_);
                    wslay_event_recv(wslay_ctx_);
                }
            }
            co_return state_ != connection_state::closed;
        }

        // Attach the remote zone, wire up the local calculator instance, and
        // send the connect_response back to the client.
        coro::task<bool> ws_client_connection::setup_zone()
        {
            transport_ = std::make_shared<transport>(
                wslay_ctx_, service_, service_->generate_new_zone_id(), pending_messages_, pending_messages_mutex_);

            RPC_INFO("[WS] connect_request received, inbound_remote_object={}", inbound_remote_object_.get_val());
            RPC_INFO("[WS] Calling attach_remote_zone");

            rpc::interface_descriptor output_descr;
            auto ret
                = CO_AWAIT service_->attach_remote_zone<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                    "websocket",
                    transport_,
                    [&]()
                    {
                        rpc::connection_settings cs;
                        cs.inbound_interface_id = websocket_demo::v1::i_context_event::get_id(rpc::get_version());
                        cs.outbound_interface_id = websocket_demo::v1::i_calculator::get_id(rpc::get_version());
                        cs.input_zone_id = transport_->get_adjacent_zone_id().as_destination().with_object(
                            inbound_remote_object_.get_object());
                        return cs;
                    }(),
                    output_descr,
                    [](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                        rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                        const std::shared_ptr<rpc::service>& svc) -> coro::task<int>
                    {
                        auto wsrvc = std::static_pointer_cast<websocket_service>(svc);
                        local = wsrvc->get_demo_instance();
                        if (!local)
                        {
                            RPC_ERROR("[WS] get_demo_instance returned null");
                            co_return rpc::error::OBJECT_NOT_FOUND();
                        }
                        if (sink)
                        {
                            RPC_INFO("[WS] Calling set_callback");
                            CO_AWAIT local->set_callback(sink);
                            RPC_INFO("[WS] set_callback completed");
                        }
                        co_return 0;
                    });

            RPC_INFO(
                "[WS] attach_remote_zone returned: {}, server_object_id={}", ret, output_descr.get_object_id().get_val());

            // Queue the connect_response so the client knows the zone/object IDs.
            websocket_demo::v1::connect_response connect_resp;
            connect_resp.caller_zone_id = transport_->get_adjacent_zone_id().as_caller();
            connect_resp.outbound_remote_object = output_descr.destination_zone_id;

            auto resp_payload = rpc::to_protobuf<std::vector<uint8_t>>(connect_resp);
            {
                std::lock_guard<std::mutex> lock(*pending_messages_mutex_);
                pending_messages_->push(std::move(resp_payload));
            }
            RPC_INFO("[WS] connect_response queued: caller_zone={} outbound_remote_object={}",
                connect_resp.caller_zone_id.get_val(),
                connect_resp.outbound_remote_object.get_val());

            co_return true;
        }

        // Drain any messages waiting in pending_messages_ into wslay.
        void ws_client_connection::drain_pending_messages()
        {
            std::lock_guard<std::mutex> pending_lock(*pending_messages_mutex_);
            if (pending_messages_->empty())
                return;

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
                    RPC_ERROR("Failed to queue WebSocket message from pending queue: {}", result);
                }
                else
                {
                    RPC_DEBUG("Drained message from pending queue to wslay");
                }

                pending_messages_->pop();
            }
        }

        // Flush any outgoing wslay data to the socket.  Returns false on error.
        coro::task<bool> ws_client_connection::do_write()
        {
            co_await stream_->poll(coro::poll_op::write);
            std::lock_guard<std::mutex> lock(wslay_mutex_);
            int r = wslay_event_send(wslay_ctx_);
            if (r != 0)
            {
                RPC_ERROR("wslay_event_send error: {}", r);
                co_return false;
            }
            co_return true;
        }

        // Read incoming data from the socket and feed it to wslay.
        // Returns false on a hard error; true on success or timeout.
        coro::task<bool> ws_client_connection::do_read()
        {
            auto poll_status = co_await stream_->poll(coro::poll_op::read, std::chrono::milliseconds{5});
            if (poll_status != coro::poll_status::event)
                co_return true; // timeout — loop back to check pending_messages_

            auto [recv_status, recv_span] = stream_->recv(buffer_);
            if (recv_status == coro::net::recv_status::closed)
            {
                RPC_INFO("Client disconnected");
                stream_->set_closed();
                co_return false;
            }
            if (recv_status == coro::net::recv_status::ok && !recv_span.empty())
            {
                feed_recv_data(recv_span);
                std::lock_guard<std::mutex> lock(wslay_mutex_);
                int r = wslay_event_recv(wslay_ctx_);
                if (r != 0)
                {
                    RPC_ERROR("wslay_event_recv error: {}", r);
                    co_return false;
                }
            }
            co_return true;
        }

        // -----------------------------------------------------------------------
        // Main coroutine
        // -----------------------------------------------------------------------

        coro::task<void> ws_client_connection::run()
        {
            try
            {
                if (!co_await wait_for_handshake())
                    co_return;

                if (!co_await setup_zone())
                    co_return;

                RPC_INFO("[WS] Entering WebSocket message loop");

                while (true)
                {
                    drain_pending_messages();

                    bool want_read, want_write;
                    {
                        std::lock_guard<std::mutex> lock(wslay_mutex_);
                        want_read = wslay_event_want_read(wslay_ctx_) != 0;
                        want_write = wslay_event_want_write(wslay_ctx_) != 0;
                    }

                    if (!want_read && !want_write)
                    {
                        RPC_INFO("WebSocket connection closing normally");
                        break;
                    }

                    // Prioritise outgoing streaming data.
                    if (want_write && !co_await do_write())
                        break;

                    if (want_read && !co_await do_read())
                        break;
                }

                RPC_INFO("WebSocket connection closed");
            }
            catch (const std::exception& e)
            {
                RPC_ERROR("Exception in ws_client_connection::run: {}", e.what());
            }

            co_return;
        }

        // -----------------------------------------------------------------------
        // wslay callbacks
        // -----------------------------------------------------------------------

        ssize_t ws_client_connection::send_callback(
            wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data)
        {
            auto* self = static_cast<ws_client_connection*>(user_data);

            if (self->stream_->is_closed())
            {
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return -1;
            }

            auto [status, remaining]
                = self->stream_->send(std::span<const char>(reinterpret_cast<const char*>(data), len));

            if (status == coro::net::send_status::ok)
                return static_cast<ssize_t>(len - remaining.size());

            if (status == coro::net::send_status::would_block)
            {
                wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
                return -1;
            }

            self->stream_->set_closed();
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }

        ssize_t ws_client_connection::recv_callback(
            wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data)
        {
            auto* self = static_cast<ws_client_connection*>(user_data);

            size_t available = self->read_buffer_.size() - self->read_buffer_pos_;
            if (available > 0)
            {
                size_t to_copy = std::min(len, available);
                std::memcpy(buf, self->read_buffer_.data() + self->read_buffer_pos_, to_copy);
                self->read_buffer_pos_ += to_copy;
                return static_cast<ssize_t>(to_copy);
            }

            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }

        // -----------------------------------------------------------------------
        // on_msg_recv_callback helpers
        // -----------------------------------------------------------------------

        void ws_client_connection::handle_binary_handshake(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg)
        {
            websocket_demo::v1::connect_request req;
            auto parse_err
                = rpc::from_protobuf<websocket_demo::v1::connect_request>({arg->msg, arg->msg + arg->msg_length}, req);

            if (!parse_err.empty())
            {
                auto reason = fmt::format("invalid connect_request: {}", parse_err);
                wslay_event_queue_close(ctx,
                    WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA,
                    reinterpret_cast<const uint8_t*>(reason.data()),
                    reason.size());
                state_ = connection_state::closed;
                return;
            }

            inbound_remote_object_ = req.inbound_remote_object;
            state_ = connection_state::running;
        }

        void ws_client_connection::close_with_parse_error(
            wslay_event_context_ptr ctx, size_t msg_length, const std::string& error)
        {
            RPC_ERROR("Received message ({} bytes) parsing error: {}", msg_length, error);
            auto reason = fmt::format("invalid message format {}", error);
            wslay_event_queue_close(
                ctx, WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, reinterpret_cast<const uint8_t*>(reason.data()), reason.size());
        }

        void ws_client_connection::handle_binary_envelope(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg)
        {
            websocket_demo::v1::envelope envelope;
            auto error
                = rpc::from_protobuf<websocket_demo::v1::envelope>({arg->msg, arg->msg + arg->msg_length}, envelope);
            if (!error.empty())
            {
                close_with_parse_error(ctx, arg->msg_length, error);
                return;
            }

            if (envelope.message_type == rpc::id<websocket_demo::v1::request>::get(rpc::get_version()))
            {
                service_->get_scheduler()->spawn(transport_->stub_handle_send(std::move(envelope)));
                return;
            }

            if (envelope.message_type == rpc::id<websocket_demo::v1::response>::get(rpc::get_version()))
            {
                websocket_demo::v1::response response;
                auto resp_error = rpc::from_protobuf<websocket_demo::v1::response>(envelope.data, response);
                if (!resp_error.empty())
                    close_with_parse_error(ctx, arg->msg_length, resp_error);
                return;
            }

            close_with_parse_error(ctx, arg->msg_length, "unknown message_type");
        }

        void ws_client_connection::on_msg_recv_callback(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data)
        {
            auto* self = static_cast<ws_client_connection*>(user_data);

            if (wslay_is_ctrl_frame(arg->opcode))
            {
                if (arg->opcode == WSLAY_CONNECTION_CLOSE)
                {
                    RPC_INFO("Connection close received, status code: {}", arg->status_code);
                }
                return;
            }

            if (arg->opcode == WSLAY_TEXT_FRAME)
            {
                RPC_INFO("Received text message ({} bytes): {}",
                    arg->msg_length,
                    std::string(reinterpret_cast<const char*>(arg->msg), arg->msg_length));

                // Echo back
                wslay_event_msg msg;
                msg.opcode = arg->opcode;
                msg.msg = arg->msg;
                msg.msg_length = arg->msg_length;
                wslay_event_queue_msg(ctx, &msg);
                return;
            }

            // Binary frame
            RPC_DEBUG("Received binary message ({} bytes)", arg->msg_length);
            if (self->state_ == connection_state::awaiting_handshake)
                self->handle_binary_handshake(ctx, arg);
            else
                self->handle_binary_envelope(ctx, arg);
        }
    }
}
