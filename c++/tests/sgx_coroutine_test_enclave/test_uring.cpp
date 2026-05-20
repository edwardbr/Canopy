/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_uring.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace io_uring_test_enclave
{
    test_uring::test_uring(
        std::shared_ptr<rpc::io_uring::controller> controller,
        std::shared_ptr<rpc::service> child_service)
        : controller_(std::move(controller))
        , child_service_(std::move(child_service))
    {
    }

    CORO_TASK(int)
    test_uring::test_noop(
        bool schedule,
        uint32_t iterations,
        bool use_proactor)
    {
        controller_->set_wait_strategy(
            use_proactor ? rpc::io_uring::wait_strategy::proactor : rpc::io_uring::wait_strategy::cooperative_poll);
        controller_->reset_measurements();
        if (iterations == 0)
        {
            store_noop_measurement();
            CO_RETURN rpc::error::OK();
        }

        if (schedule)
        {
            rpc::io_uring::data ring_data;
            auto cache_err = CO_AWAIT controller_->get_iouring_data(ring_data);
            if (cache_err != rpc::error::OK())
            {
                store_noop_measurement();
                CO_RETURN cache_err;
            }

            struct scheduled_noop_state
            {
                explicit scheduled_noop_state(uint32_t operation_count)
                    : remaining(operation_count)
                {
                }

                void complete_one(int ret)
                {
                    if (ret != rpc::error::OK())
                    {
                        int expected = rpc::error::OK();
                        first_error.compare_exchange_strong(expected, ret);
                    }

                    remaining.fetch_sub(1, std::memory_order_acq_rel);
                }

                std::atomic<uint32_t> remaining;
                std::atomic<int> first_error{rpc::error::OK()};
            };

            rpc::shared_ptr<test_uring> p_this = shared_from_this();
            std::shared_ptr<scheduled_noop_state> state;
            try
            {
                state = std::make_shared<scheduled_noop_state>(iterations);
            }
            catch (const std::bad_alloc&)
            {
                store_noop_measurement();
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                store_noop_measurement();
                CO_RETURN rpc::error::EXCEPTION();
            }

            for (int i = 0; i < iterations; i++)
            {
                // Coroutine lambdas must not capture stack state here. The
                // lambda object is destroyed after the coroutine is created,
                // so everything needed after the first suspension is passed
                // as a parameter into the coroutine frame.
                auto task = [](rpc::shared_ptr<test_uring> p_this,
                                std::shared_ptr<scheduled_noop_state> state) -> CORO_TASK(void)
                {
                    auto ret = CO_AWAIT p_this->controller_->no_op();
                    state->complete_one(ret);
                    co_return;
                }(p_this, state);

                if (!child_service_->spawn(std::move(task)))
                    state->complete_one(rpc::error::TRANSPORT_ERROR());
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10000};
            while (state->remaining.load(std::memory_order_acquire) != 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    store_noop_measurement();
                    RPC_WARNING(
                        "scheduled io_uring no_op test timed out remaining={} first_error={}",
                        state->remaining.load(std::memory_order_acquire),
                        state->first_error.load(std::memory_order_acquire));
                    CO_RETURN rpc::error::CALL_TIMEOUT();
                }

                CO_AWAIT child_service_->get_scheduler()->schedule();
            }

            int ret = state->first_error.load(std::memory_order_acquire);
            if (ret != rpc::error::OK())
            {
                store_noop_measurement();
                CO_RETURN ret;
            }
        }
        else
        {
            for (int i = 0; i < iterations; i++)
            {
                auto ret = CO_AWAIT controller_->no_op();
                if (ret != rpc::error::OK())
                {
                    store_noop_measurement();
                    CO_RETURN ret;
                }
            }
        }
        store_noop_measurement();
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int) test_uring::self_ping_test()
    {
        CO_RETURN CO_AWAIT self_ping_roundtrip_test(1, 5);
    }

    CORO_TASK(int)
    test_uring::self_ping_roundtrip_test(
        uint32_t iterations,
        uint32_t payload_size)
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }
        if (iterations == 0 || payload_size == 0 || payload_size > 64U * 1024U)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25000; candidate_port < 25032; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct self_ping_state
        {
            void complete(int ret)
            {
                if (ret != rpc::error::OK())
                {
                    int expected = rpc::error::OK();
                    first_error.compare_exchange_strong(expected, ret);
                }
                done.store(true, std::memory_order_release);
            }

            std::atomic<bool> done{false};
            std::atomic<int> first_error{rpc::error::OK()};
            std::atomic<uint32_t> server_roundtrips{0};
        };

        std::shared_ptr<self_ping_state> state;
        try
        {
            state = std::make_shared<self_ping_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }
        auto server_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<self_ping_state> state,
                               uint32_t iterations,
                               uint32_t payload_size) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                state->complete(accept_result.error_code);
                CO_RETURN;
            }

            auto server_stream = std::move(accept_result.connection);
            for (uint32_t iteration = 0; iteration < iterations; ++iteration)
            {
                std::vector<uint8_t> expected_request;
                auto err = test_uring::make_self_ping_payload(expected_request, iteration, payload_size, false);
                if (err != rpc::error::OK())
                {
                    CO_AWAIT server_stream->set_closed();
                    state->complete(err);
                    CO_RETURN;
                }

                std::vector<uint8_t> request_buffer;
                err = test_uring::make_empty_payload_buffer(request_buffer, payload_size);
                if (err != rpc::error::OK())
                {
                    CO_AWAIT server_stream->set_closed();
                    state->complete(err);
                    CO_RETURN;
                }

                err = CO_AWAIT test_uring::receive_exact(server_stream, rpc::mutable_byte_span(request_buffer));
                if (err != rpc::error::OK() || request_buffer != expected_request)
                {
                    CO_AWAIT server_stream->set_closed();
                    state->complete(err != rpc::error::OK() ? err : rpc::error::INVALID_DATA());
                    CO_RETURN;
                }

                std::vector<uint8_t> response;
                err = test_uring::make_self_ping_payload(response, iteration, payload_size, true);
                if (err != rpc::error::OK())
                {
                    CO_AWAIT server_stream->set_closed();
                    state->complete(err);
                    CO_RETURN;
                }

                auto send_status = CO_AWAIT server_stream->send(rpc::byte_span(response));
                if (!send_status.is_ok())
                {
                    CO_AWAIT server_stream->set_closed();
                    state->complete(rpc::error::TRANSPORT_ERROR());
                    CO_RETURN;
                }

                state->server_roundtrips.fetch_add(1, std::memory_order_acq_rel);
            }

            CO_AWAIT server_stream->set_closed();
            state->complete(rpc::error::OK());
            CO_RETURN;
        }(acceptor, state, iterations, payload_size);

        if (!child_service_->spawn(std::move(server_task)))
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
            CO_RETURN connect_result.error_code;
        }

        auto client_stream = std::move(connect_result.connection);
        for (uint32_t iteration = 0; iteration < iterations; ++iteration)
        {
            std::vector<uint8_t> request;
            auto err = make_self_ping_payload(request, iteration, payload_size, false);
            if (err != rpc::error::OK())
            {
                CO_AWAIT client_stream->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN err;
            }

            auto send_status = CO_AWAIT client_stream->send(rpc::byte_span(request));
            if (!send_status.is_ok())
            {
                CO_AWAIT client_stream->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::TRANSPORT_ERROR();
            }

            std::vector<uint8_t> expected_response;
            err = make_self_ping_payload(expected_response, iteration, payload_size, true);
            if (err != rpc::error::OK())
            {
                CO_AWAIT client_stream->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN err;
            }

            std::vector<uint8_t> response_buffer;
            err = make_empty_payload_buffer(response_buffer, payload_size);
            if (err != rpc::error::OK())
            {
                CO_AWAIT client_stream->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN err;
            }

            err = CO_AWAIT test_uring::receive_exact(client_stream, rpc::mutable_byte_span(response_buffer));
            if (err != rpc::error::OK() || response_buffer != expected_response)
            {
                CO_AWAIT client_stream->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN err != rpc::error::OK() ? err : rpc::error::INVALID_DATA();
            }
        }

        CO_AWAIT client_stream->set_closed();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10000};
        while (!state->done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT acceptor->close();

        if (state->server_roundtrips.load(std::memory_order_acquire) != iterations)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        CO_RETURN state->first_error.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::self_ping_receive_timeout_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25032; candidate_port < 25064; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct timeout_state
        {
            void complete(int ret)
            {
                if (ret != rpc::error::OK())
                {
                    int expected = rpc::error::OK();
                    first_error.compare_exchange_strong(expected, ret);
                }
                done.store(true, std::memory_order_release);
            }

            std::atomic<bool> done{false};
            std::atomic<int> first_error{rpc::error::OK()};
        };

        std::shared_ptr<timeout_state> state;
        try
        {
            state = std::make_shared<timeout_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto server_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<timeout_state> state,
                               std::shared_ptr<rpc::service> child_service) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                state->complete(accept_result.error_code);
                CO_RETURN;
            }

            auto server_stream = std::move(accept_result.connection);
            const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
            while (std::chrono::steady_clock::now() < close_deadline)
            {
                CO_AWAIT child_service->get_scheduler()->schedule();
            }

            CO_AWAIT server_stream->set_closed();
            state->complete(rpc::error::OK());
            CO_RETURN;
        }(acceptor, state, child_service_);

        if (!child_service_->spawn(std::move(server_task)))
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
            CO_RETURN connect_result.error_code;
        }

        auto client_stream = std::move(connect_result.connection);
        std::array<uint8_t, 16> receive_buffer{};
        auto [receive_status, received] = CO_AWAIT client_stream->receive(
            rpc::mutable_byte_span(receive_buffer.data(), receive_buffer.size()), std::chrono::milliseconds{50});
        auto result = receive_status.is_timeout() && received.empty() ? rpc::error::OK() : rpc::error::INVALID_DATA();
        CO_AWAIT client_stream->set_closed();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10000};
        while (!state->done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT acceptor->close();
        if (result != rpc::error::OK())
        {
            CO_RETURN result;
        }

        CO_RETURN state->first_error.load(std::memory_order_acquire);
    }

    CORO_TASK(int)
    test_uring::self_ping_multi_stream_test(
        uint32_t connection_count,
        uint32_t iterations,
        uint32_t payload_size)
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }
        if (connection_count == 0 || connection_count > 32 || iterations == 0 || payload_size == 0
            || payload_size > 64U * 1024U)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25064; candidate_port < 25128; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port, connection_count);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct multi_stream_state
        {
            explicit multi_stream_state(uint32_t task_count)
                : remaining_tasks(task_count)
            {
            }

            void complete_task(int ret)
            {
                if (ret != rpc::error::OK())
                {
                    int expected = rpc::error::OK();
                    first_error.compare_exchange_strong(expected, ret);
                }
                remaining_tasks.fetch_sub(1, std::memory_order_acq_rel);
            }

            bool claim_stream_id(uint32_t stream_id)
            {
                if (stream_id == 0 || stream_id > 63)
                {
                    return false;
                }

                const auto bit = uint64_t{1} << (stream_id - 1U);
                auto current = accepted_stream_mask.load(std::memory_order_acquire);
                while ((current & bit) == 0)
                {
                    if (accepted_stream_mask.compare_exchange_weak(
                            current, current | bit, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        return true;
                    }
                }
                return false;
            }

            std::atomic<uint32_t> remaining_tasks;
            std::atomic<int> first_error{rpc::error::OK()};
            std::atomic<uint32_t> server_roundtrips{0};
            std::atomic<uint32_t> client_roundtrips{0};
            std::atomic<uint64_t> accepted_stream_mask{0};
        };

        std::shared_ptr<multi_stream_state> state;
        try
        {
            state = std::make_shared<multi_stream_state>(connection_count * 2U);
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        for (uint32_t connection_index = 0; connection_index < connection_count; ++connection_index)
        {
            auto server_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                                   std::shared_ptr<multi_stream_state> state,
                                   uint32_t connection_count,
                                   uint32_t iterations,
                                   uint32_t payload_size) -> CORO_TASK(void)
            {
                auto accept_result
                    = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
                if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
                {
                    state->complete_task(accept_result.error_code);
                    CO_RETURN;
                }

                auto server_stream = std::move(accept_result.connection);
                uint32_t stream_id = 0;
                for (uint32_t iteration = 0; iteration < iterations; ++iteration)
                {
                    std::vector<uint8_t> request_buffer;
                    auto err = test_uring::make_empty_payload_buffer(request_buffer, payload_size);
                    if (err != rpc::error::OK())
                    {
                        CO_AWAIT server_stream->set_closed();
                        state->complete_task(err);
                        CO_RETURN;
                    }

                    err = CO_AWAIT test_uring::receive_exact(
                        server_stream, rpc::mutable_byte_span(request_buffer), std::chrono::milliseconds{5000});
                    if (err != rpc::error::OK())
                    {
                        CO_AWAIT server_stream->set_closed();
                        state->complete_task(err);
                        CO_RETURN;
                    }

                    if (iteration == 0)
                    {
                        err = test_uring::match_self_ping_request(
                            request_buffer, iteration, payload_size, connection_count, stream_id);
                        if (err != rpc::error::OK() || !state->claim_stream_id(stream_id))
                        {
                            RPC_WARNING(
                                "multi-stream server failed to claim first request stream_id={} err={} "
                                "connection_count={} payload_size={}",
                                stream_id,
                                err,
                                connection_count,
                                payload_size);
                            CO_AWAIT server_stream->set_closed();
                            state->complete_task(err != rpc::error::OK() ? err : rpc::error::INVALID_DATA());
                            CO_RETURN;
                        }
                    }
                    else
                    {
                        std::vector<uint8_t> expected_request;
                        err = test_uring::make_self_ping_payload(
                            expected_request, iteration, payload_size, false, stream_id);
                        if (err != rpc::error::OK() || request_buffer != expected_request)
                        {
                            RPC_WARNING(
                                "multi-stream server request mismatch stream_id={} iteration={} err={}",
                                stream_id,
                                iteration,
                                err);
                            CO_AWAIT server_stream->set_closed();
                            state->complete_task(err != rpc::error::OK() ? err : rpc::error::INVALID_DATA());
                            CO_RETURN;
                        }
                    }

                    std::vector<uint8_t> response;
                    err = test_uring::make_self_ping_payload(response, iteration, payload_size, true, stream_id);
                    if (err != rpc::error::OK())
                    {
                        CO_AWAIT server_stream->set_closed();
                        state->complete_task(err);
                        CO_RETURN;
                    }

                    auto send_status = CO_AWAIT server_stream->send(rpc::byte_span(response));
                    if (!send_status.is_ok())
                    {
                        CO_AWAIT server_stream->set_closed();
                        state->complete_task(rpc::error::TRANSPORT_ERROR());
                        CO_RETURN;
                    }

                    state->server_roundtrips.fetch_add(1, std::memory_order_acq_rel);
                }

                CO_AWAIT server_stream->set_closed();
                state->complete_task(rpc::error::OK());
                CO_RETURN;
            }(acceptor, state, connection_count, iterations, payload_size);

            if (!child_service_->spawn(std::move(server_task)))
            {
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::TRANSPORT_ERROR();
            }
        }

        for (uint32_t connection_index = 0; connection_index < connection_count; ++connection_index)
        {
            auto client_task = [](std::shared_ptr<rpc::io_uring::controller> controller,
                                   std::shared_ptr<multi_stream_state> state,
                                   uint16_t port,
                                   uint32_t connection_index,
                                   uint32_t iterations,
                                   uint32_t payload_size) -> CORO_TASK(void)
            {
                rpc::io_uring::connector connector(std::move(controller));
                auto connect_result = streaming::io_uring::make_stream_result(
                    CO_AWAIT connector.connect_loopback_with_result(port), port);
                if (connect_result.error_code != rpc::error::OK() || !connect_result.connection)
                {
                    state->complete_task(connect_result.error_code);
                    CO_RETURN;
                }

                auto client_stream = std::move(connect_result.connection);
                const auto stream_id = connection_index + 1U;
                for (uint32_t iteration = 0; iteration < iterations; ++iteration)
                {
                    std::vector<uint8_t> request;
                    auto err = test_uring::make_self_ping_payload(request, iteration, payload_size, false, stream_id);
                    if (err != rpc::error::OK())
                    {
                        CO_AWAIT client_stream->set_closed();
                        state->complete_task(err);
                        CO_RETURN;
                    }

                    auto send_status = CO_AWAIT client_stream->send(rpc::byte_span(request));
                    if (!send_status.is_ok())
                    {
                        CO_AWAIT client_stream->set_closed();
                        state->complete_task(rpc::error::TRANSPORT_ERROR());
                        CO_RETURN;
                    }

                    std::vector<uint8_t> expected_response;
                    err = test_uring::make_self_ping_payload(expected_response, iteration, payload_size, true, stream_id);
                    if (err != rpc::error::OK())
                    {
                        CO_AWAIT client_stream->set_closed();
                        state->complete_task(err);
                        CO_RETURN;
                    }

                    std::vector<uint8_t> response_buffer;
                    err = test_uring::make_empty_payload_buffer(response_buffer, payload_size);
                    if (err != rpc::error::OK())
                    {
                        CO_AWAIT client_stream->set_closed();
                        state->complete_task(err);
                        CO_RETURN;
                    }

                    err = CO_AWAIT test_uring::receive_exact(
                        client_stream, rpc::mutable_byte_span(response_buffer), std::chrono::milliseconds{5000});
                    if (err != rpc::error::OK() || response_buffer != expected_response)
                    {
                        RPC_WARNING(
                            "multi-stream client response mismatch stream_id={} iteration={} err={}",
                            stream_id,
                            iteration,
                            err);
                        CO_AWAIT client_stream->set_closed();
                        state->complete_task(err != rpc::error::OK() ? err : rpc::error::INVALID_DATA());
                        CO_RETURN;
                    }

                    state->client_roundtrips.fetch_add(1, std::memory_order_acq_rel);
                }

                CO_AWAIT client_stream->set_closed();
                state->complete_task(rpc::error::OK());
                CO_RETURN;
            }(controller_, state, port, connection_index, iterations, payload_size);

            if (!child_service_->spawn(std::move(client_task)))
            {
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::TRANSPORT_ERROR();
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20000};
        while (state->remaining_tasks.load(std::memory_order_acquire) != 0)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT acceptor->close();

        const auto expected_roundtrips = connection_count * iterations;
        if (state->server_roundtrips.load(std::memory_order_acquire) != expected_roundtrips
            || state->client_roundtrips.load(std::memory_order_acquire) != expected_roundtrips)
        {
            const auto first_error = state->first_error.load(std::memory_order_acquire);
            RPC_WARNING(
                "multi-stream roundtrip count mismatch expected={} server={} client={} first_error={}",
                expected_roundtrips,
                state->server_roundtrips.load(std::memory_order_acquire),
                state->client_roundtrips.load(std::memory_order_acquire),
                first_error);
            CO_RETURN first_error != rpc::error::OK() ? first_error : rpc::error::INVALID_DATA();
        }

        CO_RETURN state->first_error.load(std::memory_order_acquire);
    }

    CORO_TASK(int)
    test_uring::direct_descriptor_reuse_test(
        uint32_t descriptor_count,
        uint32_t cycles)
    {
        if (!controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }
        if (descriptor_count == 0 || descriptor_count > 128 || cycles == 0 || cycles > 1000)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        for (uint32_t cycle = 0; cycle < cycles; ++cycle)
        {
            std::vector<uint32_t> descriptors;
            try
            {
                descriptors.reserve(descriptor_count);
            }
            catch (const std::bad_alloc&)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                CO_RETURN rpc::error::EXCEPTION();
            }

            for (uint32_t index = 0; index < descriptor_count; ++index)
            {
                auto socket_result = CO_AWAIT controller_->create_tcp_ipv4_socket();
                if (socket_result.error_code != rpc::error::OK())
                {
                    for (auto descriptor : descriptors)
                    {
                        CO_AWAIT controller_->close_direct(descriptor);
                    }
                    CO_RETURN socket_result.error_code;
                }

                descriptors.push_back(socket_result.descriptor);
            }

            auto exhausted_result = CO_AWAIT controller_->create_tcp_ipv4_socket();
            if (exhausted_result.error_code == rpc::error::OK())
            {
                CO_AWAIT controller_->close_direct(exhausted_result.descriptor);
                for (auto descriptor : descriptors)
                {
                    CO_AWAIT controller_->close_direct(descriptor);
                }
                CO_RETURN rpc::error::INVALID_DATA();
            }

            for (auto descriptor : descriptors)
            {
                auto close_result = CO_AWAIT controller_->close_direct(descriptor);
                if (close_result.error_code != rpc::error::OK())
                {
                    CO_RETURN close_result.error_code;
                }
            }
        }

        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(int) test_uring::close_during_accept_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25128; candidate_port < 25164; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct accept_close_state
        {
            std::atomic<bool> done{false};
            std::atomic<int> result{rpc::error::OK()};
        };

        std::shared_ptr<accept_close_state> state;
        try
        {
            state = std::make_shared<accept_close_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto accept_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<accept_close_state> state) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.connection)
            {
                CO_AWAIT accept_result.connection->set_closed();
            }
            state->result.store(
                accept_result.error_code == rpc::error::OK() ? rpc::error::INVALID_DATA() : rpc::error::OK(),
                std::memory_order_release);
            state->done.store(true, std::memory_order_release);
            CO_RETURN;
        }(acceptor, state);

        if (!child_service_->spawn(std::move(accept_task)))
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT acceptor->close();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_RETURN state->result.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::close_during_receive_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25164; candidate_port < 25200; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct receive_close_state
        {
            std::atomic<bool> accepted{false};
            std::atomic<bool> receive_done{false};
            std::atomic<int> result{rpc::error::OK()};
            std::shared_ptr<streaming::stream> server_stream;
        };

        std::shared_ptr<receive_close_state> state;
        try
        {
            state = std::make_shared<receive_close_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto accept_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<receive_close_state> state) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                state->result.store(accept_result.error_code, std::memory_order_release);
                state->accepted.store(true, std::memory_order_release);
                CO_RETURN;
            }

            state->server_stream = std::move(accept_result.connection);
            state->accepted.store(true, std::memory_order_release);
            CO_RETURN;
        }(acceptor, state);

        if (!child_service_->spawn(std::move(accept_task)))
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
            CO_RETURN connect_result.error_code;
        }

        const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->accepted.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= accept_deadline)
            {
                CO_AWAIT connect_result.connection->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        if (!state->server_stream)
        {
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN state->result.load(std::memory_order_acquire);
        }

        auto receive_task = [](std::shared_ptr<receive_close_state> state) -> CORO_TASK(void)
        {
            std::array<uint8_t, 64> buffer{};
            auto [status, received] = CO_AWAIT state->server_stream->receive(
                rpc::mutable_byte_span(buffer.data(), buffer.size()), std::chrono::milliseconds{10000});
            state->result.store(
                !status.is_ok() && !status.is_timeout() && received.empty() ? rpc::error::OK() : rpc::error::INVALID_DATA(),
                std::memory_order_release);
            state->receive_done.store(true, std::memory_order_release);
            CO_RETURN;
        }(state);

        if (!child_service_->spawn(std::move(receive_task)))
        {
            CO_AWAIT state->server_stream->set_closed();
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT state->server_stream->set_closed();

        const auto receive_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->receive_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= receive_deadline)
            {
                CO_AWAIT connect_result.connection->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT connect_result.connection->set_closed();
        CO_AWAIT acceptor->close();
        CO_RETURN state->result.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::close_during_send_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25200; candidate_port < 25236; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct send_close_state
        {
            std::atomic<bool> accepted{false};
            std::atomic<bool> send_done{false};
            std::atomic<int> result{rpc::error::OK()};
            std::shared_ptr<streaming::stream> server_stream;
        };

        std::shared_ptr<send_close_state> state;
        try
        {
            state = std::make_shared<send_close_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto accept_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<send_close_state> state) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                state->result.store(accept_result.error_code, std::memory_order_release);
                state->accepted.store(true, std::memory_order_release);
                CO_RETURN;
            }

            state->server_stream = std::move(accept_result.connection);
            state->accepted.store(true, std::memory_order_release);
            CO_RETURN;
        }(acceptor, state);

        if (!child_service_->spawn(std::move(accept_task)))
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
            CO_RETURN connect_result.error_code;
        }

        const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->accepted.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= accept_deadline)
            {
                CO_AWAIT connect_result.connection->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        if (!state->server_stream)
        {
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN state->result.load(std::memory_order_acquire);
        }

        auto send_task = [](std::shared_ptr<streaming::stream> client_stream,
                             std::shared_ptr<send_close_state> state) -> CORO_TASK(void)
        {
            std::vector<uint8_t> payload;
            try
            {
                payload.assign(1024U * 1024U, 0x5aU);
            }
            catch (const std::bad_alloc&)
            {
                state->result.store(rpc::error::OUT_OF_MEMORY(), std::memory_order_release);
                state->send_done.store(true, std::memory_order_release);
                CO_RETURN;
            }
            catch (...)
            {
                state->result.store(rpc::error::EXCEPTION(), std::memory_order_release);
                state->send_done.store(true, std::memory_order_release);
                CO_RETURN;
            }

            auto status = CO_AWAIT client_stream->send(rpc::byte_span(payload));
            state->result.store(status.is_ok() ? rpc::error::INVALID_DATA() : rpc::error::OK(), std::memory_order_release);
            state->send_done.store(true, std::memory_order_release);
            CO_RETURN;
        }(connect_result.connection, state);

        if (!child_service_->spawn(std::move(send_task)))
        {
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT state->server_stream->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT connect_result.connection->set_closed();

        const auto send_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->send_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= send_deadline)
            {
                CO_AWAIT state->server_stream->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT state->server_stream->set_closed();
        CO_AWAIT acceptor->close();
        CO_RETURN state->result.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::controller_shutdown_rejects_new_work_test()
    {
        if (!controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        auto shutdown_error = CO_AWAIT controller_->shutdown();
        if (shutdown_error != rpc::error::OK())
        {
            CO_RETURN shutdown_error;
        }

        if (!controller_->is_shutdown_requested())
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        auto noop_error = CO_AWAIT controller_->no_op();
        CO_RETURN noop_error == rpc::error::OK() ? rpc::error::INVALID_DATA() : rpc::error::OK();
    }

    CORO_TASK(int) test_uring::controller_shutdown_scheduled_noop_test(uint32_t iterations)
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_ || iterations == 0)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);
        controller_->reset_measurements();

        struct shutdown_noop_state
        {
            explicit shutdown_noop_state(uint32_t operation_count)
                : remaining(operation_count)
            {
            }

            void complete_one(int ret)
            {
                if (ret == rpc::error::OK())
                {
                    successes.fetch_add(1, std::memory_order_acq_rel);
                }
                else
                {
                    failures.fetch_add(1, std::memory_order_acq_rel);
                }

                remaining.fetch_sub(1, std::memory_order_acq_rel);
            }

            std::atomic<uint32_t> remaining;
            std::atomic<uint32_t> successes{0};
            std::atomic<uint32_t> failures{0};
        };

        std::shared_ptr<shutdown_noop_state> state;
        try
        {
            state = std::make_shared<shutdown_noop_state>(iterations);
        }
        catch (const std::bad_alloc&)
        {
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }
        catch (...)
        {
            CO_RETURN rpc::error::EXCEPTION();
        }

        for (uint32_t index = 0; index < iterations; ++index)
        {
            auto task = [](std::shared_ptr<rpc::io_uring::controller> controller,
                            std::shared_ptr<shutdown_noop_state> state) -> CORO_TASK(void)
            {
                auto ret = CO_AWAIT controller->no_op();
                state->complete_one(ret);
                CO_RETURN;
            }(controller_, state);

            if (!child_service_->spawn(std::move(task)))
            {
                CO_AWAIT controller_->shutdown();
                CO_RETURN rpc::error::TRANSPORT_ERROR();
            }
        }

        // This is the fan-out shutdown rule: request shutdown before any
        // cooperative yield that could put this caller behind the spawned
        // work. The state transition is synchronous; the following yields
        // only give the spawned no-op calls time to observe it.
        controller_->request_shutdown();

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (state->remaining.load(std::memory_order_acquire) != 0)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        store_noop_measurement();
        CO_RETURN state->failures.load(std::memory_order_acquire) == 0 ? rpc::error::INVALID_DATA() : rpc::error::OK();
    }

    CORO_TASK(int) test_uring::controller_shutdown_pending_accept_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25236; candidate_port < 25272; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct accept_shutdown_state
        {
            std::atomic<bool> done{false};
            std::atomic<int> result{rpc::error::OK()};
        };

        std::shared_ptr<accept_shutdown_state> state;
        try
        {
            state = std::make_shared<accept_shutdown_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto accept_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<accept_shutdown_state> state) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            state->result.store(
                accept_result.error_code == rpc::error::OK() ? rpc::error::INVALID_DATA() : rpc::error::OK(),
                std::memory_order_release);
            state->done.store(true, std::memory_order_release);
            CO_RETURN;
        }(acceptor, state);

        if (!child_service_->spawn(std::move(accept_task)))
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT controller_->shutdown();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_RETURN state->result.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::controller_shutdown_pending_receive_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25272; candidate_port < 25308; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct receive_shutdown_state
        {
            std::atomic<bool> accepted{false};
            std::atomic<bool> receive_done{false};
            std::atomic<int> result{rpc::error::OK()};
            std::shared_ptr<streaming::stream> server_stream;
        };

        std::shared_ptr<receive_shutdown_state> state;
        try
        {
            state = std::make_shared<receive_shutdown_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto accept_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<receive_shutdown_state> state) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                state->result.store(accept_result.error_code, std::memory_order_release);
                state->accepted.store(true, std::memory_order_release);
                CO_RETURN;
            }

            state->server_stream = std::move(accept_result.connection);
            state->accepted.store(true, std::memory_order_release);
            CO_RETURN;
        }(acceptor, state);

        if (!child_service_->spawn(std::move(accept_task)))
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
            CO_RETURN connect_result.error_code;
        }

        const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->accepted.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= accept_deadline)
            {
                CO_AWAIT connect_result.connection->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        if (!state->server_stream)
        {
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN state->result.load(std::memory_order_acquire);
        }

        auto receive_task = [](std::shared_ptr<receive_shutdown_state> state) -> CORO_TASK(void)
        {
            std::array<uint8_t, 64> buffer{};
            auto [status, received] = CO_AWAIT state->server_stream->receive(
                rpc::mutable_byte_span(buffer.data(), buffer.size()), std::chrono::milliseconds{10000});
            state->result.store(
                !status.is_ok() && !status.is_timeout() && received.empty() ? rpc::error::OK() : rpc::error::INVALID_DATA(),
                std::memory_order_release);
            state->receive_done.store(true, std::memory_order_release);
            CO_RETURN;
        }(state);

        if (!child_service_->spawn(std::move(receive_task)))
        {
            CO_AWAIT state->server_stream->set_closed();
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT controller_->shutdown();

        const auto receive_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->receive_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= receive_deadline)
            {
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_RETURN state->result.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::controller_shutdown_pending_send_test()
    {
        if (!child_service_ || !child_service_->get_scheduler() || !controller_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        std::shared_ptr<rpc::io_uring::acceptor> acceptor;
        uint16_t port = 0;
        int last_listen_error = rpc::error::TRANSPORT_ERROR();
        for (uint16_t candidate_port = 25308; candidate_port < 25344; ++candidate_port)
        {
            try
            {
                acceptor = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (...)
            {
                CO_RETURN rpc::error::OUT_OF_MEMORY();
            }

            last_listen_error = CO_AWAIT acceptor->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (!acceptor || port == 0)
        {
            CO_RETURN last_listen_error;
        }

        struct send_shutdown_state
        {
            std::atomic<bool> accepted{false};
            std::atomic<bool> send_done{false};
            std::atomic<int> result{rpc::error::OK()};
            std::shared_ptr<streaming::stream> server_stream;
        };

        std::shared_ptr<send_shutdown_state> state;
        try
        {
            state = std::make_shared<send_shutdown_state>();
        }
        catch (...)
        {
        }

        if (!state)
        {
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::OUT_OF_MEMORY();
        }

        auto accept_task = [](std::shared_ptr<rpc::io_uring::acceptor> acceptor,
                               std::shared_ptr<send_shutdown_state> state) -> CORO_TASK(void)
        {
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), acceptor->port());
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                state->result.store(accept_result.error_code, std::memory_order_release);
                state->accepted.store(true, std::memory_order_release);
                CO_RETURN;
            }

            state->server_stream = std::move(accept_result.connection);
            state->accepted.store(true, std::memory_order_release);
            CO_RETURN;
        }(acceptor, state);

        if (!child_service_->spawn(std::move(accept_task)))
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
            CO_RETURN connect_result.error_code;
        }

        const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->accepted.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= accept_deadline)
            {
                CO_AWAIT connect_result.connection->set_closed();
                CO_AWAIT acceptor->close();
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        if (!state->server_stream)
        {
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN state->result.load(std::memory_order_acquire);
        }

        auto send_task = [](std::shared_ptr<streaming::stream> client_stream,
                             std::shared_ptr<send_shutdown_state> state) -> CORO_TASK(void)
        {
            std::vector<uint8_t> payload;
            try
            {
                payload.assign(1024U * 1024U, 0x73U);
            }
            catch (const std::bad_alloc&)
            {
                state->result.store(rpc::error::OUT_OF_MEMORY(), std::memory_order_release);
                state->send_done.store(true, std::memory_order_release);
                CO_RETURN;
            }
            catch (...)
            {
                state->result.store(rpc::error::EXCEPTION(), std::memory_order_release);
                state->send_done.store(true, std::memory_order_release);
                CO_RETURN;
            }

            auto status = CO_AWAIT client_stream->send(rpc::byte_span(payload));
            state->result.store(status.is_ok() ? rpc::error::INVALID_DATA() : rpc::error::OK(), std::memory_order_release);
            state->send_done.store(true, std::memory_order_release);
            CO_RETURN;
        }(connect_result.connection, state);

        if (!child_service_->spawn(std::move(send_task)))
        {
            CO_AWAIT connect_result.connection->set_closed();
            CO_AWAIT state->server_stream->set_closed();
            CO_AWAIT acceptor->close();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        for (uint32_t index = 0; index < 4; ++index)
        {
            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_AWAIT controller_->shutdown();

        const auto send_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!state->send_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= send_deadline)
            {
                CO_RETURN rpc::error::CALL_TIMEOUT();
            }

            CO_AWAIT child_service_->get_scheduler()->schedule();
        }

        CO_RETURN state->result.load(std::memory_order_acquire);
    }

    CORO_TASK(int) test_uring::get_noop_measurement(io_uring_test::iouring_noop_measurement& measurement)
    {
        measurement = last_noop_measurement_;
        CO_RETURN rpc::error::OK();
    }

    // private:
    io_uring_test::iouring_noop_measurement test_uring::to_test_measurement(
        const rpc::io_uring::controller_measurements& measurement)
    {
        return {measurement.no_op_calls,
            measurement.no_op_successes,
            measurement.no_op_failures,
            measurement.submit_attempts,
            measurement.submit_backpressure,
            measurement.completion_pump_calls,
            measurement.completion_entries,
            measurement.scheduler_yields,
            measurement.local_relax_spins,
            measurement.host_wake_calls,
            measurement.proactor_pump_starts,
            measurement.proactor_pump_iterations,
            measurement.proactor_waiter_suspends,
            measurement.proactor_resumes,
            measurement.proactor_start_failures,
            measurement.total_no_op_ticks,
            measurement.max_no_op_ticks};
    }

    void test_uring::store_noop_measurement()
    {
        last_noop_measurement_ = to_test_measurement(controller_->measurements());
        RPC_INFO(
            "io_uring no_op measurement calls={} success={} failures={} submit_attempts={} backpressure={} "
            "pump_calls={} completions={} scheduler_yields={} local_spins={} host_wakes={} proactor_starts={} "
            "proactor_iterations={} proactor_suspends={} proactor_resumes={} proactor_start_failures={} "
            "total_ticks={} max_ticks={}",
            last_noop_measurement_.no_op_calls,
            last_noop_measurement_.no_op_successes,
            last_noop_measurement_.no_op_failures,
            last_noop_measurement_.submit_attempts,
            last_noop_measurement_.submit_backpressure,
            last_noop_measurement_.completion_pump_calls,
            last_noop_measurement_.completion_entries,
            last_noop_measurement_.scheduler_yields,
            last_noop_measurement_.local_relax_spins,
            last_noop_measurement_.host_wake_calls,
            last_noop_measurement_.proactor_pump_starts,
            last_noop_measurement_.proactor_pump_iterations,
            last_noop_measurement_.proactor_waiter_suspends,
            last_noop_measurement_.proactor_resumes,
            last_noop_measurement_.proactor_start_failures,
            last_noop_measurement_.total_no_op_ticks,
            last_noop_measurement_.max_no_op_ticks);
    }

    int test_uring::make_empty_payload_buffer(
        std::vector<uint8_t>& payload,
        uint32_t payload_size)
    {
        try
        {
            payload.assign(payload_size, 0);
        }
        catch (const std::bad_alloc&)
        {
            return rpc::error::OUT_OF_MEMORY();
        }
        catch (...)
        {
            return rpc::error::EXCEPTION();
        }

        return rpc::error::OK();
    }

    int test_uring::make_self_ping_payload(
        std::vector<uint8_t>& payload,
        uint32_t iteration,
        uint32_t payload_size,
        bool response,
        uint32_t stream_id)
    {
        auto err = make_empty_payload_buffer(payload, payload_size);
        if (err != rpc::error::OK())
        {
            return err;
        }

        if (stream_id == 0 && iteration == 0 && payload_size == 5)
        {
            const char* literal = response ? "world" : "hello";
            for (uint32_t index = 0; index < payload_size; ++index)
            {
                payload[index] = static_cast<uint8_t>(literal[index]);
            }
            return rpc::error::OK();
        }

        const auto seed = response ? 0x51U : 0x23U;
        for (uint32_t index = 0; index < payload_size; ++index)
        {
            payload[index] = static_cast<uint8_t>((seed + (stream_id * 13U) + (iteration * 17U) + (index * 31U)) & 0xffU);
        }
        return rpc::error::OK();
    }

    int test_uring::match_self_ping_request(
        const std::vector<uint8_t>& payload,
        uint32_t iteration,
        uint32_t payload_size,
        uint32_t max_stream_id,
        uint32_t& stream_id)
    {
        if (payload.size() != payload_size || max_stream_id == 0 || max_stream_id > 63)
        {
            return rpc::error::INVALID_DATA();
        }

        std::vector<uint8_t> expected;
        for (uint32_t candidate_stream_id = 1; candidate_stream_id <= max_stream_id; ++candidate_stream_id)
        {
            auto err = make_self_ping_payload(expected, iteration, payload_size, false, candidate_stream_id);
            if (err != rpc::error::OK())
            {
                return err;
            }

            if (payload == expected)
            {
                stream_id = candidate_stream_id;
                return rpc::error::OK();
            }
        }

        return rpc::error::INVALID_DATA();
    }

    CORO_TASK(int)
    test_uring::receive_exact(
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
}
