// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// spsc wrapping stream implementation
#include <streaming/spsc_wrapping/stream.h>

#include <cstring>

namespace streaming::spsc_wrapping
{
    stream::stream(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<coro::scheduler> scheduler)
        : underlying_(std::move(underlying))
        , scheduler_(std::move(scheduler))
    {
    }

    auto stream::create(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<coro::scheduler> scheduler)
        -> std::shared_ptr<stream>
    {
        auto obj = std::shared_ptr<stream>(new stream(std::move(underlying), std::move(scheduler)));
        obj->start_proxy_loops();
        return obj;
    }

    void stream::start_proxy_loops()
    {
        scheduler_->spawn_detached(recv_proxy_loop(shared_from_this()));
        scheduler_->spawn_detached(send_proxy_loop(shared_from_this()));
    }

    // ---------------------------------------------------------------------------
    // recv_proxy_loop: fills recv_q_ by reading from the underlying stream.
    //
    // Runs concurrently with the transport's receive_consumer_loop on the same
    // scheduler.  The transport pops from recv_q_; this loop keeps it supplied.
    // ---------------------------------------------------------------------------

    auto stream::recv_proxy_loop(std::shared_ptr<stream> self) -> coro::task<void>
    {
        std::array<char, streaming::spsc_queue::max_payload> raw_buf;

        while (!self->closed_)
        {
            auto [status, data] = co_await self->underlying_->receive(
                rpc::mutable_byte_span{raw_buf.data(), raw_buf.size()}, std::chrono::milliseconds{10});

            if (status.is_closed())
            {
                self->closed_ = true;
                break;
            }
            if (status.is_timeout() || data.empty())
            {
                co_await self->scheduler_->schedule();
                continue;
            }

            // Pack bytes into an SPSC blob and push to recv_q_.
            streaming::spsc_queue::blob blob{};
            auto len = static_cast<uint32_t>(data.size());
            std::memcpy(blob.data(), &len, streaming::spsc_queue::header_size);
            std::memcpy(blob.data() + streaming::spsc_queue::header_size, data.data(), data.size());

            while (!self->recv_q_.push(blob))
            {
                if (self->closed_)
                    co_return;
                co_await self->scheduler_->schedule();
            }
        }
    }

    // ---------------------------------------------------------------------------
    // send_proxy_loop: drains send_q_ to the underlying stream.
    //
    // Runs concurrently with the transport's send_producer_loop on the same
    // scheduler.  The transport pushes to send_q_; this loop forwards the bytes.
    // ---------------------------------------------------------------------------

    auto stream::send_proxy_loop(std::shared_ptr<stream> self) -> coro::task<void>
    {
        streaming::spsc_queue::blob blob;

        while (true)
        {
            if (!self->send_q_.pop(blob))
            {
                // Exit only when the queue is empty AND the stream is closed.
                // This ensures any data queued before set_closed() is flushed first.
                if (self->closed_)
                    co_return;
                co_await self->scheduler_->schedule();
                continue;
            }

            uint32_t len = 0;
            std::memcpy(&len, blob.data(), streaming::spsc_queue::header_size);
            if (len == 0)
                continue;

            // Send the blob even if closed_ is set — the queue must be drained.
            auto status = co_await self->underlying_->send(
                rpc::byte_span{reinterpret_cast<const char*>(blob.data() + streaming::spsc_queue::header_size), len});
            if (!status.is_ok())
                co_return;
        }
    }

    // ---------------------------------------------------------------------------
    // Stream interface — operates on the SPSC queues only; never touches the
    // underlying stream directly.  Proxy loops handle the underlying I/O.
    // ---------------------------------------------------------------------------

    // Inbound: pop from recv_q_.  Yields once and returns timeout if the queue
    // is empty so the scheduler can run recv_proxy_loop to fill it.
    auto stream::receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds /*timeout*/)
        -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
    {
        if (closed_)
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};

        // Serve leftover bytes from a previously oversized blob.
        if (!leftover_.empty())
        {
            size_t to_copy = std::min(leftover_.size(), buffer.size());
            std::memcpy(buffer.data(), leftover_.data(), to_copy);
            leftover_.erase(leftover_.begin(), leftover_.begin() + static_cast<ptrdiff_t>(to_copy));
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        }

        // Try to pop a blob from the receive queue.
        streaming::spsc_queue::blob blob;
        if (!recv_q_.pop(blob))
        {
            // Queue is empty — yield so recv_proxy_loop gets a chance to fill it,
            // then report timeout so the transport retries on its next iteration.
            co_await scheduler_->schedule();
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
        }

        // Extract payload from the blob.
        uint32_t len = 0;
        std::memcpy(&len, blob.data(), streaming::spsc_queue::header_size);
        if (len == 0)
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, {}};

        size_t to_copy = std::min(static_cast<size_t>(len), buffer.size());
        std::memcpy(buffer.data(), blob.data() + streaming::spsc_queue::header_size, to_copy);

        if (to_copy < static_cast<size_t>(len))
        {
            leftover_.assign(blob.data() + streaming::spsc_queue::header_size + to_copy,
                blob.data() + streaming::spsc_queue::header_size + static_cast<size_t>(len));
        }

        co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
    }

    // Outbound: push to send_q_ and return immediately.
    // send_proxy_loop handles forwarding to the underlying stream.
    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        if (closed_)
            co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};

        while (!buffer.empty())
        {
            size_t chunk = std::min(buffer.size(), streaming::spsc_queue::max_payload);
            streaming::spsc_queue::blob blob{};
            auto len = static_cast<uint32_t>(chunk);
            std::memcpy(blob.data(), &len, streaming::spsc_queue::header_size);
            std::memcpy(blob.data() + streaming::spsc_queue::header_size, buffer.data(), chunk);
            while (!send_q_.push(blob))
            {
                if (closed_)
                    co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
                co_await scheduler_->schedule();
            }
            buffer = buffer.subspan(chunk);
        }

        co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
    }

    bool stream::is_closed() const
    {
        return closed_ || underlying_->is_closed();
    }

    void stream::set_closed()
    {
        // Set the flag so proxy loops see the shutdown signal.
        // Do NOT close underlying_ here: send_proxy_loop must drain send_q_ first.
        // The underlying stream is released naturally when the proxy loops exit
        // and this object is destroyed.
        closed_ = true;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return underlying_->get_peer_info();
    }

} // namespace streaming::spsc_wrapping
