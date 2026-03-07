// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_stream.cpp - WebSocket framing stream implementation
#include <streaming/ws_stream.h>

#include <cstring>
#include <stdexcept>

#include <rpc/rpc.h>

namespace streaming
{
    ws_stream::ws_stream(std::shared_ptr<stream> underlying)
        : underlying_(std::move(underlying))
        , raw_recv_buffer_(4096, '\0')
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

    ws_stream::~ws_stream()
    {
        if (wslay_ctx_ != nullptr)
        {
            wslay_event_context_free(wslay_ctx_);
            wslay_ctx_ = nullptr;
        }
    }

    auto ws_stream::poll(coro::poll_op op, std::chrono::milliseconds timeout) -> coro::task<coro::poll_status>
    {
        co_return co_await underlying_->poll(op, timeout);
    }

    auto ws_stream::recv(std::span<char> buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
    {
        // Return any already-decoded message first (supports partial reads too)
        if (!decoded_messages_.empty())
        {
            auto& msg = decoded_messages_.front();
            size_t available = msg.size() - current_msg_offset_;
            size_t to_copy = std::min(available, buffer.size());
            std::memcpy(buffer.data(), msg.data() + current_msg_offset_, to_copy);
            current_msg_offset_ += to_copy;
            if (current_msg_offset_ >= msg.size())
            {
                decoded_messages_.pop();
                current_msg_offset_ = 0;
            }
            co_return {coro::net::io_status{coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        }

        // No buffered message — read raw data from the underlying stream
        auto [status, span] = co_await underlying_->recv(raw_recv_buffer_, timeout);
        if (status.is_closed())
        {
            closed_ = true;
            co_return {status, {}};
        }
        if (status.is_ok() && !span.empty())
        {
            raw_recv_pos_ = 0;
            raw_recv_size_ = span.size();
            {
                std::lock_guard<std::mutex> lock(wslay_mutex_);
                wslay_event_recv(wslay_ctx_);
            }

            if (!decoded_messages_.empty())
            {
                auto& msg = decoded_messages_.front();
                size_t to_copy = std::min(msg.size(), buffer.size());
                std::memcpy(buffer.data(), msg.data(), to_copy);
                current_msg_offset_ = to_copy;
                if (current_msg_offset_ >= msg.size())
                {
                    decoded_messages_.pop();
                    current_msg_offset_ = 0;
                }
                co_return {coro::net::io_status{coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
            }
        }

        // Timeout or incomplete frame — no message to deliver yet
        co_return {status, {}};
    }

    auto ws_stream::send(std::span<const char> buffer) -> std::pair<coro::net::io_status, std::span<const char>>
    {
        if (closed_)
            return {coro::net::io_status{coro::net::io_status::kind::closed}, buffer};
        queue_message(std::vector<uint8_t>(buffer.begin(), buffer.end()));
        // The empty remaining span signals that all data has been accepted
        return {coro::net::io_status{coro::net::io_status::kind::ok}, {}};
    }

    bool ws_stream::is_closed() const
    {
        return closed_;
    }

    void ws_stream::set_closed()
    {
        closed_ = true;
    }

    peer_info ws_stream::get_peer_info() const
    {
        return underlying_->get_peer_info();
    }

    void ws_stream::queue_message(std::vector<uint8_t> data)
    {
        std::lock_guard<std::mutex> lock(pending_outgoing_mutex_);
        pending_outgoing_.push(std::move(data));
    }

    void ws_stream::queue_close(uint16_t code, const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        wslay_event_queue_close(wslay_ctx_, code, reinterpret_cast<const uint8_t*>(reason.data()), reason.size());
    }

    void ws_stream::drain_pending()
    {
        std::lock_guard<std::mutex> pending_lock(pending_outgoing_mutex_);
        if (pending_outgoing_.empty())
            return;

        std::lock_guard<std::mutex> wslay_lock(wslay_mutex_);
        while (!pending_outgoing_.empty())
        {
            auto& msg_data = pending_outgoing_.front();

            wslay_event_msg msg;
            msg.opcode = WSLAY_BINARY_FRAME;
            msg.msg = msg_data.data();
            msg.msg_length = msg_data.size();

            int result = wslay_event_queue_msg(wslay_ctx_, &msg);
            if (result != 0)
            {
                RPC_ERROR("Failed to queue WebSocket message: {}", result);
            }
            pending_outgoing_.pop();
        }
    }

    auto ws_stream::do_send() -> coro::task<bool>
    {
        co_await underlying_->poll(coro::poll_op::write);
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        int r = wslay_event_send(wslay_ctx_);
        if (r != 0)
        {
            RPC_ERROR("wslay_event_send error: {}", r);
            co_return false;
        }
        co_return true;
    }

    bool ws_stream::wants_read() const
    {
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        return wslay_event_want_read(wslay_ctx_) != 0;
    }

    bool ws_stream::wants_write() const
    {
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        return wslay_event_want_write(wslay_ctx_) != 0;
    }

    // -----------------------------------------------------------------------
    // wslay callbacks
    // -----------------------------------------------------------------------

    ssize_t ws_stream::send_callback(wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data)
    {
        auto* self = static_cast<ws_stream*>(user_data);

        if (self->closed_)
        {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }

        auto [status, remaining]
            = self->underlying_->send(std::span<const char>(reinterpret_cast<const char*>(data), len));

        if (status.is_ok())
            return static_cast<ssize_t>(len - remaining.size());

        if (status.try_again())
        {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }

        self->closed_ = true;
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }

    ssize_t ws_stream::recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data)
    {
        auto* self = static_cast<ws_stream*>(user_data);

        size_t available = self->raw_recv_size_ - self->raw_recv_pos_;
        if (available > 0)
        {
            size_t to_copy = std::min(len, available);
            std::memcpy(buf, self->raw_recv_buffer_.data() + self->raw_recv_pos_, to_copy);
            self->raw_recv_pos_ += to_copy;
            return static_cast<ssize_t>(to_copy);
        }

        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }

    void ws_stream::on_msg_recv_callback(wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data)
    {
        auto* self = static_cast<ws_stream*>(user_data);

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
            // Echo text frames back
            wslay_event_msg msg;
            msg.opcode = arg->opcode;
            msg.msg = arg->msg;
            msg.msg_length = arg->msg_length;
            wslay_event_queue_msg(ctx, &msg);
            return;
        }

        // Binary frame: enqueue the payload for consumption via recv()
        if (arg->opcode == WSLAY_BINARY_FRAME)
        {
            RPC_DEBUG("WebSocket binary message received ({} bytes)", arg->msg_length);
            self->decoded_messages_.emplace(arg->msg, arg->msg + arg->msg_length);
        }
    }

} // namespace streaming
