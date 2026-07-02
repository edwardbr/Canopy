// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket stream implementation
#include <streaming/websocket/stream.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

#include <rpc/rpc.h>
#include <wslay/wslay.h>

#if defined(CANOPY_BUILD_ZLIB)
#  include <streaming/detail/zlib_allocator.h>
#  include <zlib.h>
#endif

#include <random>

namespace streaming::websocket
{
    namespace
    {
        constexpr uint8_t bits_per_byte = 8;
        constexpr uint8_t byte_mask = 0xff;

        constexpr uint8_t websocket_fin_bit = 0x80;
        constexpr uint8_t websocket_rsv1_wire_bit = 0x40;
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

        constexpr std::array<uint8_t, 4> permessage_deflate_tail{{0x00, 0x00, 0xff, 0xff}};

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

        bool settings_enable_permessage_deflate(const ::rpc::websocket_stream::stream_settings& settings)
        {
#if defined(CANOPY_BUILD_ZLIB)
            return settings.permessage_deflate.enabled;
#else
            (void)settings;
            return false;
#endif
        }

#if defined(CANOPY_BUILD_ZLIB)
        constexpr size_t zlib_io_chunk_size = 8192;

        struct zlib_deflater_guard
        {
            z_stream* stream{nullptr};

            ~zlib_deflater_guard()
            {
                if (stream != nullptr)
                    deflateEnd(stream);
            }
        };

        struct zlib_inflater_guard
        {
            z_stream* stream{nullptr};

            ~zlib_inflater_guard()
            {
                if (stream != nullptr)
                    inflateEnd(stream);
            }
        };

        bool append_with_limit(
            std::vector<uint8_t>& output,
            const uint8_t* data,
            size_t size,
            uint64_t limit)
        {
            if (limit != 0 && (size > limit || output.size() > limit - size))
                return false;

            output.insert(output.end(), data, data + size);
            return true;
        }

