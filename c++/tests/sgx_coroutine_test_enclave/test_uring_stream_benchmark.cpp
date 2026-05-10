/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_uring.h"

#include <io_uring/tcp.h>
#include <streaming/io_uring/stream.h>
#include <transports/sgx_coroutine/enclave/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

namespace io_uring_test_enclave
{
    namespace
    {
        using clock_type = std::chrono::steady_clock;

        constexpr uint32_t max_benchmark_payload_size = 1024U * 1024U;
        constexpr uint16_t first_benchmark_port = 25400;
        constexpr uint16_t last_benchmark_port = 25432;

        struct stream_benchmark_server_state
        {
            std::atomic<int> error_code{rpc::error::OK()};
            std::shared_ptr<rpc::event> done;
        };

        int64_t elapsed_benchmark_units(
            uint64_t start_ticks,
            bool send_reply)
        {
            const auto end_ticks = rpc::sgx::coro::enclave::read_runtime_tick_counter();
            const auto elapsed_ticks = end_ticks >= start_ticks ? end_ticks - start_ticks : 0;
            if (elapsed_ticks == 0)
                return 0;

            return static_cast<int64_t>(
                send_reply ? rpc::sgx::coro::enclave::runtime_ticks_to_microseconds(elapsed_ticks)
                           : rpc::sgx::coro::enclave::runtime_ticks_to_nanoseconds(elapsed_ticks));
        }

        io_uring_test::stream_benchmark_stats make_empty_stats(uint32_t payload_size)
        {
            io_uring_test::stream_benchmark_stats stats;
            stats.avg = 0.0;
            stats.min_value = 0.0;
            stats.max_value = 0.0;
            stats.p50 = 0.0;
            stats.p90 = 0.0;
            stats.p95 = 0.0;
            stats.blob_size = payload_size;
            stats.samples = 0;
            stats.valid = false;
            return stats;
        }

        io_uring_test::stream_benchmark_stats compute_stream_stats(
            std::vector<int64_t> samples,
            uint32_t payload_size,
            uint32_t trim_count)
        {
            auto stats = make_empty_stats(payload_size);
            if (samples.size() <= static_cast<size_t>(trim_count) * 2U)
            {
                return stats;
            }

            std::sort(samples.begin(), samples.end());

            const size_t begin = trim_count;
            const size_t end = samples.size() - trim_count;
            const size_t count = end - begin;
            if (count == 0)
            {
                return stats;
            }

            const auto mid_begin = samples.begin() + static_cast<long>(begin);
            const auto mid_end = samples.begin() + static_cast<long>(end);
            const auto sum = std::accumulate(mid_begin, mid_end, int64_t{0});

            stats.avg = static_cast<double>(sum) / static_cast<double>(count);
            stats.min_value = static_cast<double>(*mid_begin);
            stats.max_value = static_cast<double>(*(mid_end - 1));
            stats.p50 = static_cast<double>(*(mid_begin + static_cast<long>((count * 50U) / 100U)));
            stats.p90 = static_cast<double>(*(mid_begin + static_cast<long>((count * 90U) / 100U)));
            stats.p95 = static_cast<double>(*(mid_begin + static_cast<long>((count * 95U) / 100U)));
            stats.samples = static_cast<uint32_t>(count);
            stats.valid = true;
            return stats;
        }

