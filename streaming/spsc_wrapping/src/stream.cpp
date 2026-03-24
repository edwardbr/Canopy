// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// spsc wrapping stream implementation
#include <streaming/spsc_wrapping/stream.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>

namespace streaming::spsc_wrapping
{
    stream::stream(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<coro::scheduler> scheduler)
        : state_(std::make_shared<proxy_state>())
    {
        state_->underlying = std::move(underlying);
        state_->scheduler = std::move(scheduler);
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
        state_->scheduler->spawn_detached(recv_proxy_loop(state_));
        state_->scheduler->spawn_detached(send_proxy_loop(state_));
    }

    void stream::request_stop() const
    {
        if (!state_)
            return;
        if (state_->close_requested.exchange(true, std::memory_order_acq_rel))
            return;

        state_->closed.store(true, std::memory_order_release);
    }

    // ---------------------------------------------------------------------------
    // recv_proxy_loop: fills recv_q_ by reading from the underlying stream.
    //
    // Runs concurrently with the transport's receive_consumer_loop on the same
    // scheduler.  The transport pops from recv_q_; this loop keeps it supplied.
    // ---------------------------------------------------------------------------

    auto stream::recv_proxy_loop(std::shared_ptr<proxy_state> state) -> coro::task<void>
    {
        std::array<char, streaming::spsc_queue::max_payload> raw_buf;

        while (!state->closed.load(std::memory_order_acquire))
        {
            auto [status, data] = co_await state->underlying->receive(
                rpc::mutable_byte_span{raw_buf.data(), raw_buf.size()}, std::chrono::milliseconds{10});

            if (status.is_closed())
            {
                state->closed.store(true, std::memory_order_release);
                break;
            }
            if (status.is_timeout() || data.empty())
            {
                co_await state->scheduler->schedule();
                continue;
            }

            // Pack bytes into an SPSC blob and push to recv_q_.
            streaming::spsc_queue::blob blob{};
            auto len = static_cast<uint32_t>(data.size());
            std::memcpy(blob.data(), &len, streaming::spsc_queue::header_size);
            std::memcpy(blob.data() + streaming::spsc_queue::header_size, data.data(), data.size());

            while (!state->recv_q_.push(blob))
            {
                if (state->closed.load(std::memory_order_acquire))
                    co_return;
                co_await state->scheduler->schedule();
            }
        }
    }

    // ---------------------------------------------------------------------------
    // send_proxy_loop: drains send_q_ to the underlying stream.
    //
    // Runs concurrently with the transport's send_producer_loop on the same
    // scheduler.  The transport pushes to send_q_; this loop forwards the bytes.
    // ---------------------------------------------------------------------------

