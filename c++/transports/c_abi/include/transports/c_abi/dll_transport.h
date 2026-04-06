/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <c_abi/dynamic_library/canopy_dynamic_library.h>
#  include <rpc/rpc.h>

#  include <atomic>
#  include <functional>
#  include <memory>

namespace rpc::c_abi
{
    namespace detail
    {
        rpc::expected<
            rpc::connection_settings,
            std::string>
        decode_connection_settings(const canopy_connection_settings& input_descr);

        int write_remote_object(
            const canopy_allocator_vtable& allocator,
            const rpc::remote_object& source,
            canopy_remote_object* output);
    } // namespace detail

    class parent_transport : public rpc::transport
    {
        canopy_parent_context parent_ctx_ = nullptr;
        canopy_allocator_vtable allocator_{};

        canopy_parent_send_fn parent_send_ = nullptr;
        canopy_parent_post_fn parent_post_ = nullptr;
        canopy_parent_try_cast_fn parent_try_cast_ = nullptr;
        canopy_parent_add_ref_fn parent_add_ref_ = nullptr;
        canopy_parent_release_fn parent_release_ = nullptr;
        canopy_parent_object_released_fn parent_object_released_ = nullptr;
        canopy_parent_transport_down_fn parent_transport_down_ = nullptr;
        canopy_parent_get_new_zone_id_fn parent_get_new_zone_id_ = nullptr;

    public:
        parent_transport(
            std::string name,
            rpc::zone child_zone,
            rpc::zone parent_zone,
            canopy_parent_context parent_ctx,
            canopy_allocator_vtable allocator,
            canopy_parent_send_fn send,
            canopy_parent_post_fn post,
            canopy_parent_try_cast_fn try_cast,
            canopy_parent_add_ref_fn add_ref,
            canopy_parent_release_fn release,
            canopy_parent_object_released_fn object_released,
            canopy_parent_transport_down_fn transport_down,
            canopy_parent_get_new_zone_id_fn get_new_zone_id);

        ~parent_transport() override CANOPY_DEFAULT_DESTRUCTOR;

        void set_status(rpc::transport_status status) override;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            connection_settings input_descr) override
        {
            std::ignore = stub;
            std::ignore = input_descr;
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

    struct dll_context
    {
        std::shared_ptr<parent_transport> transport;
        std::shared_ptr<rpc::child_service> service;
        canopy_allocator_vtable allocator_{};
        std::atomic<bool> destroyed{false};
    };

    template<
        class PARENT_INTERFACE,
        class CHILD_INTERFACE>
    int init_child_zone(
        canopy_dll_init_params* params,
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>,
            std::shared_ptr<rpc::child_service>)> factory)
    {
        if (!params || !params->input_descr)
            return rpc::error::INVALID_DATA();

        auto input_descr = detail::decode_connection_settings(*params->input_descr);
        if (!input_descr)
            return rpc::error::INVALID_DATA();

        auto parent_zone = rpc::zone_address::from_blob(std::vector<uint8_t>(
            params->parent_zone.address.blob.data,
            params->parent_zone.address.blob.data + params->parent_zone.address.blob.size));
        if (!parent_zone)
            return rpc::error::INVALID_DATA();

        auto child_zone = rpc::zone_address::from_blob(std::vector<uint8_t>(
            params->child_zone.address.blob.data,
            params->child_zone.address.blob.data + params->child_zone.address.blob.size));
        if (!child_zone)
            return rpc::error::INVALID_DATA();

        auto pt = std::make_shared<parent_transport>(
            params->name ? params->name : "c_abi child",
            rpc::zone(*child_zone),
            rpc::zone(*parent_zone),
            params->parent_ctx,
            params->allocator,
            params->parent_send,
            params->parent_post,
            params->parent_try_cast,
            params->parent_add_ref,
            params->parent_release,
            params->parent_object_released,
            params->parent_transport_down,
            params->parent_get_new_zone_id);

        auto create_result = rpc::child_service::create_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>(
            params->name ? params->name : "c_abi child",
            pt,
            *input_descr,
            std::move(factory));

        if (create_result.error_code != rpc::error::OK())
            return create_result.error_code;

        auto* ctx = new dll_context{};
        ctx->transport = pt;
        ctx->service = std::dynamic_pointer_cast<rpc::child_service>(pt->get_service());
        ctx->allocator_ = params->allocator;

        params->child_ctx = ctx;
        params->output_obj = {};
        auto err = detail::write_remote_object(params->allocator, create_result.descriptor, &params->output_obj);
        if (err != rpc::error::OK())
        {
            delete ctx;
            params->child_ctx = nullptr;
            params->output_obj = {};
            return err;
        }

        return rpc::error::OK();
    }
} // namespace rpc::c_abi

#endif // !CANOPY_BUILD_COROUTINE
