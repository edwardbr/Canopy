/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>
#  include <c_abi/dynamic_library/canopy_dynamic_library.h>
#  include <transports/c_abi/dynamic_library_loader.h>

#  include <string>

namespace rpc::c_abi
{
    // Parent-side transport for children that expose the shared c_abi dynamic
    // library boundary. This mirrors rpc::dynamic_library::child_transport
    // closely, but all cross-language communication flows through the neutral
    // canopy_dll_* entry points and explicit type translation.
    class child_transport : public rpc::transport
    {
        dynamic_library_loader loader_;
        canopy_child_context child_ctx_ = nullptr;
        canopy_allocator_vtable allocator_{};
        std::string library_path_;

        static canopy_byte_buffer cb_alloc(
            void* allocator_ctx,
            size_t size);
        static void cb_free(
            void* allocator_ctx,
            uint8_t* data,
            size_t size);

        static int cb_send(
            canopy_parent_context parent_ctx,
            const canopy_send_params* params,
            canopy_send_result* result);
        static void cb_post(
            canopy_parent_context parent_ctx,
            const canopy_post_params* params);
        static int cb_try_cast(
            canopy_parent_context parent_ctx,
            const canopy_try_cast_params* params,
            canopy_standard_result* result);
        static int cb_add_ref(
            canopy_parent_context parent_ctx,
            const canopy_add_ref_params* params,
            canopy_standard_result* result);
        static int cb_release(
            canopy_parent_context parent_ctx,
            const canopy_release_params* params,
            canopy_standard_result* result);
        static void cb_object_released(
            canopy_parent_context parent_ctx,
            const canopy_object_released_params* params);
        static void cb_transport_down(
            canopy_parent_context parent_ctx,
            const canopy_transport_down_params* params);
        static int cb_get_new_zone_id(
            canopy_parent_context parent_ctx,
            const canopy_get_new_zone_id_params* params,
            canopy_new_zone_id_result* result);

        int load_library();
        void unload_library();

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
} // namespace rpc::c_abi

#endif
