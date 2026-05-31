// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// Zstandard compression stream implementation.
#include <streaming/compression/stream.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include <rpc/internal/error_codes.h>

#include <zstd.h>

namespace streaming::compression
{
    namespace
    {
        constexpr uint64_t default_buffer_bytes = 16 * 1024;
        constexpr uint64_t max_configured_buffer_bytes = 4 * 1024 * 1024;

        auto bounded_buffer_size(uint64_t configured) -> size_t
        {
            const uint64_t value = configured == 0 ? default_buffer_bytes : configured;
            if (value > max_configured_buffer_bytes)
                throw std::invalid_argument("compression stream buffer setting is too large");
            return static_cast<size_t>(value);
        }

        auto make_closed_status() -> rpc::io_status
        {
            return rpc::io_status{FLD(type) rpc::io_status::kind::closed};
        }

        auto make_error_status() -> rpc::io_status
        {
            return rpc::io_status{FLD(type) rpc::io_status::kind::native, FLD(native_code) rpc::error::TRANSPORT_ERROR()};
        }
    } // namespace

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        ::rpc::compression_stream::stream_settings settings)
        : underlying_(std::move(underlying))
        , settings_(std::move(settings))
        , send_buffer_(bounded_buffer_size(settings_.send_buffer_bytes))
        , receive_buffer_(bounded_buffer_size(settings_.receive_buffer_bytes))
    {
        if (!underlying_)
            throw std::invalid_argument("compression stream requires an underlying stream");
        if (settings_.algorithm != ::rpc::compression_stream::compression_algorithm::zstd)
            throw std::invalid_argument("unsupported compression stream algorithm");

        compression_context_ = ZSTD_createCCtx();
        decompression_context_ = ZSTD_createDCtx();
        if (!compression_context_ || !decompression_context_)
            throw std::runtime_error("failed to create Zstd compression contexts");

        auto result = ZSTD_CCtx_setParameter(compression_context_, ZSTD_c_compressionLevel, settings_.level);
        if (ZSTD_isError(result))
            throw std::invalid_argument("invalid Zstd compression level");
    }

    stream::~stream()
    {
        ZSTD_freeCCtx(compression_context_);
        ZSTD_freeDCtx(decompression_context_);
    }

    auto stream::send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto send_lock = CO_AWAIT send_mtx_.scoped_lock();
#else
        std::unique_lock<std::mutex> send_lock(send_mtx_);
#endif
        if (closed_.load(std::memory_order_acquire))
            CO_RETURN make_closed_status();

        ZSTD_inBuffer input{buffer.data(), buffer.size(), 0};
        while (input.pos < input.size || input.size == 0)
        {
            ZSTD_outBuffer output{send_buffer_.data(), send_buffer_.size(), 0};
            const auto remaining = ZSTD_compressStream2(compression_context_, &output, &input, ZSTD_e_flush);
            if (ZSTD_isError(remaining))
            {
                RPC_WARNING("Zstd compression failed: {}", ZSTD_getErrorName(remaining));
                closed_.store(true, std::memory_order_release);
                CO_RETURN make_error_status();
            }

            if (output.pos != 0)
            {
                auto status = CO_AWAIT underlying_->send(
                    rpc::byte_span{reinterpret_cast<const char*>(send_buffer_.data()), output.pos});
                if (!status.is_ok())
                {
                    closed_.store(true, std::memory_order_release);
                    CO_RETURN status;
                }
            }

            if (remaining == 0 && input.pos == input.size)
                break;
        }

        CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
    }

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout) -> CORO_TASK(::streaming::receive_result)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto receive_lock = CO_AWAIT receive_mtx_.scoped_lock();
#else
        std::unique_lock<std::mutex> receive_lock(receive_mtx_);
