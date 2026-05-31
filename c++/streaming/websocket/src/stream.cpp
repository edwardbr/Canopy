// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket stream implementation
#include <streaming/websocket/stream.h>

#include <algorithm>
#include <cstring>
#include <limits>
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
        constexpr uint8_t bits_per_byte = 8;
        constexpr uint8_t byte_mask = 0xff;

        constexpr uint8_t websocket_fin_bit = 0x80;
        constexpr uint8_t websocket_rsv_bits = 0x70;
        constexpr uint8_t websocket_opcode_mask = 0x0f;
        constexpr uint8_t websocket_mask_bit = 0x80;
        constexpr uint8_t websocket_payload_length_mask = 0x7f;

        constexpr uint8_t websocket_continuation_opcode = 0x0;
        constexpr uint8_t websocket_text_opcode = 0x1;
        constexpr uint8_t websocket_binary_opcode = 0x2;
        constexpr uint8_t websocket_first_control_opcode = 0x8;
        constexpr uint8_t websocket_last_control_opcode = 0xa;

        constexpr uint8_t websocket_max_inline_payload_length = 125;
        constexpr uint8_t websocket_16_bit_payload_length_marker = 126;
        constexpr uint8_t websocket_64_bit_payload_length_marker = 127;
        constexpr uint64_t websocket_min_64_bit_payload_length = 65536;
        constexpr uint64_t websocket_64_bit_payload_sign_bit = uint64_t{1} << 63;

        constexpr uint8_t websocket_mask_key_bytes = 4;
        constexpr uint8_t websocket_16_bit_extended_length_bytes = 2;
        constexpr uint8_t websocket_64_bit_extended_length_bytes = 8;

        constexpr uint8_t validator_state_first_header_byte = 0;
        constexpr uint8_t validator_state_second_header_byte = 1;
        constexpr uint8_t validator_state_extended_length = 2;
        constexpr uint8_t validator_state_mask_key = 3;
        constexpr uint8_t validator_state_payload = 4;

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

        auto to_milliseconds(uint64_t value) -> std::chrono::milliseconds
        {
            const auto max_value = static_cast<uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
            return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(std::min(value, max_value))};
        }

        auto keep_alive_interval(const ::rpc::websocket_stream::stream_settings& settings) -> std::chrono::milliseconds
        {
            return to_milliseconds(settings.keep_alive.interval_ms);
        }

        auto keep_alive_timeout(const ::rpc::websocket_stream::stream_settings& settings) -> std::chrono::milliseconds
        {
            return to_milliseconds(settings.keep_alive.timeout_ms);
        }

        auto make_settings(::rpc::websocket_stream::endpoint_role role) -> ::rpc::websocket_stream::stream_settings
        {
            ::rpc::websocket_stream::stream_settings settings;
            settings.role = role;
            return settings;
        }

        auto concrete_role(
            const ::rpc::websocket_stream::stream_settings& settings,
            ::rpc::websocket_stream::endpoint_role default_role) -> ::rpc::websocket_stream::endpoint_role
        {
            if (!settings.role)
                return default_role;

            return settings.role.value();
        }

        auto make_ping_payload(uint64_t value) -> std::vector<uint8_t>
        {
            std::vector<uint8_t> payload(sizeof(value));
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<uint8_t>((value >> (i * bits_per_byte)) & byte_mask);
            return payload;
        }

        auto payload_matches(
            const std::vector<uint8_t>& expected,
            const uint8_t* actual,
            size_t actual_size) -> bool
        {
            return expected.size() == actual_size && std::equal(expected.begin(), expected.end(), actual);
        }

        bool is_valid_opcode(uint8_t opcode)
        {
            return opcode == websocket_continuation_opcode || opcode == websocket_text_opcode
                   || opcode == websocket_binary_opcode
                   || (opcode >= websocket_first_control_opcode && opcode <= websocket_last_control_opcode);
        }

        bool is_control_opcode(uint8_t opcode)
        {
            return opcode >= websocket_first_control_opcode;
        }
    } // namespace

    stream::stream(std::shared_ptr<::streaming::stream> underlying)
        : stream(
              std::move(underlying),
              ::rpc::websocket_stream::endpoint_role::server)
    {
    }

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        ::rpc::websocket_stream::endpoint_role role)
        : stream(
              std::move(underlying),
              make_settings(role),
              role)
    {
    }

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        ::rpc::websocket_stream::stream_settings settings,
        ::rpc::websocket_stream::endpoint_role default_role)
        : underlying_(std::move(underlying))
        , wslay_ctx_(nullptr)
        , settings_(std::move(settings))
        , role_(concrete_role(
              settings_,
              default_role))
        , raw_recv_buffer_(
              io_chunk_size,
              '\0')
    {
        if (settings_.keep_alive.enabled && keep_alive_interval(settings_) > std::chrono::milliseconds{0})
            next_ping_time_ = std::chrono::steady_clock::now() + keep_alive_interval(settings_);

        wslay_event_callbacks callbacks;
        std::memset(&callbacks, 0, sizeof(callbacks));
        callbacks.recv_callback = recv_callback;
        callbacks.send_callback = send_callback;
        callbacks.genmask_callback = genmask_callback;
        callbacks.on_msg_recv_callback = on_msg_recv_callback;

        int result = role_ == ::rpc::websocket_stream::endpoint_role::client
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
                    if (!validate_incoming_wire_locked(
                            rpc::byte_span{reinterpret_cast<const char*>(span.data()), span.size()}))
                    {
                        closed_ = true;
                        CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                    }
                    raw_recv_pos_ = 0;
                    raw_recv_size_ = span.size();
                    int receive_result = wslay_event_recv(wslay_ctx_);
                    if (receive_result != 0)
                    {
                        RPC_ERROR("wslay_event_recv error: {}", receive_result);
                        closed_ = true;
                        CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                    }
                    if (closed_)
                        CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
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
        const auto interval = keep_alive_interval(settings_);
        const auto timeout = keep_alive_timeout(settings_);
        if (!settings_.keep_alive.enabled || interval <= std::chrono::milliseconds{0})
            return true;

        if (ping_outstanding_ && timeout > std::chrono::milliseconds{0} && now >= ping_deadline_)
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

        if (timeout > std::chrono::milliseconds{0})
        {
            ping_outstanding_ = true;
            ping_deadline_ = now + timeout;
        }
        else
        {
            pending_ping_payload_.clear();
        }
        next_ping_time_ = now + interval;
        return true;
    }

    bool stream::validate_incoming_wire_locked(rpc::byte_span data)
    {
        // wslay parses frames, but server-side masking and a few cheap RFC 6455
        // invariants are enforced here so untrusted clients are rejected before
        // their payload is handed to the WebSocket message queue.
        const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
        size_t offset = 0;

        while (offset < data.size())
        {
            switch (validator_state_)
            {
            case validator_state_first_header_byte:
            {
                const uint8_t first = bytes[offset++];
                const uint8_t rsv = first & websocket_rsv_bits;
                const bool fin = (first & websocket_fin_bit) != 0;
                validator_opcode_ = first & websocket_opcode_mask;

                if (rsv != 0 || !is_valid_opcode(validator_opcode_))
                    return false;
                if (is_control_opcode(validator_opcode_) && !fin)
                    return false;

                validator_state_ = validator_state_second_header_byte;
                break;
            }
            case validator_state_second_header_byte:
            {
                const uint8_t second = bytes[offset++];
                const bool masked = (second & websocket_mask_bit) != 0;
                validator_length_code_ = second & websocket_payload_length_mask;

                if (role_ == ::rpc::websocket_stream::endpoint_role::server && !masked)
                    return false;
                if (role_ == ::rpc::websocket_stream::endpoint_role::client && masked)
                    return false;
                if (is_control_opcode(validator_opcode_) && validator_length_code_ > websocket_max_inline_payload_length)
                    return false;

                if (validator_length_code_ <= websocket_max_inline_payload_length)
                {
                    validator_payload_remaining_ = validator_length_code_;
                    validator_mask_remaining_ = masked ? websocket_mask_key_bytes : 0;
                    validator_state_ = validator_mask_remaining_ != 0 ? validator_state_mask_key : validator_state_payload;
                }
                else
                {
                    validator_extended_length_ = 0;
                    validator_extended_remaining_ = validator_length_code_ == websocket_16_bit_payload_length_marker
                                                        ? websocket_16_bit_extended_length_bytes
                                                        : websocket_64_bit_extended_length_bytes;
                    validator_state_ = validator_state_extended_length;
                }
                break;
            }
            case validator_state_extended_length:
            {
                validator_extended_length_ = (validator_extended_length_ << bits_per_byte) | bytes[offset++];
                --validator_extended_remaining_;
                if (validator_extended_remaining_ != 0)
                    break;

                if (validator_length_code_ == websocket_16_bit_payload_length_marker
                    && validator_extended_length_ < websocket_16_bit_payload_length_marker)
                    return false;
                if (validator_length_code_ == websocket_64_bit_payload_length_marker
                    && validator_extended_length_ < websocket_min_64_bit_payload_length)
                    return false;
                if (validator_length_code_ == websocket_64_bit_payload_length_marker
                    && (validator_extended_length_ & websocket_64_bit_payload_sign_bit) != 0)
                    return false;

                validator_payload_remaining_ = validator_extended_length_;
                validator_mask_remaining_
                    = role_ == ::rpc::websocket_stream::endpoint_role::server ? websocket_mask_key_bytes : 0;
                validator_state_ = validator_mask_remaining_ != 0 ? validator_state_mask_key : validator_state_payload;
                break;
            }
            case validator_state_mask_key:
            {
                const auto mask_bytes = std::min<size_t>(validator_mask_remaining_, data.size() - offset);
                offset += mask_bytes;
                validator_mask_remaining_ -= static_cast<uint8_t>(mask_bytes);
                if (validator_mask_remaining_ == 0)
                    validator_state_ = validator_state_payload;
                break;
            }
            case validator_state_payload:
            {
                const auto payload_bytes = std::min<uint64_t>(validator_payload_remaining_, data.size() - offset);
                offset += static_cast<size_t>(payload_bytes);
                validator_payload_remaining_ -= payload_bytes;
                if (validator_payload_remaining_ == 0)
                    validator_state_ = validator_state_first_header_byte;
                break;
            }
            default:
                return false;
            }
        }

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
        const auto interval = keep_alive_interval(settings_);
        const auto timeout_value = keep_alive_timeout(settings_);
        if (!settings_.keep_alive.enabled || interval <= std::chrono::milliseconds{0})
            return timeout;

        if (ping_outstanding_ && timeout_value > std::chrono::milliseconds{0})
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
            if (self->settings_.max_message_bytes != 0 && arg->msg_length > self->settings_.max_message_bytes)
            {
                RPC_WARNING(
                    "WebSocket message too large: {} bytes limit={}", arg->msg_length, self->settings_.max_message_bytes);
                self->closed_ = true;
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return;
            }
            if (self->settings_.max_decoded_messages != 0
                && self->decoded_messages_.size() >= self->settings_.max_decoded_messages)
            {
                RPC_WARNING(
                    "WebSocket decoded message queue full: {} messages limit={}",
                    self->decoded_messages_.size(),
                    self->settings_.max_decoded_messages);
                self->closed_ = true;
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return;
            }
            RPC_DEBUG("WebSocket message received ({} bytes)", arg->msg_length);
            self->decoded_messages_.emplace(arg->msg, arg->msg + arg->msg_length);
            self->current_msg_offset_ = 0;
        }
    }

} // namespace streaming::websocket
