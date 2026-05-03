/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Host-side (child_transport) implementation for the libcoro_dll_scheduled_dynamic_library
// transport.  Coroutine-build only.

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/libcoro_dll_scheduled_dynamic_library/transport.h>
#  include <rpc/rpc.h>

#  include <coro/scheduler.hpp>

#  include <coroutine>
#  include <exception>
#  include <mutex>
#  include <thread>
#  include <utility>

#  if defined(_WIN32)
#    include <windows.h>
#  else
#    include <dlfcn.h>
#  endif

namespace rpc::libcoro_dll_scheduled_dynamic_library
{
    namespace
    {
        struct ready_state
        {
            std::mutex mutex;
            std::shared_ptr<coro::scheduler> scheduler;
            std::coroutine_handle<> continuation;
            bool done = false;

            explicit ready_state(std::shared_ptr<coro::scheduler> input_scheduler)
                : scheduler(std::move(input_scheduler))
            {
            }

            void complete()
            {
                std::coroutine_handle<> resume;
                std::shared_ptr<coro::scheduler> resume_scheduler;
                {
                    std::lock_guard lock(mutex);
                    if (done)
                        return;
                    done = true;
                    resume = continuation;
                    resume_scheduler = scheduler;
                }

                if (resume)
                {
                    if (!resume_scheduler || !resume_scheduler->resume(resume))
                        resume.resume();
                }
            }
        };

        struct ready_awaitable
        {
            std::shared_ptr<ready_state> state;

            bool await_ready() const
            {
                std::lock_guard lock(state->mutex);
                return state->done;
            }

            bool await_suspend(std::coroutine_handle<> handle)
            {
                std::lock_guard lock(state->mutex);
                if (state->done)
                    return false;

                state->continuation = handle;
                return true;
            }

            void await_resume() const { std::lock_guard lock(state->mutex); }
        };
    } // namespace

    struct loaded_runtime
    {
        loaded_runtime(
            std::string input_library_path,
            std::shared_ptr<coro::scheduler> input_scheduler)
            : library_path(std::move(input_library_path))
            , ready(std::make_shared<ready_state>(std::move(input_scheduler)))
        {
        }

        std::string library_path;
        std::string transport_name;
        rpc::connection_settings settings;
        std::shared_ptr<ready_state> ready;
        std::thread entry_thread;
        std::mutex mutex;
        dll_start_result start_result;
        bool ready_published = false;
        void* lib_handle = nullptr;
    };

    namespace
    {
        void* load_native_library(const std::string& path)
        {
#  if defined(_WIN32)
            return static_cast<void*>(LoadLibraryA(path.c_str()));
#  else
            return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#  endif
        }

        void close_native_library(void* handle)
        {
            if (!handle)
                return;
#  if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(handle));
#  else
            dlclose(handle);
#  endif
        }

        void* resolve_native_symbol(
            void* handle,
            const char* name)
        {
            if (!handle)
                return nullptr;
#  if defined(_WIN32)
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#  else
            return dlsym(handle, name);
#  endif
        }

#  ifdef CANOPY_USE_LOGGING
        const char* load_error_text()
        {
#    if defined(_WIN32)
            return "LoadLibrary/GetProcAddress failed";
#    else
            auto* error = dlerror();
            return error ? error : "dlopen/dlsym failed";
#    endif
        }
#  endif

        void publish_ready(
            loaded_runtime& loaded,
            dll_start_result result)
        {
            bool should_notify = false;
            {
                std::lock_guard lock(loaded.mutex);
                if (!loaded.ready_published)
                {
                    loaded.start_result = std::move(result);
                    loaded.ready_published = true;
                    should_notify = true;
                }
            }

            if (should_notify && loaded.ready)
                loaded.ready->complete();
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

        void complete_void(void* ctx)
        {
            auto* state = static_cast<pending_void_state*>(ctx);
            if (state)
                state->complete();
        }
    } // namespace

    child_transport::child_transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::string library_path)
        : rpc::transport(
              name,
              service)
        , library_path_(std::move(library_path))
    {
    }

    child_transport::~child_transport()
    {
        unload_library();
    }

    void child_transport::dll_ready(
        void* ctx,
        dll_start_result* result)
    {
        auto* loaded = static_cast<loaded_runtime*>(ctx);
        if (!loaded || !result)
            return;
        publish_ready(*loaded, *result);
    }

