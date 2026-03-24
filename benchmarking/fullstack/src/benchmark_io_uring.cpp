/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <streaming/io_uring/acceptor.h>
#  include <streaming/io_uring/stream.h>
#  include <streaming/listener.h>
#  include <transports/streaming/transport.h>

namespace comprehensive::v1
{
    namespace
    {
        rpc::zone_address make_client_zone_address()
        {
            return *rpc::zone_address::create(rpc::zone_address::construction_args(rpc::zone_address::version_3,
                rpc::zone_address::address_type::local,
                0,
                {},
                rpc::zone_address::default_subnet_size_bits,
                2,
                rpc::zone_address::default_object_id_size_bits,
                1,
                {}));
        }

        CORO_TASK(void)
        io_uring_server_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc,
            uint16_t port)
        {
            auto service = std::make_shared<rpc::root_service>("io_uring_server", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            service->set_shutdown_event(shutdown_event);

            canopy::network_config::ip_address addr{};
            addr[0] = 127;
            addr[1] = 0;
            addr[2] = 0;
            addr[3] = 1;
            auto io_uring_listener = std::make_shared<streaming::listener>("io_uring_server_transport",
                std::make_shared<streaming::io_uring::acceptor>(addr, port),
                rpc::stream_transport::make_connection_callback<i_data_processor, i_data_processor>(
                    [](const rpc::shared_ptr<i_data_processor>&,
                        const std::shared_ptr<rpc::service>&) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                    {
                        auto local_service = make_benchmark_data_processor();
                        CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local_service)};
                    }));
            auto started = CO_AWAIT io_uring_listener->start_listening_async(service);
            server_ready.set();

            CO_AWAIT client_finished.wait();
            CO_AWAIT io_uring_listener->stop_listening();
            io_uring_listener.reset();
            service.reset();
            CO_AWAIT shutdown_event->wait();
        }

        CORO_TASK(void)
        io_uring_client_task(std::shared_ptr<coro::scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            benchmark_result& result)
        {
            CO_AWAIT server_ready.wait();

            auto client_service
                = std::make_shared<rpc::root_service>("io_uring_client", make_client_zone_address(), scheduler);
            client_service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            client_service->set_shutdown_event(shutdown_event);

            coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", port});
            const auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
            if (connection_status != coro::net::connect_status::connected)
            {
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_finished.set();
                CO_RETURN;
            }

            auto stm = std::make_shared<streaming::io_uring::stream>(std::move(client), scheduler);
            auto client_transport
                = rpc::stream_transport::make_client("io_uring_client_transport", client_service, std::move(stm));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;
            auto error = rpc::error::OK();
            {
                auto connect_result = CO_AWAIT client_service->connect_to_zone<i_data_processor, i_data_processor>(
                    "io_uring_server", client_transport, not_used);
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

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, io_uring_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_us);

            remote_processor.reset();
            client_finished.set();
            client_transport.reset();
            client_service.reset();
            CO_AWAIT shutdown_event->wait();
        }
    }

    benchmark_result run_io_uring_benchmark(rpc::encoding enc, size_t blob_size, uint16_t port)
    {
        benchmark_result result{};

        auto scheduler_1 = make_benchmark_scheduler();
        auto scheduler_2 = make_benchmark_scheduler();
        auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
        auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);
        rpc::event server_ready;
        rpc::event client_finished;

        coro::sync_wait(coro::when_all(io_uring_server_task(scheduler_1, server_ready, client_finished, enc, port),
            io_uring_client_task(scheduler_2, server_ready, client_finished, enc, blob_size, port, result)));

        scheduler_1.reset();
        scheduler_2.reset();
        wait_for_scheduler_cleanup(weak_scheduler_1);
        wait_for_scheduler_cleanup(weak_scheduler_2);

        return result;
    }
}

#endif
