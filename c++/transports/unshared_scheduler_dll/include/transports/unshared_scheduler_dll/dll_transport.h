/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// DLL-side unshared_scheduler_dll transport.  Coroutine-build only.
//
// This header is included by shared objects that participate in the
// Canopy unshared_scheduler_dll transport.  The shared object supplies
// canopy_unshared_scheduler_dll_init; the transport library supplies the exported
// canopy_unshared_scheduler_dll_start entry point.

#ifdef CANOPY_BUILD_COROUTINE

#  include <atomic>
#  include <condition_variable>
#  include <functional>
#  include <memory>
#  include <mutex>

#  include <rpc/rpc.h>
#  include <transports/unshared_scheduler_dll/dll_abi.h>

namespace rpc::unshared_scheduler_dll
{
    // Concrete init coroutine supplied by the shared object that links this
    // transport library. canopy_unshared_scheduler_dll_start calls this once on the
    // DLL-owned scheduler.
    coro::task<rpc::connect_result> canopy_unshared_scheduler_dll_init(
        void* runtime_ctx,
        const rpc::connection_settings* settings);

    class runtime_finish_gate
    {
        std::mutex mutex_;
        std::condition_variable cv_;
        bool finished_ = false;

    public:
        void finish();
        void wait();
    };

    class parent_transport : public rpc::transport
    {
        void* host_ctx_;

        host_begin_send_fn host_send_;
        host_begin_post_fn host_post_;
        host_begin_try_cast_fn host_try_cast_;
        host_begin_add_ref_fn host_add_ref_;
        host_begin_release_fn host_release_;
        host_begin_object_released_fn host_object_released_;
        host_begin_transport_down_fn host_transport_down_;
        host_begin_get_new_zone_id_fn host_get_new_zone_id_;
        host_coro_release_parent_fn host_coro_release_parent_;
        std::weak_ptr<coro::scheduler> scheduler_;
        std::weak_ptr<runtime_finish_gate> runtime_finished_;

    public:
        parent_transport(
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
            std::weak_ptr<runtime_finish_gate> runtime_finished);

        ~parent_transport() override;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub>,
            connection_settings) override
        {
            CO_RETURN rpc::connect_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
        CORO_TASK(new_zone_id_result) outbound_get_new_zone_id(get_new_zone_id_params params) override;
    };

    struct runtime_context
    {
        std::shared_ptr<coro::scheduler> scheduler;
        std::shared_ptr<runtime_finish_gate> runtime_finished;
        std::shared_ptr<parent_transport> pending_transport;
        std::weak_ptr<parent_transport> transport;
        std::atomic_bool destroyed{false};
    };

    template<
        class PARENT_INTERFACE,
        class CHILD_INTERFACE>
    coro::task<rpc::connect_result> init_child_zone(
        void* runtime_ctx,
        const rpc::connection_settings* settings,
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>,
            std::shared_ptr<rpc::child_service>)> factory)
    {
        auto* runtime = static_cast<runtime_context*>(runtime_ctx);
        if (!runtime || !runtime->scheduler || !runtime->pending_transport)
            co_return rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};

        auto pt = runtime->pending_transport;

        auto create_result = co_await rpc::child_service::create_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>(
            pt->get_name().c_str(), pt, *settings, std::move(factory), runtime->scheduler);

        if (create_result.error_code != rpc::error::OK())
            co_return rpc::connect_result{create_result.error_code, {}};

        runtime->transport = pt;
        runtime->pending_transport.reset();

        co_return rpc::connect_result{rpc::error::OK(), create_result.descriptor};
    }

} // namespace rpc::unshared_scheduler_dll

#endif // CANOPY_BUILD_COROUTINE
