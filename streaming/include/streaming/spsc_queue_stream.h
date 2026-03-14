// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// spsc_queue_stream.h - Stream adapter wrapping a pair of SPSC queues
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "stream.h"

#include <spsc/queue.h>

namespace streaming
{
    // Fixed blob size matching the SPSC transport's wire format
    static constexpr std::size_t spsc_blob_capacity = 10024;
    static constexpr std::size_t spsc_header_size = sizeof(uint32_t);
    static constexpr std::size_t spsc_max_payload = spsc_blob_capacity - spsc_header_size;

    using spsc_blob = std::array<uint8_t, spsc_blob_capacity>;
    using spsc_raw_queue = ::spsc::queue<spsc_blob, spsc_blob_capacity>;

    // Stream adapter over two SPSC lock-free queues.
    //
    // Wire format: each blob begins with a 4-byte little-endian uint32_t indicating
    // how many payload bytes follow in that blob.  Blobs are fixed size; unused
    // trailing bytes are zero.  Large messages are split across multiple blobs.
    class spsc_queue_stream : public stream
    {
    public:
        // send_q: queue this side writes into (the other side reads from)
        // recv_q: queue this side reads from  (the other side writes into)
        spsc_queue_stream(spsc_raw_queue* send_q, spsc_raw_queue* recv_q, std::shared_ptr<coro::scheduler> scheduler);

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override;

        // Async send-all: yields via the scheduler when the send queue is full.
        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;

        bool is_closed() const override;
        void set_closed() override;
        peer_info get_peer_info() const override;

    private:
        spsc_raw_queue* send_queue_;
        spsc_raw_queue* recv_queue_;
        std::shared_ptr<coro::scheduler> scheduler_;

        // Leftover bytes from a partially consumed blob
        std::vector<uint8_t> leftover_;
        bool closed_{false};
    };

} // namespace streaming
