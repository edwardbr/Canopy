/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include <atomic>
#  include "benchmark_data_processor.h"
#  include <streaming/tcp/stream.h>
#  include <transports/streaming/transport.h>

namespace comprehensive::v1
{
    namespace
    {
        rpc::zone_address make_client_zone_address()
        {
            return rpc::zone_address(rpc::zone_address::construction_args(
                rpc::zone_address::version_3, rpc::zone_address::address_type::local, 0, {}, rpc::zone_address::default_local_subnet_size_bits, 2,
                rpc::zone_address::get_default_local_object_id_size_bits(), 1, {}));
        }

        CORO_TASK(void)
        tcp_server_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            std::atomic<bool>& server_started,
            rpc::encoding enc,
            uint16_t port)
        {
            auto service = std::make_shared<rpc::root_service>("tcp_server", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);
            coro::net::tcp::server server(scheduler, coro::net::socket_address{"127.0.0.1", port});
            server_started.store(true, std::memory_order_release);
            server_ready.set();

            auto accepted = CO_AWAIT server.accept(std::chrono::milliseconds(5000));
            if (!accepted)
            {
                std::cerr << "tcp_server: accept failed\n";
                CO_RETURN;
            }

            auto tcp_stream = std::make_shared<streaming::tcp::stream>(std::move(*accepted), scheduler);
            auto server_transport
                = CO_AWAIT service->make_acceptor<i_data_processor, i_data_processor>("server_transport",
                    rpc::stream_transport::transport_factory(std::move(tcp_stream)),
                    [](const rpc::shared_ptr<i_data_processor>&,
                        const std::shared_ptr<rpc::service>&) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                    {
                        auto local = make_benchmark_data_processor();
                        CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local)};
                    });

            CO_AWAIT server_transport->accept();
            CO_AWAIT client_finished.wait();
            server_transport.reset();
            service.reset();
        }

        CORO_TASK(void)
        tcp_client_task(std::shared_ptr<coro::scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            std::atomic<bool>& server_started,
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            benchmark_result& result)
        {
            CO_AWAIT server_ready.wait();

            if (!server_started.load(std::memory_order_acquire))
            {
                std::cerr << "tcp_client: server did not start\n";
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_finished.set();
                CO_RETURN;
            }

            auto client_service = std::make_shared<rpc::root_service>("tcp_client", make_client_zone_address(), scheduler);
            client_service->set_default_encoding(enc);

            coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", port});
            const auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
            if (connection_status != coro::net::connect_status::connected)
            {
                std::cerr << "tcp_client: connect failed with status " << static_cast<int>(connection_status) << '\n';
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_finished.set();
                CO_RETURN;
            }

            auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
            auto client_transport
                = rpc::stream_transport::make_client("client_transport", client_service, std::move(tcp_stm));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT client_service->connect_to_zone<i_data_processor, i_data_processor>(
                "tcp_server", client_transport, not_used);
            remote_processor = connect_result.output_interface;
            const auto error = connect_result.error_code;
            not_used = nullptr;

            if (error != rpc::error::OK())
            {
                std::cerr << "tcp_client: connect_to_zone failed with error " << error << '\n';
                result.error = error;
                client_finished.set();
                CO_RETURN;
            }

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, tcp_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_us);

            remote_processor.reset();
            client_finished.set();
            client_transport.reset();
            client_service.reset();
        }
    }

    benchmark_result run_tcp_benchmark(rpc::encoding enc, size_t blob_size, uint16_t port)
    {
        benchmark_result result{};

        auto scheduler_1 = make_benchmark_scheduler();
        auto scheduler_2 = make_benchmark_scheduler();
        auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
        auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);

        rpc::event server_ready;
        rpc::event client_finished;
        std::atomic<bool> server_started = false;

        coro::sync_wait(coro::when_all(tcp_server_task(scheduler_1, server_ready, client_finished, server_started, enc, port),
            tcp_client_task(scheduler_2, server_ready, client_finished, server_started, enc, blob_size, port, result)));

        scheduler_1.reset();
        scheduler_2.reset();
        wait_for_scheduler_cleanup(weak_scheduler_1);
        wait_for_scheduler_cleanup(weak_scheduler_2);

        return result;
    }
}

#endif
