// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// spsc_queue_stream.cpp - SPSC queue stream adapter implementation
#include <streaming/spsc_queue_stream.h>

#include <cstring>

// Include the SPSC queue template definition
#include <spsc/queue.h>

namespace streaming
{
    spsc_queue_stream::spsc_queue_stream(
        spsc_raw_queue* send_q, spsc_raw_queue* recv_q, std::shared_ptr<coro::scheduler> scheduler)
        : send_queue_(send_q)
        , recv_queue_(recv_q)
        , scheduler_(std::move(scheduler))
    {
    }

    auto spsc_queue_stream::receive(std::span<char> buffer, std::chrono::milliseconds)
        -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
    {
        if (closed_)
            co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};

        // Serve any bytes left over from a previous blob first.
        if (!leftover_.empty())
        {
            size_t to_copy = std::min(leftover_.size(), buffer.size());
            std::memcpy(buffer.data(), leftover_.data(), to_copy);
            leftover_.erase(leftover_.begin(), leftover_.begin() + static_cast<ptrdiff_t>(to_copy));
            co_return {coro::net::io_status{coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        }

        // Try to pop one blob from the receive queue.
        spsc_blob blob;
        if (!recv_queue_->pop(blob))
        {
            co_return {coro::net::io_status{coro::net::io_status::kind::timeout}, {}};
        }

        // Read the length header.
        uint32_t len = 0;
        std::memcpy(&len, blob.data(), spsc_header_size);

        if (len == 0)
        {
            co_return {coro::net::io_status{coro::net::io_status::kind::ok}, {}};
        }

        // Copy payload into caller's buffer; save any excess.
        size_t to_copy = std::min(static_cast<size_t>(len), buffer.size());
        std::memcpy(buffer.data(), blob.data() + spsc_header_size, to_copy);

        if (to_copy < static_cast<size_t>(len))
        {
            leftover_.assign(
                blob.data() + spsc_header_size + to_copy, blob.data() + spsc_header_size + static_cast<size_t>(len));
        }

        co_return {coro::net::io_status{coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
    }

    auto spsc_queue_stream::send(std::span<const char> buffer) -> coro::task<coro::net::io_status>
    {
        if (closed_)
            co_return coro::net::io_status{coro::net::io_status::kind::closed};

        while (!buffer.empty())
        {
            size_t to_send = std::min(buffer.size(), spsc_max_payload);

            spsc_blob blob{};
            uint32_t len = static_cast<uint32_t>(to_send);
            std::memcpy(blob.data(), &len, spsc_header_size);
            std::memcpy(blob.data() + spsc_header_size, buffer.data(), to_send);

            if (!send_queue_->push(blob))
            {
                // Queue full — yield and retry
                co_await scheduler_->schedule();
                continue;
            }

            buffer = buffer.subspan(to_send);
        }

        co_return coro::net::io_status{coro::net::io_status::kind::ok};
    }

    bool spsc_queue_stream::is_closed() const
    {
        return closed_;
    }

    void spsc_queue_stream::set_closed()
    {
        closed_ = true;
    }

    peer_info spsc_queue_stream::get_peer_info() const
    {
        return {};
    }

} // namespace streaming