#endif
        if (closed_.load(std::memory_order_acquire))
            CO_RETURN ::streaming::receive_result{make_closed_status(), {}};

        while (true)
        {
            if (receive_buffer_offset_ == receive_buffer_size_ && !decompression_may_have_pending_output_)
            {
                auto status = CO_AWAIT receive_compressed_input(timeout);
                if (!status.is_ok())
                    CO_RETURN ::streaming::receive_result{status, {}};
            }

            ZSTD_inBuffer input{
                receive_buffer_.data() + receive_buffer_offset_, receive_buffer_size_ - receive_buffer_offset_, 0};
            ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
            const auto result = ZSTD_decompressStream(decompression_context_, &output, &input);
            receive_buffer_offset_ += input.pos;
            decompression_may_have_pending_output_ = output.pos == output.size;

            if (ZSTD_isError(result))
            {
                RPC_WARNING("Zstd decompression failed: {}", ZSTD_getErrorName(result));
                closed_.store(true, std::memory_order_release);
                CO_RETURN ::streaming::receive_result{make_error_status(), {}};
            }

            if (output.pos != 0)
            {
                if (expansion_limit_exceeded(output.pos))
                {
                    RPC_WARNING("Compression stream expansion ratio limit exceeded");
                    closed_.store(true, std::memory_order_release);
                    CO_RETURN ::streaming::receive_result{make_closed_status(), {}};
                }
                if (receive_buffer_offset_ == receive_buffer_size_ && !decompression_may_have_pending_output_)
                {
                    receive_buffer_offset_ = 0;
                    receive_buffer_size_ = 0;
                    current_input_chunk_bytes_ = 0;
                    current_input_chunk_output_bytes_ = 0;
                }
                CO_RETURN ::streaming::receive_result{
                    rpc::io_status{FLD(type) rpc::io_status::kind::ok}, buffer.subspan(0, output.pos)};
            }

            if (receive_buffer_offset_ == receive_buffer_size_)
            {
                receive_buffer_offset_ = 0;
                receive_buffer_size_ = 0;
                current_input_chunk_bytes_ = 0;
                current_input_chunk_output_bytes_ = 0;
            }

            if (receive_buffer_size_ == 0)
                continue;

            if (input.pos == 0)
            {
                if (decompression_may_have_pending_output_)
                {
                    decompression_may_have_pending_output_ = false;
                    continue;
                }
                RPC_WARNING("Zstd decompression made no progress");
                closed_.store(true, std::memory_order_release);
                CO_RETURN ::streaming::receive_result{make_error_status(), {}};
            }
        }
    }

    bool stream::is_closed() const
    {
        return closed_.load(std::memory_order_acquire) || underlying_->is_closed();
    }

    auto stream::set_closed() -> CORO_TASK(void)
    {
        closed_.store(true, std::memory_order_release);
        CO_AWAIT underlying_->set_closed();
        CO_RETURN;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return underlying_->get_peer_info();
    }

    bool stream::expansion_limit_exceeded(size_t produced_bytes)
    {
        const auto max_uint64 = std::numeric_limits<uint64_t>::max();
        if (produced_bytes > max_uint64 - current_input_chunk_output_bytes_)
            return true;
        current_input_chunk_output_bytes_ += produced_bytes;

        if (settings_.max_decompressed_chunk_bytes != 0
            && current_input_chunk_output_bytes_ > settings_.max_decompressed_chunk_bytes)
            return true;

        if (settings_.max_expansion_ratio == 0 || current_input_chunk_bytes_ == 0)
            return false;

        const auto compressed_bytes = current_input_chunk_bytes_;
        if (settings_.max_expansion_ratio > max_uint64 / compressed_bytes)
            return false;

        return current_input_chunk_output_bytes_ > compressed_bytes * settings_.max_expansion_ratio;
    }

    auto stream::receive_compressed_input(std::chrono::milliseconds timeout) -> CORO_TASK(rpc::io_status)
    {
        auto [status, span] = CO_AWAIT underlying_->receive(
            rpc::mutable_byte_span{reinterpret_cast<char*>(receive_buffer_.data()), receive_buffer_.size()}, timeout);
        if (!status.is_ok() || span.empty())
            CO_RETURN status.is_ok() ? rpc::io_status{FLD(type) rpc::io_status::kind::timeout} : status;

        receive_buffer_offset_ = 0;
        receive_buffer_size_ = span.size();
        decompression_may_have_pending_output_ = false;
        current_input_chunk_bytes_ = span.size();
        current_input_chunk_output_bytes_ = 0;
        CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
    }
} // namespace streaming::compression