        CORO_TASK(int)
        receive_exact_stream(
            const std::shared_ptr<streaming::stream>& stream,
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout)
        {
            size_t bytes_received = 0;
            while (bytes_received < buffer.size())
            {
                auto [status, received] = CO_AWAIT stream->receive(buffer.subspan(bytes_received), timeout);
                if (!status.is_ok() || received.empty())
                {
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }

                bytes_received += received.size();
            }

            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        wait_for_server_done(
            const std::shared_ptr<rpc::service>& service,
            const std::shared_ptr<stream_benchmark_server_state>& state)
        {
            const auto deadline = clock_type::now() + std::chrono::milliseconds{10000};
            while (state && state->done && !state->done->is_set())
            {
                if (clock_type::now() >= deadline)
                {
                    CO_RETURN rpc::error::CALL_TIMEOUT();
                }

                CO_AWAIT service->get_scheduler()->schedule();
            }

            CO_RETURN state ? state->error_code.load(std::memory_order_acquire) : rpc::error::INVALID_DATA();
        }

        CORO_TASK(void)
        run_stream_benchmark_server(
            std::shared_ptr<rpc::io_uring::acceptor> acceptor,
            std::shared_ptr<stream_benchmark_server_state> state,
            uint32_t total_iterations,
            uint32_t payload_size,
            bool send_reply)
        {
            int err = rpc::error::OK();

            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                err = accept_result.error_code != rpc::error::OK() ? accept_result.error_code
                                                                   : rpc::error::PROTOCOL_ERROR();
                state->error_code.store(err, std::memory_order_release);
                state->done->set();
                CO_RETURN;
            }

            auto stream = accept_result.connection;
            std::vector<uint8_t> buffer(payload_size);
            for (uint32_t iteration = 0; iteration < total_iterations; ++iteration)
            {
                err = CO_AWAIT receive_exact_stream(
                    stream, rpc::mutable_byte_span(buffer), std::chrono::milliseconds{1000});
                if (err != rpc::error::OK())
                {
                    break;
                }

                if (send_reply)
                {
                    auto status = CO_AWAIT stream->send(rpc::byte_span(buffer));
                    if (!status.is_ok())
                    {
                        err = rpc::error::TRANSPORT_ERROR();
                        break;
                    }
                }
            }

            CO_AWAIT stream->set_closed();
            state->error_code.store(err, std::memory_order_release);
            state->done->set();
            CO_RETURN;
        }
    } // namespace

    CORO_TASK(int)
    test_uring::stream_benchmark(
        bool send_reply,
        uint32_t iterations,
        uint32_t warmup,
        uint32_t payload_size,
        io_uring_test::stream_benchmark_stats& stats)
    {
        stats = make_empty_stats(payload_size);

        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }
        if (iterations == 0 || payload_size == 0 || payload_size > max_benchmark_payload_size)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        const uint32_t total_iterations = iterations + warmup;
        if (total_iterations < iterations)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
        uint16_t port = 0;
        int listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = first_benchmark_port; candidate_port < last_benchmark_port; ++candidate_port)
        {
            listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (port == 0)
        {
            CO_RETURN listen_error;
        }

        auto state = std::make_shared<stream_benchmark_server_state>();
        state->done = std::make_shared<rpc::event>(false);
        state->done->set_scheduler(child_service_->get_scheduler().get());

        if (!child_service_->spawn(run_stream_benchmark_server(acceptor, state, total_iterations, payload_size, send_reply)))
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        rpc::io_uring::connector connector(controller_);
        auto connect_result
            = streaming::io_uring::make_stream_result(CO_AWAIT connector.connect_loopback_with_result(port), port);
        if (connect_result.error_code != rpc::error::OK() || !connect_result.connection)
        {
            CO_AWAIT acceptor->close();
            auto server_err = CO_AWAIT wait_for_server_done(child_service_, state);
            (void)server_err;
            CO_RETURN connect_result.error_code != rpc::error::OK() ? connect_result.error_code
                                                                    : rpc::error::PROTOCOL_ERROR();
        }

        auto client_stream = connect_result.connection;
        std::vector<uint8_t> payload(payload_size, 0xab);
        std::vector<uint8_t> response(payload_size);
        std::vector<int64_t> samples;
        samples.reserve(iterations);

        int err = rpc::error::OK();
        for (uint32_t iteration = 0; iteration < total_iterations; ++iteration)
        {
            const auto start_ticks = rpc::sgx::coro::enclave::read_runtime_tick_counter();

            auto send_status = CO_AWAIT client_stream->send(rpc::byte_span(payload));
            if (!send_status.is_ok())
            {
                err = rpc::error::TRANSPORT_ERROR();
                break;
            }

            if (send_reply)
            {
                err = CO_AWAIT receive_exact_stream(
                    client_stream, rpc::mutable_byte_span(response), std::chrono::milliseconds{1000});
                if (err != rpc::error::OK())
                {
                    break;
                }
            }

            if (iteration >= warmup)
            {
                samples.push_back(elapsed_benchmark_units(start_ticks, send_reply));
            }
        }

        CO_AWAIT client_stream->set_closed();
        const auto server_err = CO_AWAIT wait_for_server_done(child_service_, state);
        CO_AWAIT acceptor->close();

        if (err != rpc::error::OK())
        {
            CO_RETURN err;
        }
        if (server_err != rpc::error::OK())
        {
            CO_RETURN server_err;
        }

        stats = compute_stream_stats(std::move(samples), payload_size, iterations / 10U);
        CO_RETURN stats.valid ? rpc::error::OK() : rpc::error::INVALID_DATA();
    }
} // namespace io_uring_test_enclave
