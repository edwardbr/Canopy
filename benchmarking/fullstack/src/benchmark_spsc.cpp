/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <streaming/spsc_queue/stream.h>
#  include <transports/streaming/transport.h>

namespace comprehensive::v1
{
    namespace
    {
        struct spsc_queues
        {
            streaming::spsc_queue::queue_type to_process_2;
            streaming::spsc_queue::queue_type to_process_1;
        };

        void pin_spsc_benchmark_queues(const std::shared_ptr<spsc_queues>& queues)
        {
            static auto* pinned_queues = new std::vector<std::shared_ptr<spsc_queues>>();
            pinned_queues->push_back(queues);
        }

        CORO_TASK(void)
        spsc_client_task(
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_1,
            spsc_queues* queues,
            rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            benchmark_result& result)
        {
            CO_AWAIT server_ready.wait();

            auto service = std::make_shared<rpc::root_service>("spsc_client", zone_1, scheduler);
            service->set_default_encoding(enc);

            auto stream_1 = std::make_shared<streaming::spsc_queue::stream>(
                &queues->to_process_1, &queues->to_process_2, scheduler);
            auto transport = rpc::stream_transport::make_client("spsc_transport_1", service, std::move(stream_1));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT service->connect_to_zone<i_data_processor, i_data_processor>(
                "spsc_server", transport, not_used);
            remote_processor = connect_result.output_interface;
            const auto error = connect_result.error_code;
            not_used = nullptr;
            transport.reset();
            service.reset();

            if (error == rpc::error::OK())
            {
                const auto payload = make_blob(blob_size);
                std::vector<int64_t> durations_us;
                result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, spsc_warmup_calls);
                if (result.error == rpc::error::OK())
                    result.stats = compute_stats(durations_us);
            }
            else
            {
                result.error = error;
            }

            remote_processor.reset();
            client_finished.set();
            CO_RETURN;
        }

        CORO_TASK(void)
        spsc_server_task(
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_2,
            spsc_queues* queues,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc)
        {
            auto service = std::make_shared<rpc::root_service>("spsc_server", zone_2, scheduler);
            service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            service->set_shutdown_event(shutdown_event);

            rpc::event on_connected;

            auto stream_2 = std::make_shared<streaming::spsc_queue::stream>(
                &queues->to_process_2, &queues->to_process_1, scheduler);
            auto transport = CO_AWAIT service->make_acceptor<i_data_processor, i_data_processor>(
                "spsc_transport_2",
                rpc::stream_transport::transport_factory(std::move(stream_2)),
                [&on_connected](const rpc::shared_ptr<i_data_processor>&, const std::shared_ptr<rpc::service>&)
                    -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    auto local = make_benchmark_data_processor();
                    on_connected.set();
                    CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local)};
                });

            server_ready.set();
            CO_AWAIT transport->accept();

            CO_AWAIT on_connected.wait();
            CO_AWAIT client_finished.wait();
            transport.reset();
            service.reset();
            CO_AWAIT shutdown_event->wait();
            CO_RETURN;
        }
    }

    benchmark_result run_spsc_benchmark(
        rpc::encoding enc,
        size_t blob_size)
    {
        benchmark_result result{};
        auto zone_1 = rpc::DEFAULT_PREFIX;
        auto zone_2 = rpc::DEFAULT_PREFIX;
        std::ignore = zone_2.set_subnet(zone_2.get_subnet() + 1);
        auto queues = std::make_shared<spsc_queues>();
        pin_spsc_benchmark_queues(queues);

        auto scheduler_1 = make_benchmark_scheduler(1);
        auto scheduler_2 = make_benchmark_scheduler(1);
        auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
        auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);
        rpc::event server_ready;
        rpc::event client_finished;

        coro::sync_wait(
            coro::when_all(
                spsc_client_task(scheduler_1, zone_1, queues.get(), server_ready, client_finished, enc, blob_size, result),
                spsc_server_task(scheduler_2, zone_2, queues.get(), server_ready, client_finished, enc)));

        scheduler_1.reset();
        scheduler_2.reset();
        wait_for_scheduler_cleanup(weak_scheduler_1);
        wait_for_scheduler_cleanup(weak_scheduler_2);

        return result;
    }
}

#endif
