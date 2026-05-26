// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket stream implementation
#include <streaming/websocket/stream.h>

#include <cstring>
#include <stdexcept>

#include <rpc/rpc.h>
#include <wslay/wslay.h>

namespace streaming::websocket
{
    namespace
    {
        auto remaining_timeout(std::chrono::steady_clock::time_point deadline) -> std::chrono::milliseconds
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return std::chrono::milliseconds{0};

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            return remaining.count() > 0 ? remaining : std::chrono::milliseconds{1};
        }
    } // namespace

    stream::stream(std::shared_ptr<::streaming::stream> underlying)
        : underlying_(std::move(underlying))
        , raw_recv_buffer_(
              io_chunk_size,
              '\0')
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
        std::lock_guard<std::mutex> lock(mtx_);
        if (wslay_ctx_ != nullptr)
        {
            wslay_event_context_free(wslay_ctx_);
            wslay_ctx_ = nullptr;
        }
    }

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout) -> CORO_TASK(::streaming::receive_result)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!decoded_messages_.empty())
                CO_RETURN serve_decoded_locked(buffer);
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;
        bool single_attempt = timeout <= std::chrono::milliseconds{0};

        while (true)
        {
            if (!CO_AWAIT drive_send())
                CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};

            auto [status, span] = CO_AWAIT underlying_->receive(
                rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()),
                single_attempt ? std::chrono::milliseconds{0} : remaining_timeout(deadline));
            if (status.is_closed())
            {
                std::lock_guard<std::mutex> lock(mtx_);
                closed_ = true;
                CO_RETURN{status, {}};
            }
            if (status.is_ok() && !span.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (closed_ || !wslay_ctx_)
                        CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                    raw_recv_pos_ = 0;
                    raw_recv_size_ = span.size();
                    wslay_event_recv(wslay_ctx_);
                }

                if (!CO_AWAIT drive_send())
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};

                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (!decoded_messages_.empty())
                        CO_RETURN serve_decoded_locked(buffer);
                }
            }
            else if (status.is_timeout() || span.empty())
            {
                if (single_attempt || std::chrono::steady_clock::now() >= deadline)
                    CO_RETURN{status, {}};
            }
            else
            {
                CO_RETURN{status, {}};
            }

            if (single_attempt)
                CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::timeout}, {}};
        }
    }

    auto stream::send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto send_lock = CO_AWAIT send_mtx_.scoped_lock();
#else
        std::unique_lock<std::mutex> send_lock(send_mtx_);
#endif
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (closed_ || !wslay_ctx_)
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};

            // wslay copies the payload internally so we can pass the span directly.
            wslay_event_msg msg{};
            msg.opcode = WSLAY_BINARY_FRAME;
            msg.msg = reinterpret_cast<const uint8_t*>(buffer.data());
            msg.msg_length = buffer.size();
            if (wslay_event_queue_msg(wslay_ctx_, &msg) != 0)
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};
        }

        if (!CO_AWAIT drive_send_locked())
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};
        CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
    }

    bool stream::is_closed() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return closed_;
    }

    auto stream::set_closed() -> CORO_TASK(void)
    {
        std::shared_ptr<::streaming::stream> underlying;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            closed_ = true;
            underlying = underlying_;
        }
        if (underlying)
            CO_AWAIT underlying->set_closed();
        CO_RETURN;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return underlying_->get_peer_info();
    }

    auto stream::serve_decoded(rpc::mutable_byte_span buffer) -> std::pair<
        rpc::io_status,
        rpc::mutable_byte_span>
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return serve_decoded_locked(buffer);
    }

    auto stream::serve_decoded_locked(rpc::mutable_byte_span buffer) -> std::pair<
        rpc::io_status,
        rpc::mutable_byte_span>
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
        return {rpc::io_status{FLD(type) rpc::io_status::kind::ok}, buffer.subspan(0, to_copy)};
    }

    auto stream::drive_send() -> CORO_TASK(bool)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto send_lock = CO_AWAIT send_mtx_.scoped_lock();
#else
        std::unique_lock<std::mutex> send_lock(send_mtx_);
#endif
        CO_RETURN CO_AWAIT drive_send_locked();
    }

    auto stream::drive_send_locked() -> CORO_TASK(bool)
    {
        while (true)
        {
            {
                std::vector<uint8_t> raw;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (closed_)
                        CO_RETURN false;
                    if (!wslay_ctx_)
                    {
                        assert(false);
                        CO_RETURN false;
                    }
                    if (!wslay_event_want_write(wslay_ctx_))
                        CO_RETURN true;
                    outgoing_raw_.clear();
                    int r = wslay_event_send(wslay_ctx_);
                    if (r != 0)
                    {
                        RPC_ERROR("wslay_event_send error: {}", r);
                        CO_RETURN false;
                    }
                    raw.swap(outgoing_raw_);
                }

                if (!CO_AWAIT flush_outgoing_raw(std::move(raw)))
                    CO_RETURN false;
            }
        }
    }

    auto stream::flush_outgoing_raw(std::vector<uint8_t> raw) -> CORO_TASK(bool)
    {
        size_t offset = 0;
        while (offset < raw.size())
        {
            size_t chunk_size = std::min(io_chunk_size, raw.size() - offset);
            auto status = CO_AWAIT underlying_->send(
                rpc::byte_span(reinterpret_cast<const char*>(raw.data() + offset), chunk_size));
            if (!status.is_ok())
                CO_RETURN false;
            offset += chunk_size;
        }
        CO_RETURN true;
    }

    // -----------------------------------------------------------------------
    // wslay callbacks
    // -----------------------------------------------------------------------

    auto stream::send_callback(
        wslay_event_context* ctx,
        const uint8_t* data,
        size_t len,
        int /*flags*/,
        void* user_data) -> ssize_t
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

    auto stream::recv_callback(
        wslay_event_context* ctx,
        uint8_t* buf,
        size_t len,
        int /*flags*/,
        void* user_data) -> ssize_t
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

    void stream::on_msg_recv_callback(
        wslay_event_context* ctx,
        const wslay_event_on_msg_recv_arg* arg,
        void* user_data)
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
