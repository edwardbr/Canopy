// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <streaming/stream.h>
#include <spsc/queue.h>

namespace streaming::spsc_queue
{
    inline constexpr std::size_t blob_capacity = 10024;
    inline constexpr std::size_t header_size = sizeof(uint32_t);
    inline constexpr std::size_t max_payload = blob_capacity - header_size;

    using blob = std::array<uint8_t, blob_capacity>;
    using queue_type = ::spsc::queue<blob, blob_capacity>;

    class stream : public ::streaming::stream
    {
    public:
        stream(queue_type* send_q, queue_type* recv_q, std::shared_ptr<coro::scheduler> scheduler);

        auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

        auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override;
        bool is_closed() const override;
        void set_closed() override;
        auto get_peer_info() const -> peer_info override;

    private:
        queue_type* send_queue_;
        queue_type* recv_queue_;
        std::shared_ptr<coro::scheduler> scheduler_;
        std::vector<uint8_t> leftover_;
        bool closed_{false};
    };
} // namespace streaming::spsc_queue
