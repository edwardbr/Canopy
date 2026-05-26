/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <streaming/stream.h>

#ifdef CANOPY_BUILD_WEBSOCKET

#  include <rpc/rpc.h>

#  include <wslay/wslay.h>

#  include <algorithm>
#  include <cstdlib>
#  include <cstring>
#  include <mutex>
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
            std::lock_guard<std::mutex> lock(mtx_);
            if (context_)
            {
                wslay_event_context_free(context_);
                context_ = nullptr;
            }
        }

        websocket_client_stream(const websocket_client_stream&) = delete;
        auto operator=(const websocket_client_stream&) -> websocket_client_stream& = delete;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) -> CORO_TASK(streaming::receive_result) override
        {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (!decoded_.empty())
                    CO_RETURN serve_decoded_locked(buffer);
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            const bool single_attempt = timeout <= std::chrono::milliseconds{0};

            while (true)
            {
                if (!CO_AWAIT flush_pending_frames())
                    CO_RETURN streaming::receive_result{make_status(rpc::io_status::kind::closed), {}};

                auto [status, span] = CO_AWAIT underlying_->receive(
                    rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()),
                    single_attempt ? std::chrono::milliseconds{0} : remaining_timeout(deadline));
                if (status.is_closed())
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    closed_ = true;
                    CO_RETURN streaming::receive_result{status, {}};
                }
                if (status.is_ok() && !span.empty())
                {
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        if (closed_ || !context_)
                            CO_RETURN streaming::receive_result{make_status(rpc::io_status::kind::closed), {}};
                        raw_recv_pos_ = 0;
                        raw_recv_size_ = span.size();
                        wslay_event_recv(context_);
                    }
                    if (!CO_AWAIT flush_pending_frames())
                        CO_RETURN streaming::receive_result{make_status(rpc::io_status::kind::closed), {}};
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        if (!decoded_.empty())
                            CO_RETURN serve_decoded_locked(buffer);
                    }
                }
                else if (status.is_timeout() || span.empty())
                {
                    if (single_attempt || std::chrono::steady_clock::now() >= deadline)
                        CO_RETURN streaming::receive_result{status, {}};
                }
                else
                {
                    CO_RETURN streaming::receive_result{status, {}};
                }

                if (single_attempt)
                    CO_RETURN streaming::receive_result{make_status(rpc::io_status::kind::timeout), {}};
            }
        }

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override
        {
#  ifdef CANOPY_BUILD_COROUTINE
            auto send_lock = CO_AWAIT send_mtx_.scoped_lock();
#  else
            std::unique_lock<std::mutex> send_lock(send_mtx_);
#  endif
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (closed_ || !context_)
                    CO_RETURN make_status(rpc::io_status::kind::closed);

                wslay_event_msg message{};
                message.opcode = WSLAY_BINARY_FRAME;
                message.msg = reinterpret_cast<const uint8_t*>(buffer.data());
                message.msg_length = buffer.size();
                if (wslay_event_queue_msg(context_, &message) != 0)
                    CO_RETURN make_status(rpc::io_status::kind::closed);
            }
            if (!CO_AWAIT flush_pending_frames_locked())
                CO_RETURN make_status(rpc::io_status::kind::closed);
            CO_RETURN make_status(rpc::io_status::kind::ok);
        }

        bool is_closed() const override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return closed_;
        }

        auto set_closed() -> CORO_TASK(void) override
        {
            std::shared_ptr<streaming::stream> underlying;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                closed_ = true;
                underlying = underlying_;
            }
            if (underlying)
                CO_AWAIT underlying->set_closed();
            CO_RETURN;
        }

        streaming::peer_info get_peer_info() const override { return underlying_->get_peer_info(); }

    private:
        static constexpr size_t websocket_io_chunk_size = 8192;

        static rpc::io_status make_status(rpc::io_status::kind kind)
        {
            rpc::io_status status;
            status.type = kind;
            return status;
        }

        static std::chrono::milliseconds remaining_timeout(std::chrono::steady_clock::time_point deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return std::chrono::milliseconds{0};
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            return remaining.count() > 0 ? remaining : std::chrono::milliseconds{1};
        }

        streaming::receive_result serve_decoded(rpc::mutable_byte_span buffer)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return serve_decoded_locked(buffer);
        }

        streaming::receive_result serve_decoded_locked(rpc::mutable_byte_span buffer)
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
            return {make_status(rpc::io_status::kind::ok), buffer.subspan(0, count)};
        }

        auto flush_pending_frames() -> CORO_TASK(bool)
        {
#  ifdef CANOPY_BUILD_COROUTINE
            auto send_lock = CO_AWAIT send_mtx_.scoped_lock();
#  else
            std::unique_lock<std::mutex> send_lock(send_mtx_);
#  endif
            CO_RETURN CO_AWAIT flush_pending_frames_locked();
        }

        auto flush_pending_frames_locked() -> CORO_TASK(bool)
        {
            while (true)
            {
                {
                    std::vector<uint8_t> outgoing_raw;
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        if (closed_ || !context_)
                            CO_RETURN false;
                        if (!wslay_event_want_write(context_))
                            CO_RETURN true;
                        outgoing_raw_.clear();
                        if (wslay_event_send(context_) != 0)
                        {
                            closed_ = true;
                            CO_RETURN false;
                        }
                        outgoing_raw.swap(outgoing_raw_);
                    }

                    if (!CO_AWAIT flush_outgoing_raw(std::move(outgoing_raw)))
                        CO_RETURN false;
                }
            }
        }

        auto flush_outgoing_raw(std::vector<uint8_t> raw) -> CORO_TASK(bool)
        {
            size_t offset = 0;
            while (offset < raw.size())
            {
                const size_t chunk_size = std::min(websocket_io_chunk_size, raw.size() - offset);
                auto status = CO_AWAIT underlying_->send(
                    rpc::byte_span(reinterpret_cast<const char*>(raw.data() + offset), chunk_size));
                if (!status.is_ok())
                    CO_RETURN false;
                offset += chunk_size;
            }
            CO_RETURN true;
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

        mutable std::mutex mtx_;
#  ifdef CANOPY_BUILD_COROUTINE
        rpc::coro::mutex send_mtx_;
#  else
        std::mutex send_mtx_;
#  endif
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
