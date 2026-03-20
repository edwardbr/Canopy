/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <atomic>
#  include <thread>
#  include <transports/libcoro_ipc_dynamic_dll/dll_transport.h>

namespace rpc::libcoro_ipc_dynamic_dll
{
    namespace
    {
        struct runtime_context
        {
            std::shared_ptr<coro::scheduler> scheduler;
            std::shared_ptr<std::atomic<bool>> running;
            std::shared_ptr<rpc::event> shutdown_event;
            std::thread worker;
        };

        CORO_TASK(void)
        run_runtime(std::string name,
            rpc::zone dll_zone,
            streaming::spsc_queue::queue_type* send_queue,
            streaming::spsc_queue::queue_type* recv_queue,
            parent_expired_fn on_parent_expired,
            void* callback_ctx,
            std::shared_ptr<coro::scheduler> scheduler,
            std::shared_ptr<rpc::event> shutdown_event,
            std::shared_ptr<std::atomic<bool>> running)
        {
            auto service = std::make_shared<rpc::root_service>(name.c_str(), dll_zone, scheduler);
            service->set_shutdown_event(shutdown_event);

            auto stream = std::make_shared<streaming::spsc_queue::stream>(send_queue, recv_queue, scheduler);
            auto acceptor = CO_AWAIT canopy_libcoro_ipc_dll_init(name, service, std::move(stream));
            if (!acceptor)
            {
                running->store(false, std::memory_order_release);
                CO_RETURN;
            }

            auto accept_err = CO_AWAIT acceptor->accept();
            if (accept_err != rpc::error::OK())
            {
                running->store(false, std::memory_order_release);
                CO_RETURN;
            }

            while (acceptor->get_status() != rpc::transport_status::DISCONNECTED)
                CO_AWAIT scheduler->schedule();

            acceptor.reset();
            service.reset();
            running->store(false, std::memory_order_release);

            if (on_parent_expired)
                on_parent_expired(callback_ctx);
        }

        void stop_runtime(void* raw_ctx)
        {
            auto* ctx = static_cast<runtime_context*>(raw_ctx);
            if (!ctx)
                return;

            if (ctx->shutdown_event)
                ctx->shutdown_event->set();
            if (ctx->worker.joinable())
                ctx->worker.join();
            if (ctx->scheduler)
                ctx->scheduler->shutdown();
            delete ctx;
        }
    }
}

extern "C" CANOPY_LIBCORO_IPC_DLL_EXPORT void canopy_libcoro_ipc_dll_start(
    rpc::libcoro_ipc_dynamic_dll::dll_start_params* params, rpc::libcoro_ipc_dynamic_dll::dll_start_result* result)
{
    using namespace rpc::libcoro_ipc_dynamic_dll;

    auto* ctx = new runtime_context{};
    ctx->scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{
                .thread_count = 1,
            }}));
    ctx->running = std::make_shared<std::atomic<bool>>(true);
    ctx->shutdown_event = std::make_shared<rpc::event>();

    ctx->worker = std::thread(
        [name = std::string(params->name ? params->name : "libcoro_ipc_dynamic_dll"),
            dll_zone = params->dll_zone,
            send_queue = params->send_queue,
            recv_queue = params->recv_queue,
            on_parent_expired = params->on_parent_expired,
            callback_ctx = params->callback_ctx,
            scheduler = ctx->scheduler,
            shutdown_event = ctx->shutdown_event,
            running = ctx->running]() mutable
        {
            RPC_ASSERT(scheduler->spawn_detached(run_runtime(
                std::move(name), dll_zone, send_queue, recv_queue, on_parent_expired, callback_ctx, scheduler, shutdown_event, running)));

            while (running->load(std::memory_order_acquire))
                scheduler->process_events(std::chrono::milliseconds(1));
        });

    result->runtime_ctx = ctx;
    result->stop_fn = &stop_runtime;
    result->error_code = rpc::error::OK();
}

#endif // CANOPY_BUILD_COROUTINE
