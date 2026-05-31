// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <compression_stream/compression_stream_config.h>
#include <streaming/stream.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <rpc/internal/coro_runtime/mutex.h>
#endif

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace streaming::compression
{
    class stream : public ::streaming::stream
    {
    public:
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            ::rpc::compression_stream::stream_settings settings = {});
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;
        stream(stream&&) = delete;
        auto operator=(stream&&) -> stream& = delete;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override;

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override;
        [[nodiscard]] bool is_closed() const override;
        auto set_closed() -> CORO_TASK(void) override;
        [[nodiscard]] auto get_peer_info() const -> peer_info override;

    private:
        std::shared_ptr<::streaming::stream> underlying_;
        ::rpc::compression_stream::stream_settings settings_;
        ZSTD_CCtx_s* compression_context_{nullptr};
        ZSTD_DCtx_s* decompression_context_{nullptr};
        std::vector<uint8_t> send_buffer_;
        std::vector<uint8_t> receive_buffer_;
        size_t receive_buffer_size_{0};
        size_t receive_buffer_offset_{0};
        bool decompression_may_have_pending_output_{false};
        uint64_t current_input_chunk_bytes_{0};
        uint64_t current_input_chunk_output_bytes_{0};
        std::atomic<bool> closed_{false};
#ifdef CANOPY_BUILD_COROUTINE
        rpc::coro::mutex send_mtx_;
        rpc::coro::mutex receive_mtx_;
#else
        std::mutex send_mtx_;
        std::mutex receive_mtx_;
#endif

        bool expansion_limit_exceeded(size_t produced_bytes);
        auto receive_compressed_input(std::chrono::milliseconds timeout) -> CORO_TASK(rpc::io_status);
    };
} // namespace streaming::compression
