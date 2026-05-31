/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// DLL-side (parent_transport) implementation.  Coroutine-build only.
//
// Compiled into transport_unshared_scheduler_dll_runtime, which the DLL links.
// The DLL author only needs to provide canopy_unshared_scheduler_dll_init.

#ifdef CANOPY_BUILD_COROUTINE

#  ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
#    include <google/protobuf/stubs/common.h>
#  endif

#  include <transports/unshared_scheduler_dll/dll_transport.h>
#  include <rpc/rpc.h>

#  include <algorithm>
#  include <coroutine>
#  include <exception>
#  include <mutex>
#  include <thread>

namespace rpc::unshared_scheduler_dll
{
    namespace
    {
        std::shared_ptr<coro::scheduler> make_runtime_scheduler()
        {
            auto thread_count = std::thread::hardware_concurrency();
            if (thread_count == 0)
                thread_count = 2;

            return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = std::max<unsigned int>(2, thread_count)},
                    .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
        }

        std::shared_ptr<parent_transport> lock_transport(runtime_context& runtime)
        {
            auto transport = runtime.transport.lock();
            if (transport)
                return transport;
            return runtime.pending_transport;
        }

        template<typename Result> struct pending_result_state
        {
            std::mutex mutex;
            std::shared_ptr<coro::scheduler> scheduler;
            std::coroutine_handle<> continuation;
            bool done = false;
            Result result;

            pending_result_state(
                std::shared_ptr<coro::scheduler> input_scheduler,
                Result failure_result)
                : scheduler(std::move(input_scheduler))
                , result(std::move(failure_result))
            {
            }

            void complete(Result value)
            {
                std::coroutine_handle<> resume;
                {
                    std::lock_guard lock(mutex);
                    result = std::move(value);
                    done = true;
                    resume = continuation;
                }

                if (resume)
                {
                    if (!scheduler || !scheduler->resume(resume))
                        resume.resume();
                }
            }
        };

        struct pending_void_state
        {
            std::mutex mutex;
            std::shared_ptr<coro::scheduler> scheduler;
            std::coroutine_handle<> continuation;
            bool done = false;

            explicit pending_void_state(std::shared_ptr<coro::scheduler> input_scheduler)
                : scheduler(std::move(input_scheduler))
            {
            }

            void complete()
            {
                std::coroutine_handle<> resume;
                {
                    std::lock_guard lock(mutex);
                    done = true;
                    resume = continuation;
                }

                if (resume)
                {
                    if (!scheduler || !scheduler->resume(resume))
                        resume.resume();
                }
            }
        };

        template<typename Result> class pending_result_awaitable
        {
            std::shared_ptr<pending_result_state<Result>> state_;

        public:
            explicit pending_result_awaitable(std::shared_ptr<pending_result_state<Result>> state)
                : state_(std::move(state))
            {
            }

            bool await_ready() const
            {
                std::lock_guard lock(state_->mutex);
                return state_->done;
            }

            bool await_suspend(std::coroutine_handle<> handle)
            {
                std::lock_guard lock(state_->mutex);
                if (state_->done)
                    return false;
                state_->continuation = handle;
                return true;
            }

            Result await_resume()
            {
                std::lock_guard lock(state_->mutex);
                return std::move(state_->result);
            }
        };

        class pending_void_awaitable
        {
            std::shared_ptr<pending_void_state> state_;

        public:
            explicit pending_void_awaitable(std::shared_ptr<pending_void_state> state)
                : state_(std::move(state))
            {
            }

            bool await_ready() const
            {
                std::lock_guard lock(state_->mutex);
                return state_->done;
            }

            bool await_suspend(std::coroutine_handle<> handle)
            {
                std::lock_guard lock(state_->mutex);
                if (state_->done)
                    return false;
                state_->continuation = handle;
                return true;
            }

            void await_resume() const { }
        };