    auto stream::send_proxy_loop(std::shared_ptr<proxy_state> state) -> coro::task<void>
    {
        streaming::spsc_queue::blob blob;

        while (true)
        {
            if (!state->send_q_.pop(blob))
            {
                // Exit only when the queue is empty AND the stream is closed.
                // This ensures any data queued before set_closed() is flushed first.
                if (state->closed.load(std::memory_order_acquire))
                    co_return;
                co_await state->scheduler->schedule();
                continue;
            }

            uint32_t len = 0;
            std::memcpy(&len, blob.data(), streaming::spsc_queue::header_size);
            if (len == 0)
            {
                state->pending_send_blobs.fetch_sub(1, std::memory_order_release);
                continue;
            }

            // Send the blob even if closed_ is set — the queue must be drained.
            auto status = co_await state->underlying->send(
                rpc::byte_span{reinterpret_cast<const char*>(blob.data() + streaming::spsc_queue::header_size), len});
            // Decrement only after the underlying send completes so that set_closed()
            // waiting on pending_send_blobs == 0 cannot race past an in-flight send.
            state->pending_send_blobs.fetch_sub(1, std::memory_order_release);
            if (!status.is_ok())
            {
                state->send_failure_type.store(static_cast<int>(status.type), std::memory_order_release);
                state->send_failure_native_code.store(status.native_code, std::memory_order_release);
                state->send_failed.store(true, std::memory_order_release);
                co_return;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Stream interface — operates on the SPSC queues only; never touches the
    // underlying stream directly.  Proxy loops handle the underlying I/O.
    // ---------------------------------------------------------------------------

    // Inbound: pop from recv_q_.  Yields once and returns timeout if the queue
    // is empty so the scheduler can run recv_proxy_loop to fill it.
    auto stream::receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
    {
        if (state_->closed.load(std::memory_order_acquire))
            co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};

        auto try_receive_once = [&]() -> std::optional<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
        {
            if (leftover_offset_ < leftover_.size())
            {
                size_t available = leftover_.size() - leftover_offset_;
                size_t to_copy = std::min(available, buffer.size());
                std::memcpy(buffer.data(), leftover_.data() + leftover_offset_, to_copy);
                leftover_offset_ += to_copy;
                if (leftover_offset_ == leftover_.size())
                {
                    leftover_.clear();
                    leftover_offset_ = 0;
                }
                return std::pair{coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
            }

            streaming::spsc_queue::blob blob;
            if (!state_->recv_q_.pop(blob))
                return std::nullopt;

            uint32_t len = 0;
            std::memcpy(&len, blob.data(), streaming::spsc_queue::header_size);
            if (len == 0)
                return std::pair{coro::net::io_status{.type = coro::net::io_status::kind::ok}, rpc::mutable_byte_span{}};

            size_t to_copy = std::min(static_cast<size_t>(len), buffer.size());
            std::memcpy(buffer.data(), blob.data() + streaming::spsc_queue::header_size, to_copy);

            if (to_copy < static_cast<size_t>(len))
            {
                leftover_.assign(blob.data() + streaming::spsc_queue::header_size + to_copy,
                    blob.data() + streaming::spsc_queue::header_size + static_cast<size_t>(len));
                leftover_offset_ = 0;
            }

            return std::pair{coro::net::io_status{.type = coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        };

        if (auto result = try_receive_once())
            co_return *result;

        auto start = std::chrono::steady_clock::now();
        while (true)
        {
            if (state_->closed.load(std::memory_order_acquire))
                co_return std::pair{
                    coro::net::io_status{.type = coro::net::io_status::kind::closed}, rpc::mutable_byte_span{}};

            if (timeout.count() == 0)
            {
                co_await state_->scheduler->schedule();
                co_return std::pair{
                    coro::net::io_status{.type = coro::net::io_status::kind::timeout}, rpc::mutable_byte_span{}};
            }

            if (std::chrono::steady_clock::now() - start >= timeout)
                co_return std::pair{
                    coro::net::io_status{.type = coro::net::io_status::kind::timeout}, rpc::mutable_byte_span{}};

            co_await state_->scheduler->schedule();

            if (auto result = try_receive_once())
                co_return *result;
        }
    }

    // Outbound: push to send_q_ and return immediately.
    // send_proxy_loop handles forwarding to the underlying stream.
    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        if (state_->send_failed.load(std::memory_order_acquire))
        {
            co_return coro::net::io_status{.type = static_cast<coro::net::io_status::kind>(
                                               state_->send_failure_type.load(std::memory_order_acquire)),
                .native_code = state_->send_failure_native_code.load(std::memory_order_acquire)};
        }
        if (state_->closed.load(std::memory_order_acquire))
            co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};

        while (!buffer.empty())
        {
            size_t chunk = std::min(buffer.size(), streaming::spsc_queue::max_payload);
            streaming::spsc_queue::blob blob{};
            auto len = static_cast<uint32_t>(chunk);
            std::memcpy(blob.data(), &len, streaming::spsc_queue::header_size);
            std::memcpy(blob.data() + streaming::spsc_queue::header_size, buffer.data(), chunk);

            state_->pending_send_blobs.fetch_add(1, std::memory_order_acq_rel);
            while (!state_->send_q_.push(blob))
            {
                if (state_->closed.load(std::memory_order_acquire))
                {
                    state_->pending_send_blobs.fetch_sub(1, std::memory_order_acq_rel);
                    co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
                }
                if (state_->send_failed.load(std::memory_order_acquire))
                {
                    state_->pending_send_blobs.fetch_sub(1, std::memory_order_acq_rel);
                    co_return coro::net::io_status{.type = static_cast<coro::net::io_status::kind>(
                                                       state_->send_failure_type.load(std::memory_order_acquire)),
                        .native_code = state_->send_failure_native_code.load(std::memory_order_acquire)};
                }
                co_await state_->scheduler->schedule();
            }
            buffer = buffer.subspan(chunk);
        }

        co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
    }

    bool stream::is_closed() const
    {
        return state_->closed.load(std::memory_order_acquire) || state_->underlying->is_closed();
    }

    auto stream::set_closed() -> coro::task<void>
    {
        request_stop();

        while (state_->pending_send_blobs.load(std::memory_order_acquire) > 0)
        {
            if (!state_->underlying || state_->underlying->is_closed())
                break;
            if (state_->send_failed.load(std::memory_order_acquire))
                break;

            co_await state_->scheduler->schedule();
        }

        if (state_->underlying)
            co_await state_->underlying->set_closed();
        co_return;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return state_->underlying->get_peer_info();
    }

} // namespace streaming::spsc_wrapping