        auto permessage_deflate_compress(
            const uint8_t* input,
            size_t input_size) -> std::optional<std::vector<uint8_t>>
        {
            z_stream zstream{};
            streaming::detail::initialise_zlib_allocator(zstream);
            const auto init_result
                = deflateInit2(&zstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            if (init_result != Z_OK)
            {
                RPC_WARNING("WebSocket permessage-deflate deflateInit2 failed: {}", init_result);
                return std::nullopt;
            }
            zlib_deflater_guard guard{&zstream};

            std::vector<uint8_t> output;
            std::array<uint8_t, zlib_io_chunk_size> input_chunk{};
            size_t input_offset = 0;
            bool finished = false;

            while (!finished)
            {
                if (zstream.avail_in == 0 && input_offset < input_size)
                {
                    const auto chunk = std::min(input_size - input_offset, input_chunk.size());
                    std::copy_n(input + input_offset, chunk, input_chunk.data());
                    zstream.next_in = input_chunk.data();
                    zstream.avail_in = static_cast<uInt>(chunk);
                    input_offset += chunk;
                }

                const int flush = input_offset >= input_size && zstream.avail_in == 0 ? Z_SYNC_FLUSH : Z_NO_FLUSH;
                do
                {
                    std::array<uint8_t, zlib_io_chunk_size> chunk{};
                    zstream.next_out = chunk.data();
                    zstream.avail_out = static_cast<uInt>(chunk.size());

                    const int result = deflate(&zstream, flush);
                    if (result != Z_OK)
                    {
                        RPC_WARNING(
                            "WebSocket permessage-deflate deflate failed: result={} msg={} avail_in={} avail_out={} total_in={} total_out={}",
                            result,
                            zstream.msg ? zstream.msg : "",
                            zstream.avail_in,
                            zstream.avail_out,
                            zstream.total_in,
                            zstream.total_out);
                        return std::nullopt;
                    }

                    const auto produced = chunk.size() - zstream.avail_out;
                    output.insert(output.end(), chunk.data(), chunk.data() + produced);
                } while (zstream.avail_out == 0);

                finished = flush == Z_SYNC_FLUSH && zstream.avail_in == 0;
            }

            if (output.size() < permessage_deflate_tail.size()
                || !std::equal(
                    permessage_deflate_tail.begin(),
                    permessage_deflate_tail.end(),
                    output.end() - permessage_deflate_tail.size()))
            {
                RPC_WARNING("WebSocket permessage-deflate deflate output missing sync-flush tail");
                return std::nullopt;
            }

            output.resize(output.size() - permessage_deflate_tail.size());
            return output;
        }

        auto permessage_deflate_decompress(
            const uint8_t* input,
            size_t input_size,
            uint64_t max_output_bytes) -> std::optional<std::vector<uint8_t>>
        {
            std::vector<uint8_t> compressed;
            compressed.reserve(input_size + permessage_deflate_tail.size());
            compressed.insert(compressed.end(), input, input + input_size);
            compressed.insert(compressed.end(), permessage_deflate_tail.begin(), permessage_deflate_tail.end());

            z_stream zstream{};
            streaming::detail::initialise_zlib_allocator(zstream);
            const auto init_result = inflateInit2(&zstream, -MAX_WBITS);
            if (init_result != Z_OK)
            {
                RPC_WARNING("WebSocket permessage-deflate inflateInit2 failed: {}", init_result);
                return std::nullopt;
            }
            zlib_inflater_guard guard{&zstream};

            std::vector<uint8_t> output;
            size_t input_offset = 0;
            bool finished = false;

            while (!finished)
            {
                if (zstream.avail_in == 0 && input_offset < compressed.size())
                {
                    const auto chunk
                        = std::min<size_t>(compressed.size() - input_offset, std::numeric_limits<uInt>::max());
                    zstream.next_in = compressed.data() + input_offset;
                    zstream.avail_in = static_cast<uInt>(chunk);
                    input_offset += chunk;
                }

                if (zstream.avail_in == 0)
                    break;

                do
                {
                    std::array<uint8_t, zlib_io_chunk_size> chunk{};
                    const auto total_in_before = zstream.total_in;
                    const auto total_out_before = zstream.total_out;
                    zstream.next_out = chunk.data();
                    zstream.avail_out = static_cast<uInt>(chunk.size());

                    const int result = inflate(&zstream, Z_SYNC_FLUSH);
                    if (result != Z_OK && result != Z_STREAM_END)
                    {
                        RPC_WARNING(
                            "WebSocket permessage-deflate inflate failed: result={} msg={} avail_in={} avail_out={} total_in={} total_out={}",
                            result,
                            zstream.msg ? zstream.msg : "",
                            zstream.avail_in,
                            zstream.avail_out,
                            zstream.total_in,
                            zstream.total_out);
                        return std::nullopt;
                    }

                    const auto produced = chunk.size() - zstream.avail_out;
                    if (!append_with_limit(output, chunk.data(), produced, max_output_bytes))
                    {
                        RPC_WARNING("WebSocket permessage-deflate inflate output exceeded limit {}", max_output_bytes);
                        return std::nullopt;
                    }

                    if (result == Z_STREAM_END)
                    {
                        finished = true;
                        break;
                    }

                    if (zstream.total_in == total_in_before && zstream.total_out == total_out_before)
                    {
                        RPC_WARNING("WebSocket permessage-deflate inflate made no progress");
                        return std::nullopt;
                    }
                } while (zstream.avail_out == 0);

                if (input_offset >= compressed.size() && zstream.avail_in == 0)
                    finished = true;
            }

            return output;
        }
#endif
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
        , settings_(settings)
        , role_(concrete_role(
              settings_,
              default_role))
        , raw_recv_buffer_(
              io_chunk_size,
              '\0')
    {
        if (settings_.keep_alive.enabled && keep_alive_interval(settings_) > std::chrono::milliseconds{0})
            next_ping_time_ = std::chrono::steady_clock::now() + keep_alive_interval(settings_);

        wslay_event_callbacks callbacks{};
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

        if (permessage_deflate_enabled())
            wslay_event_config_set_allowed_rsv_bits(wslay_ctx_, WSLAY_RSV1_BIT);
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

            if (!queue_binary_message_locked(buffer))
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
                const bool rsv1 = (first & websocket_rsv1_wire_bit) != 0;
                const uint8_t unsupported_rsv = first & (websocket_rsv_bits & ~websocket_rsv1_wire_bit);
                validator_fin_ = (first & websocket_fin_bit) != 0;
                validator_opcode_ = first & websocket_opcode_mask;
                validator_rsv1_ = rsv1;

                if (unsupported_rsv != 0 || !is_valid_opcode(validator_opcode_))
                    return false;
                if (validator_rsv1_
                    && (!permessage_deflate_enabled()
                        || (validator_opcode_ != websocket_text_opcode && validator_opcode_ != websocket_binary_opcode)))
                {
                    return false;
                }
                if (is_control_opcode(validator_opcode_) && !validator_fin_)
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
                    validator_frame_payload_length_ = validator_payload_remaining_;
                    if (!validate_current_frame_metadata_locked())
                        return false;
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
                validator_frame_payload_length_ = validator_payload_remaining_;
                if (!validate_current_frame_metadata_locked())
                    return false;
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

    bool stream::validate_current_frame_metadata_locked()
    {
        if (settings_.max_frame_payload_bytes != 0 && validator_frame_payload_length_ > settings_.max_frame_payload_bytes)
        {
            RPC_WARNING(
                "WebSocket frame too large: {} bytes limit={}",
                validator_frame_payload_length_,
                settings_.max_frame_payload_bytes);
            return false;
        }

        if (is_control_opcode(validator_opcode_))
            return true;

        if (validator_opcode_ == websocket_continuation_opcode)
        {
            if (!validator_fragmented_message_active_)
            {
                RPC_WARNING("WebSocket continuation frame received without an active fragmented message");
                return false;
            }

            if (settings_.max_message_bytes != 0)
            {
                if (validator_fragmented_message_bytes_ > settings_.max_message_bytes
                    || validator_frame_payload_length_ > settings_.max_message_bytes - validator_fragmented_message_bytes_)
                {
                    RPC_WARNING(
                        "WebSocket fragmented message too large: current={} frame={} limit={}",
                        validator_fragmented_message_bytes_,
                        validator_frame_payload_length_,
                        settings_.max_message_bytes);
                    return false;
                }
                validator_fragmented_message_bytes_ += validator_frame_payload_length_;
            }

            if (validator_fin_)
            {
                validator_fragmented_message_active_ = false;
                validator_fragmented_message_bytes_ = 0;
            }
            return true;
        }

        if (validator_fragmented_message_active_)
        {
            RPC_WARNING("WebSocket data frame received before completing fragmented message");
            return false;
        }

        if (settings_.max_message_bytes != 0 && validator_frame_payload_length_ > settings_.max_message_bytes)
        {
            RPC_WARNING(
                "WebSocket message too large: {} bytes limit={}",
                validator_frame_payload_length_,
                settings_.max_message_bytes);
            return false;
        }

        if (!validator_fin_)
        {
            validator_fragmented_message_active_ = true;
            validator_fragmented_message_bytes_
                = settings_.max_message_bytes != 0 ? validator_frame_payload_length_ : uint64_t{0};
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

    bool stream::permessage_deflate_enabled() const
    {
        return settings_enable_permessage_deflate(settings_);
    }

    bool stream::queue_binary_message_locked(rpc::byte_span buffer)
    {
#if defined(CANOPY_BUILD_ZLIB)
        if (permessage_deflate_enabled() && !buffer.empty())
        {
            auto compressed = permessage_deflate_compress(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size());
            if (compressed)
            {
                wslay_event_msg compressed_msg{};
                compressed_msg.opcode = WSLAY_BINARY_FRAME;
                compressed_msg.msg = compressed->data();
                compressed_msg.msg_length = compressed->size();
                return wslay_event_queue_msg_ex(wslay_ctx_, &compressed_msg, WSLAY_RSV1_BIT) == 0;
            }

            RPC_WARNING("WebSocket permessage-deflate compression failed; sending uncompressed message");
        }
#endif

        // wslay copies the payload internally so we can pass the span directly.
        wslay_event_msg msg{};
        msg.opcode = WSLAY_BINARY_FRAME;
        msg.msg = reinterpret_cast<const uint8_t*>(buffer.data());
        msg.msg_length = buffer.size();
        return wslay_event_queue_msg(wslay_ctx_, &msg) == 0;
    }

    bool stream::queue_received_message_locked(std::vector<uint8_t> message)
    {
        if (settings_.max_message_bytes != 0 && message.size() > settings_.max_message_bytes)
        {
            RPC_WARNING("WebSocket message too large: {} bytes limit={}", message.size(), settings_.max_message_bytes);
            return false;
        }
        if (settings_.max_decoded_messages != 0 && decoded_messages_.size() >= settings_.max_decoded_messages)
        {
            RPC_WARNING(
                "WebSocket decoded message queue full: {} messages limit={}",
                decoded_messages_.size(),
                settings_.max_decoded_messages);
            return false;
        }

        RPC_DEBUG("WebSocket message received ({} bytes)", message.size());
        decoded_messages_.push(std::move(message));
        current_msg_offset_ = 0;
        return true;
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
            if ((arg->rsv & ~WSLAY_RSV1_BIT) != 0
                || ((arg->rsv & WSLAY_RSV1_BIT) != 0 && !self->permessage_deflate_enabled()))
            {
                RPC_WARNING("WebSocket message received with unsupported RSV bits: {}", arg->rsv);
                self->closed_ = true;
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return;
            }

            std::vector<uint8_t> message;
            if ((arg->rsv & WSLAY_RSV1_BIT) != 0)
            {
#if defined(CANOPY_BUILD_ZLIB)
                auto decompressed
                    = permessage_deflate_decompress(arg->msg, arg->msg_length, self->settings_.max_message_bytes);
                if (!decompressed)
                {
                    RPC_WARNING("WebSocket permessage-deflate decompression failed");
                    self->closed_ = true;
                    wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                    return;
                }
                message = std::move(*decompressed);
#else
                self->closed_ = true;
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return;
#endif
            }
            else
            {
                message.assign(arg->msg, arg->msg + arg->msg_length);
            }

            if (!self->queue_received_message_locked(std::move(message)))
            {
                self->closed_ = true;
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
                return;
            }
        }
    }

} // namespace streaming::websocket
