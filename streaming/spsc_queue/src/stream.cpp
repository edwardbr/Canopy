// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// spsc queue stream implementation
#include <streaming/spsc_queue/stream.h>

#include <algorithm>
#include <chrono>
#include <cstring>

// Include the SPSC queue template definition
#include <spsc/queue.h>

namespace streaming::spsc_queue
{
    namespace
    {
        constexpr auto poll_interval = std::chrono::milliseconds{1};

        auto remaining_timeout(std::chrono::steady_clock::time_point deadline) -> std::chrono::milliseconds
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return std::chrono::milliseconds{0};

            return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        }
    } // namespace

    stream::stream(queue_type* send_q, queue_type* recv_q, std::shared_ptr<coro::scheduler> scheduler)
        : send_queue_(send_q)
        , recv_queue_(recv_q)
        , scheduler_(std::move(scheduler))
    {
    }

    auto stream::receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
    {
        if (closed_)
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};

        // Serve any bytes left over from a previous blob first.
        if (!leftover_.empty())
        {
            size_t to_copy = std::min(leftover_.size(), buffer.size());
            std::memcpy(buffer.data(), leftover_.data(), to_copy);
            leftover_.erase(leftover_.begin(), leftover_.begin() + static_cast<ptrdiff_t>(to_copy));
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;
        bool single_attempt = timeout <= std::chrono::milliseconds{0};

        while (true)
        {
            blob blob;
            if (recv_queue_->pop(blob))
            {
                uint32_t len = 0;
                std::memcpy(&len, blob.data(), header_size);

                if (len == 0)
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, {}};

                size_t to_copy = std::min(static_cast<size_t>(len), buffer.size());
                std::memcpy(buffer.data(), blob.data() + header_size, to_copy);

                if (to_copy < static_cast<size_t>(len))
                {
                    leftover_.assign(
                        blob.data() + header_size + to_copy, blob.data() + header_size + static_cast<size_t>(len));
                }

                co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
            }

            if (single_attempt)
            {
                co_await scheduler_->schedule();
                co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
            }

            auto remaining = remaining_timeout(deadline);
            if (remaining <= std::chrono::milliseconds{0})
                co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};

            co_await scheduler_->yield_for(std::min(poll_interval, remaining));

            if (closed_)
                co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
        }
    }

    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        if (closed_)
            co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};

        while (!buffer.empty())
        {
            size_t to_send = std::min(buffer.size(), max_payload);

            blob blob{};
            auto len = static_cast<uint32_t>(to_send);
            std::memcpy(blob.data(), &len, header_size);
            std::memcpy(blob.data() + header_size, buffer.data(), to_send);

            if (!send_queue_->push(blob))
            {
                // Queue full — yield and retry
                co_await scheduler_->schedule();
                continue;
            }

            buffer = buffer.subspan(to_send);
        }

        co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
    }

    bool stream::is_closed() const
    {
        return closed_;
    }

    auto stream::set_closed() -> coro::task<void>
    {
        closed_ = true;
        co_return;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return {};
    }

} // namespace streaming::spsc_queue