    void child_transport::unload_library()
    {
        dll_ctx_ = nullptr;
        dll_begin_ = {};

        auto loaded = std::move(loaded_);
        if (!loaded)
            return;

        if (loaded->entry_thread.joinable())
            loaded->entry_thread.join();

        close_native_library(loaded->lib_handle);
        loaded->lib_handle = nullptr;
    }

    void child_transport::schedule_parent_release()
    {
        bool expected = false;
        if (!release_task_scheduled_.compare_exchange_strong(expected, true, std::memory_order::acq_rel))
            return;

        auto scheduler = scheduler_.lock();
        if (!scheduler)
        {
            release_task_scheduled_.store(false, std::memory_order::release);
            return;
        }

        if (!scheduler->spawn_detached(release_parent_after_dll_callback(std::move(scheduler))))
            release_task_scheduled_.store(false, std::memory_order::release);
    }

    CORO_TASK(void)
    child_transport::release_parent_after_dll_callback(std::shared_ptr<coro::scheduler> scheduler)
    {
        auto self = std::static_pointer_cast<child_transport>(shared_from_this());

        CO_AWAIT scheduler->schedule();
        keep_alive_.reset();
        release_task_scheduled_.store(false, std::memory_order::release);
    }

    int child_transport::host_begin_send(
        void* ctx,
        send_params params,
        void* completion_ctx,
        complete_send_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_send(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_post(
        void* ctx,
        post_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_post(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_try_cast(
        void* ctx,
        try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_try_cast(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_add_ref(
        void* ctx,
        add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_add_ref(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_release(
        void* ctx,
        release_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_release(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_object_released(
        void* ctx,
        object_released_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_object_released(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_transport_down(
        void* ctx,
        transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_transport_down(std::move(params), completion_ctx, complete);
    }

    int child_transport::host_begin_get_new_zone_id(
        void* ctx,
        get_new_zone_id_params params,
        void* completion_ctx,
        complete_new_zone_id_fn complete)
    {
        return static_cast<child_transport*>(ctx)->begin_host_get_new_zone_id(std::move(params), completion_ctx, complete);
    }

    void child_transport::host_coro_release_parent(void* ctx)
    {
        static_cast<child_transport*>(ctx)->schedule_parent_release();
    }

    int child_transport::begin_host_send(
        send_params params,
        void* completion_ctx,
        complete_send_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_send(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_post(
        post_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_post(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_try_cast(
        try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_try_cast(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_add_ref(
        add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_add_ref(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_release(
        release_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_release(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_object_released(
        object_released_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_object_released(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_transport_down(
        transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_transport_down(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    int child_transport::begin_host_get_new_zone_id(
        get_new_zone_id_params params,
        void* completion_ctx,
        complete_new_zone_id_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (!scheduler || !complete)
            return rpc::error::ZONE_NOT_FOUND();
        return scheduler->spawn_detached(complete_host_get_new_zone_id(std::move(params), completion_ctx, complete))
                   ? rpc::error::OK()
                   : rpc::error::TRANSPORT_ERROR();
    }

    CORO_TASK(void)
    child_transport::complete_host_send(
        send_params params,
        void* completion_ctx,
        complete_send_fn complete)
    {
        auto scheduler = scheduler_.lock();
        auto result = send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            result = CO_AWAIT inbound_send(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    child_transport::complete_host_post(
        post_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            CO_AWAIT inbound_post(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx);
    }

    CORO_TASK(void)
    child_transport::complete_host_try_cast(
        try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto scheduler = scheduler_.lock();
        auto result = standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            result = CO_AWAIT inbound_try_cast(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    child_transport::complete_host_add_ref(
        add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto scheduler = scheduler_.lock();
        auto result = standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            result = CO_AWAIT inbound_add_ref(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    child_transport::complete_host_release(
        release_params params,
        void* completion_ctx,
        complete_standard_fn complete)
    {
        auto scheduler = scheduler_.lock();
        auto result = standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            result = CO_AWAIT inbound_release(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx, &result);
    }

    CORO_TASK(void)
    child_transport::complete_host_object_released(
        object_released_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            CO_AWAIT inbound_object_released(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx);
    }

    CORO_TASK(void)
    child_transport::complete_host_transport_down(
        transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete)
    {
        auto scheduler = scheduler_.lock();
        if (scheduler)
        {
            CO_AWAIT scheduler->schedule();
            CO_AWAIT inbound_transport_down(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx);
    }

    CORO_TASK(void)
    child_transport::complete_host_get_new_zone_id(
        get_new_zone_id_params params,
        void* completion_ctx,
        complete_new_zone_id_fn complete)
    {
        auto scheduler = scheduler_.lock();
        auto result = new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        if (auto svc = get_service(); svc && scheduler)
        {
            CO_AWAIT scheduler->schedule();
            result = CO_AWAIT svc->get_new_zone_id(std::move(params));
            CO_AWAIT scheduler->schedule();
        }
        complete(completion_ctx, &result);
    }

    void child_transport::on_destination_count_zero()
    {
        // The DLL is unloaded only after the DLL-side parent transport has
        // died and the entry thread has returned.
    }

    void child_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);
    }

    CORO_TASK(rpc::connect_result)
    child_transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        connection_settings input_descr)
    {
        auto p_this = shared_from_this();
        auto svc = get_service();
        auto scheduler = svc->get_scheduler();
        scheduler_ = scheduler;

        get_new_zone_id_params zone_params;
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT svc->get_new_zone_id(std::move(zone_params));
        if (zone_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("[libcoro_dll_scheduled_dynamic_library] get_new_zone_id failed: {}", zone_result.error_code);
            CO_RETURN rpc::connect_result{zone_result.error_code, {}};
        }

        rpc::zone adjacent_zone_id = zone_result.zone_id;
        set_adjacent_zone_id(adjacent_zone_id);
        svc->add_transport(adjacent_zone_id, shared_from_this());

        if (stub)
        {
            auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id);
            if (ret != rpc::error::OK())
                CO_RETURN rpc::connect_result{ret, {}};
        }

        auto loaded = std::make_unique<loaded_runtime>(library_path_, scheduler);
        loaded->transport_name = get_name();
        loaded->settings = input_descr;
        auto* loaded_ptr = loaded.get();

        loaded->entry_thread = std::thread(
            [this, loaded_ptr, adjacent_zone_id]()
            {
                dll_start_result failure{};
                failure.connect_result = rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};

                loaded_ptr->lib_handle = load_native_library(loaded_ptr->library_path);
                if (!loaded_ptr->lib_handle)
                {
                    RPC_ERROR(
                        "[libcoro_dll_scheduled_dynamic_library] dlopen failed for {}: {}",
                        loaded_ptr->library_path,
                        load_error_text());
                    publish_ready(*loaded_ptr, failure);
                    return;
                }

                auto start_fn = reinterpret_cast<dll_start_fn>(
                    resolve_native_symbol(loaded_ptr->lib_handle, "canopy_libcoro_dll_scheduled_dll_start"));
                if (!start_fn)
                {
                    RPC_ERROR(
                        "[libcoro_dll_scheduled_dynamic_library] canopy_libcoro_dll_scheduled_dll_start not found in "
                        "{}: {}",
                        loaded_ptr->library_path,
                        load_error_text());
                    publish_ready(*loaded_ptr, failure);
                    return;
                }

                dll_start_params params{};
                params.name = loaded_ptr->transport_name.c_str();
                params.dll_zone = adjacent_zone_id;
                params.host_zone = get_zone_id();
                params.settings = &loaded_ptr->settings;
                params.host_ctx = this;
                params.host_send = &child_transport::host_begin_send;
                params.host_post = &child_transport::host_begin_post;
                params.host_try_cast = &child_transport::host_begin_try_cast;
                params.host_add_ref = &child_transport::host_begin_add_ref;
                params.host_release = &child_transport::host_begin_release;
                params.host_object_released = &child_transport::host_begin_object_released;
                params.host_transport_down = &child_transport::host_begin_transport_down;
                params.host_get_new_zone_id = &child_transport::host_begin_get_new_zone_id;
                params.host_coro_release_parent = &child_transport::host_coro_release_parent;
                params.ready_ctx = loaded_ptr;
                params.ready = &child_transport::dll_ready;

                start_fn(&params);

                publish_ready(*loaded_ptr, failure);
            });

        loaded_ = std::move(loaded);

        CO_AWAIT ready_awaitable{loaded_ptr->ready};
        CO_AWAIT scheduler->schedule();

        dll_start_result start_result{};
        {
            std::lock_guard lock(loaded_ptr->mutex);
            start_result = loaded_ptr->start_result;
        }

        if (start_result.connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR(
                "[libcoro_dll_scheduled_dynamic_library] DLL start failed: {}", start_result.connect_result.error_code);
            unload_library();
            CO_RETURN rpc::connect_result{start_result.connect_result.error_code, {}};
        }

        if (!start_result.runtime_ctx || !start_result.begin.send || !start_result.begin.post
            || !start_result.begin.try_cast || !start_result.begin.add_ref || !start_result.begin.release
            || !start_result.begin.object_released || !start_result.begin.transport_down)
        {
            RPC_ERROR("[libcoro_dll_scheduled_dynamic_library] canopy_libcoro_dll_scheduled_dll_start returned incomplete ABI result");
            unload_library();
            CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        dll_ctx_ = start_result.runtime_ctx;
        dll_begin_ = start_result.begin;
        keep_alive_ = p_this;

        set_status(rpc::transport_status::CONNECTED);
        CO_RETURN rpc::connect_result{rpc::error::OK(), start_result.connect_result.output_descriptor};
    }

    CORO_TASK(send_result)
    child_transport::outbound_send(send_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.send || !dll_ctx_ || !scheduler)
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        auto state = std::make_shared<pending_result_state<send_result>>(
            scheduler, send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}});
        auto err = dll_begin_.send(dll_ctx_, std::move(params), state.get(), &complete_send);
        if (err != rpc::error::OK())
            CO_RETURN send_result{err, {}, {}};

        auto result = CO_AWAIT pending_result_awaitable<send_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(void)
    child_transport::outbound_post(post_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.post || !dll_ctx_ || !scheduler)
            CO_RETURN;

        auto state = std::make_shared<pending_void_state>(scheduler);
        auto err = dll_begin_.post(dll_ctx_, std::move(params), state.get(), &complete_void);
        if (err != rpc::error::OK())
            CO_RETURN;

        CO_AWAIT pending_void_awaitable(std::move(state));
        CO_AWAIT scheduler->schedule();
    }

    CORO_TASK(standard_result)
    child_transport::outbound_try_cast(try_cast_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.try_cast || !dll_ctx_ || !scheduler)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        auto state = std::make_shared<pending_result_state<standard_result>>(
            scheduler, standard_result{rpc::error::ZONE_NOT_FOUND(), {}});
        auto err = dll_begin_.try_cast(dll_ctx_, std::move(params), state.get(), &complete_standard);
        if (err != rpc::error::OK())
            CO_RETURN standard_result{err, {}};

        auto result = CO_AWAIT pending_result_awaitable<standard_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    child_transport::outbound_add_ref(add_ref_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.add_ref || !dll_ctx_ || !scheduler)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        auto state = std::make_shared<pending_result_state<standard_result>>(
            scheduler, standard_result{rpc::error::ZONE_NOT_FOUND(), {}});
        auto err = dll_begin_.add_ref(dll_ctx_, std::move(params), state.get(), &complete_standard);
        if (err != rpc::error::OK())
            CO_RETURN standard_result{err, {}};

        auto result = CO_AWAIT pending_result_awaitable<standard_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    child_transport::outbound_release(release_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.release || !dll_ctx_ || !scheduler)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        auto state = std::make_shared<pending_result_state<standard_result>>(
            scheduler, standard_result{rpc::error::ZONE_NOT_FOUND(), {}});
        auto err = dll_begin_.release(dll_ctx_, std::move(params), state.get(), &complete_standard);
        if (err != rpc::error::OK())
            CO_RETURN standard_result{err, {}};

        auto result = CO_AWAIT pending_result_awaitable<standard_result>(std::move(state));
        CO_AWAIT scheduler->schedule();
        CO_RETURN result;
    }

    CORO_TASK(void)
    child_transport::outbound_object_released(object_released_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.object_released || !dll_ctx_ || !scheduler)
            CO_RETURN;

        auto state = std::make_shared<pending_void_state>(scheduler);
        auto err = dll_begin_.object_released(dll_ctx_, std::move(params), state.get(), &complete_void);
        if (err != rpc::error::OK())
            CO_RETURN;

        CO_AWAIT pending_void_awaitable(std::move(state));
        CO_AWAIT scheduler->schedule();
    }

    CORO_TASK(void)
    child_transport::outbound_transport_down(transport_down_params params)
    {
        auto p_this = shared_from_this();
        auto scheduler = scheduler_.lock();
        if (!dll_begin_.transport_down || !dll_ctx_ || !scheduler)
            CO_RETURN;

        auto state = std::make_shared<pending_void_state>(scheduler);
        auto err = dll_begin_.transport_down(dll_ctx_, std::move(params), state.get(), &complete_void);
        if (err != rpc::error::OK())
            CO_RETURN;

        CO_AWAIT pending_void_awaitable(std::move(state));
        CO_AWAIT scheduler->schedule();
    }

} // namespace rpc::libcoro_dll_scheduled_dynamic_library

#endif // CANOPY_BUILD_COROUTINE