        void complete_send(
            void* ctx,
            rpc::send_result* result)
        {
            auto* state = static_cast<pending_result_state<rpc::send_result>*>(ctx);
            if (state)
                state->complete(result ? std::move(*result) : rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}});
        }

        void complete_standard(
            void* ctx,
            rpc::standard_result* result)
        {
            auto* state = static_cast<pending_result_state<rpc::standard_result>*>(ctx);
            if (state)
                state->complete(result ? std::move(*result) : rpc::standard_result{rpc::error::TRANSPORT_ERROR(), {}});
        }

        void complete_new_zone_id(
            void* ctx,
            rpc::new_zone_id_result* result)
        {
            auto* state = static_cast<pending_result_state<rpc::new_zone_id_result>*>(ctx);
            if (state)
                state->complete(
                    result ? std::move(*result) : rpc::new_zone_id_result{rpc::error::TRANSPORT_ERROR(), {}, {}});
        }

        void complete_void(void* ctx)
        {
            auto* state = static_cast<pending_void_state*>(ctx);
            if (state)
                state->complete();
        }

    } // namespace

    void runtime_finish_gate::finish()
    {
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                return;
            finished_ = true;
        }
        cv_.notify_all();
    }

    void runtime_finish_gate::wait()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this]() { return finished_; });
    }

    parent_transport::parent_transport(
        std::string name,
        rpc::zone dll_zone,
        rpc::zone host_zone,
        void* host_ctx,
        host_begin_send_fn send,
        host_begin_post_fn post,
        host_begin_try_cast_fn try_cast,
        host_begin_add_ref_fn add_ref,
        host_begin_release_fn release,
        host_begin_object_released_fn object_released,
        host_begin_transport_down_fn transport_down,
        host_begin_get_new_zone_id_fn get_new_zone_id,
        host_coro_release_parent_fn host_coro_release_parent,
        std::weak_ptr<coro::scheduler> scheduler,
        std::weak_ptr<runtime_finish_gate> runtime_finished)
        : rpc::transport(
              name,
              dll_zone)
        , host_ctx_(host_ctx)
        , host_send_(send)
        , host_post_(post)
        , host_try_cast_(try_cast)
        , host_add_ref_(add_ref)
        , host_release_(release)
        , host_object_released_(object_released)
        , host_transport_down_(transport_down)
        , host_get_new_zone_id_(get_new_zone_id)
        , host_coro_release_parent_(host_coro_release_parent)
        , scheduler_(std::move(scheduler))
        , runtime_finished_(std::move(runtime_finished))
    {
        set_adjacent_zone_id(host_zone);
        set_status(rpc::transport_status::CONNECTED);
    }

    parent_transport::~parent_transport()
    {
        if (host_coro_release_parent_)
            host_coro_release_parent_(host_ctx_);
        if (auto finished = runtime_finished_.lock())
            finished->finish();
    }

    CORO_TASK(send_result)
    parent_transport::outbound_send(send_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_send_ || !host_ctx_ || !scheduler)
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        auto state = std::make_shared<pending_result_state<send_result>>(
            scheduler, send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}});
        auto err = host_send_(host_ctx_, std::move(params), state.get(), &complete_send);
        if (err != rpc::error::OK())
            CO_RETURN send_result{err, {}, {}};

        auto result = CO_AWAIT pending_result_awaitable<send_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(void)
    parent_transport::outbound_post(post_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_post_ || !host_ctx_ || !scheduler)
            CO_RETURN;

        auto state = std::make_shared<pending_void_state>(scheduler);
        auto err = host_post_(host_ctx_, std::move(params), state.get(), &complete_void);
        if (err != rpc::error::OK())
            CO_RETURN;

        CO_AWAIT pending_void_awaitable(std::move(state));
        CO_AWAIT scheduler->schedule();
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_try_cast(try_cast_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_try_cast_ || !host_ctx_ || !scheduler)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        auto state = std::make_shared<pending_result_state<standard_result>>(
            scheduler, standard_result{rpc::error::ZONE_NOT_FOUND(), {}});
        auto err = host_try_cast_(host_ctx_, std::move(params), state.get(), &complete_standard);
        if (err != rpc::error::OK())
            CO_RETURN standard_result{err, {}};

        auto result = CO_AWAIT pending_result_awaitable<standard_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_add_ref(add_ref_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_add_ref_ || !host_ctx_ || !scheduler)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        auto state = std::make_shared<pending_result_state<standard_result>>(
            scheduler, standard_result{rpc::error::ZONE_NOT_FOUND(), {}});
        auto err = host_add_ref_(host_ctx_, std::move(params), state.get(), &complete_standard);
        if (err != rpc::error::OK())
            CO_RETURN standard_result{err, {}};

        auto result = CO_AWAIT pending_result_awaitable<standard_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_release(release_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_release_ || !host_ctx_ || !scheduler)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        auto state = std::make_shared<pending_result_state<standard_result>>(
            scheduler, standard_result{rpc::error::ZONE_NOT_FOUND(), {}});
        auto err = host_release_(host_ctx_, std::move(params), state.get(), &complete_standard);
        if (err != rpc::error::OK())
            CO_RETURN standard_result{err, {}};

        auto result = CO_AWAIT pending_result_awaitable<standard_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(void)
    parent_transport::outbound_object_released(object_released_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_object_released_ || !host_ctx_ || !scheduler)
            CO_RETURN;

        auto state = std::make_shared<pending_void_state>(scheduler);
        auto err = host_object_released_(host_ctx_, std::move(params), state.get(), &complete_void);
        if (err != rpc::error::OK())
            CO_RETURN;

        CO_AWAIT pending_void_awaitable(std::move(state));
        CO_AWAIT scheduler->schedule();
    }

    CORO_TASK(void)
    parent_transport::outbound_transport_down(transport_down_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_transport_down_ || !host_ctx_ || !scheduler)
            CO_RETURN;

        auto state = std::make_shared<pending_void_state>(scheduler);
        auto err = host_transport_down_(host_ctx_, std::move(params), state.get(), &complete_void);
        if (err != rpc::error::OK())
            CO_RETURN;

        CO_AWAIT pending_void_awaitable(std::move(state));
        CO_AWAIT scheduler->schedule();
    }

    CORO_TASK(new_zone_id_result)
    parent_transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        auto scheduler = scheduler_.lock();
        if (!host_get_new_zone_id_ || !host_ctx_ || !scheduler)
            CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        auto state = std::make_shared<pending_result_state<new_zone_id_result>>(
            scheduler, new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}});
        auto err = host_get_new_zone_id_(host_ctx_, std::move(params), state.get(), &complete_new_zone_id);
        if (err != rpc::error::OK())
            CO_RETURN new_zone_id_result{err, {}, {}};

        auto result = CO_AWAIT pending_result_awaitable<new_zone_id_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(rpc::connect_result)
    init_runtime(
        void* raw_ctx,
        const rpc::connection_settings* settings)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || !settings || runtime->destroyed.load(std::memory_order::acquire))
            CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};

        auto scheduler = runtime->scheduler;
        CO_AWAIT scheduler->schedule();
        CO_RETURN CO_AWAIT canopy_unshared_scheduler_dll_init(runtime, settings);
    }

    CORO_TASK(void)
    complete_inbound_send(
        runtime_context* runtime,
        send_params params,
        void* completion_ctx,
        complete_send_fn complete)
    {
        auto result = send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                result = CO_AWAIT transport->inbound_send(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    complete_inbound_post(
        runtime_context* runtime,
        post_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                CO_AWAIT transport->inbound_post(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx);
    }

    CORO_TASK(void)
    complete_inbound_try_cast(
        runtime_context* runtime,
        try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto result = standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                result = CO_AWAIT transport->inbound_try_cast(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    complete_inbound_add_ref(
        runtime_context* runtime,
        add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto result = standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                result = CO_AWAIT transport->inbound_add_ref(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    complete_inbound_release(
        runtime_context* runtime,
        release_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto result = standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                result = CO_AWAIT transport->inbound_release(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    complete_inbound_object_released(
        runtime_context* runtime,
        object_released_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                CO_AWAIT transport->inbound_object_released(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx);
    }

    CORO_TASK(void)
    complete_inbound_transport_down(
        runtime_context* runtime,
        transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        if (runtime && runtime->scheduler && !runtime->destroyed.load(std::memory_order::acquire))
        {
            auto scheduler = runtime->scheduler;
            CO_AWAIT scheduler->schedule();
            if (auto transport = lock_transport(*runtime))
                CO_AWAIT transport->inbound_transport_down(std::move(params));
            CO_AWAIT scheduler->schedule();
        }

        if (complete)
            complete(completion_ctx);
    }

    int begin_inbound_send(
        void* raw_ctx,
        send_params params,
        void* completion_ctx,
        complete_send_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_send(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int begin_inbound_post(
        void* raw_ctx,
        post_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_post(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int begin_inbound_try_cast(
        void* raw_ctx,
        try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_try_cast(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int begin_inbound_add_ref(
        void* raw_ctx,
        add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_add_ref(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int begin_inbound_release(
        void* raw_ctx,
        release_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_release(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int begin_inbound_object_released(
        void* raw_ctx,
        object_released_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_object_released(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int begin_inbound_transport_down(
        void* raw_ctx,
        transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto* runtime = static_cast<runtime_context*>(raw_ctx);
        if (!runtime || !runtime->scheduler || runtime->destroyed.load(std::memory_order::acquire))
            return rpc::error::ZONE_NOT_FOUND();
        return runtime->scheduler->spawn_detached(
                   complete_inbound_transport_down(runtime, std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    void shutdown_runtime(runtime_context& runtime)
    {
        if (runtime.destroyed.exchange(true, std::memory_order::acq_rel))
            return;

        runtime.pending_transport.reset();
        runtime.transport.reset();
        if (runtime.scheduler)
        {
            runtime.scheduler->shutdown();
            runtime.scheduler.reset();
        }
        runtime.runtime_finished.reset();
    }

} // namespace rpc::unshared_scheduler_dll

extern "C" CANOPY_UNSHARED_SCHEDULER_DLL_EXPORT void canopy_unshared_scheduler_dll_start(
    rpc::unshared_scheduler_dll::dll_start_params* params)
{
    using namespace rpc::unshared_scheduler_dll;

    auto runtime = std::make_unique<runtime_context>();
    dll_start_result start_result{};

    try
    {
        if (!params || !params->settings || !params->ready)
        {
            start_result.connect_result = rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
        }
        else
        {
            runtime->scheduler = make_runtime_scheduler();
            runtime->runtime_finished = std::make_shared<runtime_finish_gate>();
            runtime->pending_transport = std::make_shared<parent_transport>(
                params->name ? params->name : "",
                params->dll_zone,
                params->host_zone,
                params->host_ctx,
                params->host_send,
                params->host_post,
                params->host_try_cast,
                params->host_add_ref,
                params->host_release,
                params->host_object_released,
                params->host_transport_down,
                params->host_get_new_zone_id,
                params->host_coro_release_parent,
                runtime->scheduler,
                runtime->runtime_finished);

            start_result.runtime_ctx = runtime.get();
            start_result.begin.send = &begin_inbound_send;
            start_result.begin.post = &begin_inbound_post;
            start_result.begin.try_cast = &begin_inbound_try_cast;
            start_result.begin.add_ref = &begin_inbound_add_ref;
            start_result.begin.release = &begin_inbound_release;
            start_result.begin.object_released = &begin_inbound_object_released;
            start_result.begin.transport_down = &begin_inbound_transport_down;
            start_result.connect_result = coro::sync_wait(init_runtime(runtime.get(), params->settings));
        }
    }
    catch (...)
    {
        RPC_ERROR("[unshared_scheduler_dll] DLL start threw during initialisation");
        start_result.connect_result = rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
    }

    if (params && params->ready)
        params->ready(params->ready_ctx, &start_result);

    if (start_result.connect_result.error_code == rpc::error::OK() && runtime->runtime_finished)
        runtime->runtime_finished->wait();

    shutdown_runtime(*runtime);

#  ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
    google::protobuf::ShutdownProtobufLibrary();
#  endif
}

#endif // CANOPY_BUILD_COROUTINE
