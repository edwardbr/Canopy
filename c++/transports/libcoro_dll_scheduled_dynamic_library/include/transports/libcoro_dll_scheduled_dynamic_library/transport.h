/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Host-side libcoro_dll_scheduled_dynamic_library transport.  Coroutine-build only.
//
// child_transport lives in the host zone and manages the lifetime of a loaded
// shared object.  The shared object exports one blocking entry point,
// canopy_libcoro_dll_scheduled_dll_start.  The host runs that entry point on a dedicated
// entry thread and unloads the library only after the entry point has returned.
//
// Cross-boundary RPCs use non-blocking begin_* functions and completion
// callbacks.  The host never CO_AWAITs a DLL-owned coro::task.

#ifdef CANOPY_BUILD_COROUTINE

#  include <atomic>
#  include <memory>
#  include <string>

#  include <rpc/rpc.h>
#  include <transports/libcoro_dll_scheduled_dynamic_library/dll_abi.h>

namespace rpc::libcoro_dll_scheduled_dynamic_library
{
    struct loaded_runtime;

    class child_transport : public rpc::transport
    {
        std::shared_ptr<rpc::transport> keep_alive_;
        std::weak_ptr<coro::scheduler> scheduler_;
        std::unique_ptr<loaded_runtime> loaded_;
        std::atomic_bool release_task_scheduled_ = false;

        void* dll_ctx_ = nullptr;
        dll_begin_table dll_begin_;
        std::string library_path_;

        static int host_begin_send(
            void* ctx,
            send_params params,
            void* completion_ctx,
            complete_send_fn complete);
        static int host_begin_post(
            void* ctx,
            post_params params,
            void* completion_ctx,
            complete_void_fn complete);
        static int host_begin_try_cast(
            void* ctx,
            try_cast_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        static int host_begin_add_ref(
            void* ctx,
            add_ref_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        static int host_begin_release(
            void* ctx,
            release_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        static int host_begin_object_released(
            void* ctx,
            object_released_params params,
            void* completion_ctx,
            complete_void_fn complete);
        static int host_begin_transport_down(
            void* ctx,
            transport_down_params params,
            void* completion_ctx,
            complete_void_fn complete);
        static int host_begin_get_new_zone_id(
            void* ctx,
            get_new_zone_id_params params,
            void* completion_ctx,
            complete_new_zone_id_fn complete);

        static void host_coro_release_parent(void* ctx);
        static void dll_ready(
            void* ctx,
            dll_start_result* result);

        int begin_host_send(
            send_params params,
            void* completion_ctx,
            complete_send_fn complete);
        int begin_host_post(
            post_params params,
            void* completion_ctx,
            complete_void_fn complete);
        int begin_host_try_cast(
            try_cast_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        int begin_host_add_ref(
            add_ref_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        int begin_host_release(
            release_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        int begin_host_object_released(
            object_released_params params,
            void* completion_ctx,
            complete_void_fn complete);
        int begin_host_transport_down(
            transport_down_params params,
            void* completion_ctx,
            complete_void_fn complete);
        int begin_host_get_new_zone_id(
            get_new_zone_id_params params,
            void* completion_ctx,
            complete_new_zone_id_fn complete);

        CORO_TASK(void)
        complete_host_send(
            send_params params,
            void* completion_ctx,
            complete_send_fn complete);
        CORO_TASK(void)
        complete_host_post(
            post_params params,
            void* completion_ctx,
            complete_void_fn complete);
        CORO_TASK(void)
        complete_host_try_cast(
            try_cast_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        CORO_TASK(void)
        complete_host_add_ref(
            add_ref_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        CORO_TASK(void)
        complete_host_release(
            release_params params,
            void* completion_ctx,
            complete_standard_fn complete);
        CORO_TASK(void)
        complete_host_object_released(
            object_released_params params,
            void* completion_ctx,
            complete_void_fn complete);
        CORO_TASK(void)
        complete_host_transport_down(
            transport_down_params params,
            void* completion_ctx,
            complete_void_fn complete);
        CORO_TASK(void)
        complete_host_get_new_zone_id(
            get_new_zone_id_params params,
            void* completion_ctx,
            complete_new_zone_id_fn complete);

        void unload_library();
        void schedule_parent_release();
        CORO_TASK(void) release_parent_after_dll_callback(std::shared_ptr<coro::scheduler> scheduler);

    protected:
        void on_destination_count_zero() override;

    public:
        child_transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::string library_path);

        ~child_transport() override;

        void set_status(rpc::transport_status status) override;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
    };

} // namespace rpc::libcoro_dll_scheduled_dynamic_library

#endif // CANOPY_BUILD_COROUTINE
