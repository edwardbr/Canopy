// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket stream implementation
#include <streaming/websocket/stream.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <rpc/rpc.h>
#include <wslay/wslay.h>

#if defined(FOR_SGX)
#  include <sgx_error.h>
#  include <sgx_trts.h>
#else
#  include <random>
#endif

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

        auto timeout_until(
            std::chrono::steady_clock::time_point target,
            std::chrono::steady_clock::time_point now) -> std::chrono::milliseconds
        {
            if (now >= target)
                return std::chrono::milliseconds{0};

            auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(target - now);
            return timeout.count() > 0 ? timeout : std::chrono::milliseconds{1};
        }

        auto make_options(stream_role role) -> stream_options
        {
            stream_options options;
            options.role = role;
            return options;
        }

        auto make_ping_payload(uint64_t value) -> std::vector<uint8_t>
        {
            std::vector<uint8_t> payload(sizeof(value));
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
            return payload;
        }

        auto payload_matches(
            const std::vector<uint8_t>& expected,
            const uint8_t* actual,
            size_t actual_size) -> bool
        {
            return expected.size() == actual_size && std::equal(expected.begin(), expected.end(), actual);
        }
    } // namespace

    stream::stream(std::shared_ptr<::streaming::stream> underlying)
        : stream(
              std::move(underlying),
              stream_role::server)
    {
    }

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        stream_role role)
        : stream(
              std::move(underlying),
              make_options(role))
    {
    }

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        stream_options options)
        : underlying_(std::move(underlying))
        , wslay_ctx_(nullptr)
        , options_(options)
        , raw_recv_buffer_(
              io_chunk_size,
              '\0')
    {
        if (options_.keep_alive.enabled && options_.keep_alive.interval > std::chrono::milliseconds{0})
            next_ping_time_ = std::chrono::steady_clock::now() + options_.keep_alive.interval;

        wslay_event_callbacks callbacks;
        std::memset(&callbacks, 0, sizeof(callbacks));
        callbacks.recv_callback = recv_callback;
        callbacks.send_callback = send_callback;
        callbacks.genmask_callback = genmask_callback;
        callbacks.on_msg_recv_callback = on_msg_recv_callback;

        int result = options_.role == stream_role::client
                         ? wslay_event_context_client_init(&wslay_ctx_, &callbacks, this)
                         : wslay_event_context_server_init(&wslay_ctx_, &callbacks, this);
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
            std::chrono::milliseconds receive_timeout;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (closed_ || !wslay_ctx_)
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                auto now = std::chrono::steady_clock::now();
                if (!maybe_queue_keep_alive_locked(now))
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                receive_timeout = next_receive_timeout_locked(deadline, single_attempt, now);
            }

            if (!CO_AWAIT drive_send())
                CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};

            auto [status, span] = CO_AWAIT underlying_->receive(
                rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()), receive_timeout);
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
                    int receive_result = wslay_event_recv(wslay_ctx_);
                    if (receive_result != 0)
                    {
                        RPC_ERROR("wslay_event_recv error: {}", receive_result);
                        closed_ = true;
                        CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                    }
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

    auto stream::maybe_queue_keep_alive_locked(std::chrono::steady_clock::time_point now) -> bool
    {
        if (!options_.keep_alive.enabled || options_.keep_alive.interval <= std::chrono::milliseconds{0})
            return true;

        if (ping_outstanding_ && options_.keep_alive.timeout > std::chrono::milliseconds{0} && now >= ping_deadline_)
        {
            RPC_WARNING("WebSocket keep-alive pong timeout");
            closed_ = true;
            return false;
        }

        if (ping_outstanding_ || now < next_ping_time_)
            return true;

        pending_ping_payload_ = make_ping_payload(next_ping_id_++);
        wslay_event_msg msg{};
        msg.opcode = WSLAY_PING;
        msg.msg = pending_ping_payload_.data();
        msg.msg_length = pending_ping_payload_.size();
        if (wslay_event_queue_msg(wslay_ctx_, &msg) != 0)
        {
            closed_ = true;
            return false;
        }

        if (options_.keep_alive.timeout > std::chrono::milliseconds{0})
        {
            ping_outstanding_ = true;
            ping_deadline_ = now + options_.keep_alive.timeout;
        }
        else
        {
            pending_ping_payload_.clear();
        }
        next_ping_time_ = now + options_.keep_alive.interval;
        return true;
    }

    auto stream::next_receive_timeout_locked(
        std::chrono::steady_clock::time_point deadline,
        bool single_attempt,
        std::chrono::steady_clock::time_point now) const -> std::chrono::milliseconds
    {
        if (single_attempt)
            return std::chrono::milliseconds{0};

        auto timeout = remaining_timeout(deadline);
        if (!options_.keep_alive.enabled || options_.keep_alive.interval <= std::chrono::milliseconds{0})
            return timeout;

        if (ping_outstanding_ && options_.keep_alive.timeout > std::chrono::milliseconds{0})
            return std::min(timeout, timeout_until(ping_deadline_, now));

        return std::min(timeout, timeout_until(next_ping_time_, now));
    }

    void stream::handle_keep_alive_locked(const wslay_event_on_msg_recv_arg* arg)
    {
        if (arg->opcode != WSLAY_PONG || !ping_outstanding_)
            return;

        if (payload_matches(pending_ping_payload_, arg->msg, arg->msg_length))
        {
            ping_outstanding_ = false;
            pending_ping_payload_.clear();
        }
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

    auto stream::genmask_callback(
        wslay_event_context* ctx,
        uint8_t* buf,
        size_t len,
        void*) -> int
    {
#if defined(FOR_SGX)
        if (sgx_read_rand(buf, len) == SGX_SUCCESS)
            return 0;

        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
#else
        try
        {
            static thread_local std::random_device random_device;
            for (size_t i = 0; i < len; ++i)
                buf[i] = static_cast<uint8_t>(random_device());
            return 0;
        }
        catch (...)
        {
            static thread_local uint64_t fallback_state = 0x9e3779b97f4a7c15ull;
            fallback_state ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            for (size_t i = 0; i < len; ++i)
            {
                fallback_state ^= fallback_state << 13;
                fallback_state ^= fallback_state >> 7;
                fallback_state ^= fallback_state << 17;
                buf[i] = static_cast<uint8_t>(fallback_state & 0xff);
            }
            return 0;
        }
#endif
    }

    void stream::on_msg_recv_callback(
        wslay_event_context* ctx,
        const wslay_event_on_msg_recv_arg* arg,
        void* user_data)
    {
        auto* self = static_cast<stream*>(user_data);

        if (wslay_is_ctrl_frame(arg->opcode))
        {
            self->handle_keep_alive_locked(arg);
            if (arg->opcode == WSLAY_CONNECTION_CLOSE)
            {
                RPC_INFO("Connection close received, status code: {}", arg->status_code);
            }
            return;
        }

        if (arg->opcode == WSLAY_BINARY_FRAME || arg->opcode == WSLAY_TEXT_FRAME)
        {
            RPC_DEBUG("WebSocket message received ({} bytes)", arg->msg_length);
            self->decoded_messages_.emplace(arg->msg, arg->msg + arg->msg_length);
            self->current_msg_offset_ = 0;
        }
    }

} // namespace streaming::websocket
