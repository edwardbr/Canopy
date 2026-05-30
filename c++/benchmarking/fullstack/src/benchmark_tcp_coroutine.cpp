/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <io_uring/host_io_uring.h>
#  include <streaming/tcp_coroutine/acceptor.h>
#  include <streaming/tcp_coroutine/connector.h>
#  include <transports/streaming/transport.h>

namespace comprehensive::v1
{
    namespace
    {
        rpc::zone_address make_client_zone_address()
        {
            return *rpc::zone_address::create(
                rpc::zone_address_args(
                    rpc::default_values::version_3,
                    rpc::address_type::local,
                    0,
                    {},
                    rpc::default_values::default_subnet_size_bits,
                    2,
                    rpc::default_values::default_object_id_size_bits,
                    1,
                    {}));
        }

        rpc::io_uring::linux_io_uring_handle::options benchmark_tcp_coroutine_io_options(uint32_t host_buffer_size)
        {
            rpc::io_uring::linux_io_uring_handle::options options;
            options.queue_depth = 256;
            options.use_sqpoll = true;
            options.buffer_count = 256;
            options.buffer_size = host_buffer_size;
            options.register_buffers = false;
            options.fixed_file_count = 128;
            options.register_fixed_files = true;
            return options;
        }

        CORO_TASK(void)
        wait_for_transport_disconnect(
            std::shared_ptr<coro::scheduler> scheduler,
            std::shared_ptr<rpc::stream_transport::transport> transport,
            benchmark_result& result)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
            while (transport && transport->get_status() < rpc::transport_status::DISCONNECTED)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    if (result.error == rpc::error::OK())
                        result.error = rpc::error::CALL_TIMEOUT();
                    CO_RETURN;
                }

                CO_AWAIT scheduler->schedule();
            }
        }

        CORO_TASK(void)
        tcp_coroutine_server_task(
            std::shared_ptr<coro::scheduler> scheduler,
            std::shared_ptr<rpc::io_uring::controller> controller,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc,
            uint16_t port)
        {
            auto service = rpc::root_service::create("tcp_coroutine_server", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            service->set_shutdown_event(shutdown_event);

            auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(controller);
            auto listen_result = CO_AWAIT acceptor->listen_loopback(port);
            if (listen_result != rpc::error::OK())
            {
                server_ready.set();
                service.reset();
                CO_AWAIT shutdown_event->wait();
                CO_RETURN;
            }

            server_ready.set();

            auto maybe_stream = CO_AWAIT acceptor->accept();
            if (!maybe_stream)
            {
                service.reset();
                CO_AWAIT shutdown_event->wait();
                CO_RETURN;
            }

            auto server_transport = CO_AWAIT service->make_acceptor<i_data_processor, i_data_processor>(
                "tcp_coroutine_server_transport",
                rpc::stream_transport::transport_factory(std::move(*maybe_stream)),
                [](const rpc::shared_ptr<i_data_processor>&,
                    const std::shared_ptr<rpc::service>&) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    auto local_service = make_benchmark_data_processor();
                    CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local_service)};
                });

            CO_AWAIT server_transport->accept();
            controller->set_wait_strategy(measured_wait_strategy);
            CO_AWAIT client_finished.wait();
            server_transport.reset();
            acceptor->stop();
            acceptor.reset();
            service.reset();
            CO_AWAIT shutdown_event->wait();
        }

        CORO_TASK(void)
        tcp_coroutine_client_task(
            std::shared_ptr<coro::scheduler> scheduler,
            std::shared_ptr<rpc::io_uring::controller> controller,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            benchmark_result& result)
        {
            CO_AWAIT server_ready.wait();

            auto client_service = rpc::root_service::create("tcp_coroutine_client", make_client_zone_address(), scheduler);
            client_service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            client_service->set_shutdown_event(shutdown_event);

            auto stream_result = CO_AWAIT streaming::coroutine::tcp::connect_loopback(controller, port);
            if (stream_result.error_code != rpc::error::OK() || !stream_result.connection)
            {
                result.error = stream_result.error_code == rpc::error::OK() ? rpc::error::ZONE_NOT_FOUND()
                                                                            : stream_result.error_code;
                client_finished.set();
                CO_RETURN;
            }

            auto client_transport = rpc::stream_transport::make_client(
                "tcp_coroutine_client_transport", client_service, std::move(stream_result.connection));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;
            auto error = rpc::error::OK();
            {
                auto connect_result = CO_AWAIT client_service->connect_to_zone<i_data_processor, i_data_processor>(
                    "tcp_coroutine_server", client_transport, not_used);
                remote_processor = connect_result.output_interface;
                error = connect_result.error_code;
            }
            not_used = nullptr;

            if (error != rpc::error::OK())
            {
                result.error = error;
                client_finished.set();
                client_transport.reset();
                client_service.reset();
                CO_AWAIT shutdown_event->wait();
                CO_RETURN;
            }

            controller->set_wait_strategy(measured_wait_strategy);
            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_ns;
            result.error
                = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_ns, tcp_coroutine_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_ns);

            remote_processor.reset();
            CO_AWAIT wait_for_transport_disconnect(scheduler, client_transport, result);
            client_finished.set();
            client_transport.reset();
            client_service.reset();
            CO_AWAIT shutdown_event->wait();
        }
    }

    benchmark_result run_tcp_coroutine_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port,
        bool use_proactor,
        uint32_t host_buffer_size)
    {
        benchmark_result result{};

        auto scheduler_1 = make_benchmark_scheduler();
        auto scheduler_2 = make_benchmark_scheduler();
        auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
        auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);
        std::shared_ptr<rpc::io_uring::io_uring_scheduler> io_uring_owner_1;
        std::shared_ptr<rpc::io_uring::io_uring_scheduler> io_uring_owner_2;
        const auto handle_options = benchmark_tcp_coroutine_io_options(host_buffer_size);
        const auto setup_controller_options = rpc::io_uring::default_controller_options();
        const auto measured_wait_strategy
            = use_proactor ? rpc::io_uring::wait_strategy::proactor : rpc::io_uring::wait_strategy::cooperative_poll;

        result.error
            = rpc::io_uring::create_scheduler(io_uring_owner_1, handle_options, scheduler_1, setup_controller_options);
        if (result.error != rpc::error::OK())
            return result;

        result.error
            = rpc::io_uring::create_scheduler(io_uring_owner_2, handle_options, scheduler_2, setup_controller_options);
        if (result.error != rpc::error::OK())
            return result;

        auto server_controller = io_uring_owner_1->get_controller();
        auto client_controller = io_uring_owner_2->get_controller();
        if (!server_controller || !client_controller)
        {
            result.error = rpc::error::RESOURCE_CLOSED();
            return result;
        }

        rpc::event server_ready;
        rpc::event client_finished;

        coro::sync_wait(
            coro::when_all(
                tcp_coroutine_server_task(
                    scheduler_1, server_controller, measured_wait_strategy, server_ready, client_finished, enc, port),
                tcp_coroutine_client_task(
                    scheduler_2, client_controller, measured_wait_strategy, server_ready, client_finished, enc, blob_size, port, result)));

        io_uring_owner_1->shutdown();
        io_uring_owner_2->shutdown();
        io_uring_owner_1.reset();
        io_uring_owner_2.reset();
        scheduler_1.reset();
        scheduler_2.reset();
        wait_for_scheduler_cleanup(weak_scheduler_1);
        wait_for_scheduler_cleanup(weak_scheduler_2);

        return result;
    }
}

#endif
