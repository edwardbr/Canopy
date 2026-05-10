/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <streaming/stream.h>

#ifdef CANOPY_BUILD_WEBSOCKET

#  include <rpc/rpc.h>

#  include <coro/coro.hpp>
#  include <wslay/wslay.h>

#  include <algorithm>
#  include <cstdlib>
#  include <cstring>
#  include <queue>
#  include <string>
#  include <vector>

namespace stream_bench
{
    class websocket_client_stream : public streaming::stream
    {
    public:
        explicit websocket_client_stream(std::shared_ptr<streaming::stream> underlying)
            : underlying_(std::move(underlying))
            , raw_recv_buffer_(
                  websocket_io_chunk_size,
                  '\0')
        {
            wslay_event_callbacks callbacks;
            std::memset(&callbacks, 0, sizeof(callbacks));
            callbacks.recv_callback = recv_cb;
            callbacks.send_callback = send_cb;
            callbacks.on_msg_recv_callback = on_msg_cb;
            callbacks.genmask_callback = genmask_cb;
            if (wslay_event_context_client_init(&context_, &callbacks, this) != 0)
                RPC_ASSERT(false);
        }

        ~websocket_client_stream() override
        {
            if (context_)
                wslay_event_context_free(context_);
        }

        websocket_client_stream(const websocket_client_stream&) = delete;
        auto operator=(const websocket_client_stream&) -> websocket_client_stream& = delete;

        coro::task<std::pair<
            coro::net::io_status,
            rpc::mutable_byte_span>>
        receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) override
        {
            if (!decoded_.empty())
                co_return serve_decoded(buffer);

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            const bool single_attempt = timeout <= std::chrono::milliseconds{0};

            while (true)
            {
                if (!co_await flush_pending_frames())
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};

                auto [status, span] = co_await underlying_->receive(
                    rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()),
                    single_attempt ? std::chrono::milliseconds{0} : remaining_timeout(deadline));
                if (status.is_closed())
                {
                    closed_ = true;
                    co_return {status, {}};
                }
                if (status.is_ok() && !span.empty())
                {
                    raw_recv_pos_ = 0;
                    raw_recv_size_ = span.size();
                    wslay_event_recv(context_);
                    if (!co_await flush_pending_frames())
                        co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
                    if (!decoded_.empty())
                        co_return serve_decoded(buffer);
                }
                else if (status.is_timeout() || span.empty())
                {
                    if (single_attempt || std::chrono::steady_clock::now() >= deadline)
                        co_return {status, {}};
                }
                else
                {
                    co_return {status, {}};
                }

                if (single_attempt)
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
            }
        }

        coro::task<coro::net::io_status> send(rpc::byte_span buffer) override
        {
            if (closed_)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};

            wslay_event_msg message{};
            message.opcode = WSLAY_BINARY_FRAME;
            message.msg = reinterpret_cast<const uint8_t*>(buffer.data());
            message.msg_length = buffer.size();
            if (wslay_event_queue_msg(context_, &message) != 0)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            if (!co_await flush_pending_frames())
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        bool is_closed() const override { return closed_; }

        coro::task<void> set_closed() override
        {
            closed_ = true;
            if (underlying_)
                co_await underlying_->set_closed();
        }

        streaming::peer_info get_peer_info() const override { return underlying_->get_peer_info(); }

    private:
        static constexpr size_t websocket_io_chunk_size = 8192;

        static std::chrono::milliseconds remaining_timeout(std::chrono::steady_clock::time_point deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return std::chrono::milliseconds{0};
            return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        }

        std::pair<
            coro::net::io_status,
            rpc::mutable_byte_span>
        serve_decoded(rpc::mutable_byte_span buffer)
        {
            auto& message = decoded_.front();
            const size_t available = message.size() - message_offset_;
            const size_t count = std::min(available, buffer.size());
            std::memcpy(buffer.data(), message.data() + message_offset_, count);
            message_offset_ += count;
            if (message_offset_ >= message.size())
            {
                decoded_.pop();
                message_offset_ = 0;
            }
            return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, count)};
        }

        coro::task<bool> flush_pending_frames()
        {
            while (wslay_event_want_write(context_))
            {
                outgoing_raw_.clear();
                wslay_event_send(context_);
                size_t offset = 0;
                while (offset < outgoing_raw_.size())
                {
                    const size_t chunk_size = std::min(websocket_io_chunk_size, outgoing_raw_.size() - offset);
                    auto status = co_await underlying_->send(
                        rpc::byte_span(reinterpret_cast<const char*>(outgoing_raw_.data() + offset), chunk_size));
                    if (!status.is_ok())
                        co_return false;
                    offset += chunk_size;
                }
                outgoing_raw_.clear();
            }
            co_return true;
        }

        static int genmask_cb(
            wslay_event_context_ptr,
            uint8_t* buffer,
            size_t length,
            void*)
        {
            for (size_t i = 0; i < length; ++i)
                buffer[i] = static_cast<uint8_t>(std::rand() & 0xff);
            return 0;
        }

        static ssize_t send_cb(
            wslay_event_context_ptr,
            const uint8_t* data,
            size_t length,
            int,
            void* user_data)
        {
            auto* self = static_cast<websocket_client_stream*>(user_data);
            self->outgoing_raw_.insert(self->outgoing_raw_.end(), data, data + length);
            return static_cast<ssize_t>(length);
        }

        static ssize_t recv_cb(
            wslay_event_context_ptr context,
            uint8_t* output,
            size_t length,
            int,
            void* user_data)
        {
            auto* self = static_cast<websocket_client_stream*>(user_data);
            const size_t available = self->raw_recv_size_ - self->raw_recv_pos_;
            if (available == 0)
            {
                wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
                return -1;
            }
            const size_t count = std::min(length, available);
            std::memcpy(output, self->raw_recv_buffer_.data() + self->raw_recv_pos_, count);
            self->raw_recv_pos_ += count;
            return static_cast<ssize_t>(count);
        }

        static void on_msg_cb(
            wslay_event_context_ptr,
            const wslay_event_on_msg_recv_arg* argument,
            void* user_data)
        {
            auto* self = static_cast<websocket_client_stream*>(user_data);
            if (!wslay_is_ctrl_frame(argument->opcode) && argument->opcode == WSLAY_BINARY_FRAME)
                self->decoded_.emplace(argument->msg, argument->msg + argument->msg_length);
        }

        std::shared_ptr<streaming::stream> underlying_;
        wslay_event_context_ptr context_ = nullptr;
        std::string raw_recv_buffer_;
        size_t raw_recv_size_ = 0;
        size_t raw_recv_pos_ = 0;
        std::queue<std::vector<uint8_t>> decoded_;
        size_t message_offset_ = 0;
        std::vector<uint8_t> outgoing_raw_;
        bool closed_ = false;
    };
}

#endif
