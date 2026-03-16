// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket stream implementation
#include <streaming/websocket/stream.h>

#include <cstring>
#include <stdexcept>

#include <rpc/rpc.h>

namespace streaming::websocket
{
    stream::stream(std::shared_ptr<::streaming::stream> underlying)
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

    stream::~stream()
    {
        if (wslay_ctx_ != nullptr)
        {
            wslay_event_context_free(wslay_ctx_);
            wslay_ctx_ = nullptr;
        }
    }

    auto stream::receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
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
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        }

        // No buffered message — read raw data from the underlying stream
        auto [status, span] = co_await underlying_->receive(
            rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()), timeout);
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
                co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
            }
        }

        // Timeout or incomplete frame — no message to deliver yet
        co_return {status, {}};
    }

    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        if (closed_)
            co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
        queue_message(std::vector<uint8_t>(buffer.begin(), buffer.end()));
        if (!co_await do_send())
            co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
        co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
    }

    bool stream::is_closed() const
    {
        return closed_;
    }

    void stream::set_closed()
    {
        closed_ = true;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return underlying_->get_peer_info();
    }

    void stream::queue_message(std::vector<uint8_t> data)
    {
        std::lock_guard<std::mutex> lock(pending_outgoing_mutex_);
        pending_outgoing_.push(std::move(data));
    }

    void stream::queue_close(uint16_t code, const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        wslay_event_queue_close(wslay_ctx_, code, reinterpret_cast<const uint8_t*>(reason.data()), reason.size());
    }

    void stream::drain_pending()
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

    auto stream::do_send() -> coro::task<bool>
    {
        drain_pending();

        // Let wslay encode frames into outgoing_raw_ via send_callback.
        // Capture the staged bytes while holding the lock, then write without it.
        std::vector<uint8_t> to_write;
        {
            std::lock_guard<std::mutex> lock(wslay_mutex_);
            int r = wslay_event_send(wslay_ctx_);
            if (r != 0)
            {
                RPC_ERROR("wslay_event_send error: {}", r);
                co_return false;
            }
            to_write = std::move(outgoing_raw_);
            outgoing_raw_ = {};
        }

        if (!to_write.empty())
        {
            auto status = co_await underlying_->send(
                rpc::byte_span(reinterpret_cast<const char*>(to_write.data()), to_write.size()));
            if (!status.is_ok())
                co_return false;
        }

        co_return true;
    }

    bool stream::wants_read() const
    {
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        return wslay_event_want_read(wslay_ctx_) != 0;
    }

    bool stream::wants_write() const
    {
        std::lock_guard<std::mutex> lock(wslay_mutex_);
        return wslay_event_want_write(wslay_ctx_) != 0;
    }

    // -----------------------------------------------------------------------
    // wslay callbacks
    // -----------------------------------------------------------------------

    auto stream::send_callback(
        wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int /*flags*/, void* user_data) -> ssize_t
    {
        auto* self = static_cast<stream*>(user_data);

        if (self->closed_)
        {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }

        // Buffer the encoded frame bytes; do_send() will write them asynchronously.
        self->outgoing_raw_.insert(self->outgoing_raw_.end(), data, data + len);
        return static_cast<ssize_t>(len);
    }

    auto stream::recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int /*flags*/, void* user_data)
        -> ssize_t
    {
        auto* self = static_cast<stream*>(user_data);

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

    void stream::on_msg_recv_callback(wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data)
    {
        auto* self = static_cast<stream*>(user_data);

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

} // namespace streaming::websocket
