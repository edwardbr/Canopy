/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>

#include <rpc/rpc.h>

namespace rpc::local
{
    class child_transport;
    class parent_transport;

    // Transport from child zone to parent zone
    // Used by child to communicate with parent
    class parent_transport : public rpc::transport
    {
        stdex::member_ptr<child_transport> parent_;

    public:
        parent_transport(std::string name, std::shared_ptr<rpc::service> service, std::shared_ptr<child_transport> parent);
        parent_transport(std::string name, std::shared_ptr<child_transport> parent);

        ~parent_transport() override CANOPY_DEFAULT_DESTRUCTOR;

        // Override to propagate disconnect to parent zone
        void set_status(rpc::transport_status status) override;

        CORO_TASK(int)
        inner_connect(const std::shared_ptr<rpc::object_stub>& stub,
            connection_settings& input_descr,
            rpc::interface_descriptor& output_descr) override
        {
            std::ignore = stub;
            std::ignore = input_descr;
            std::ignore = output_descr;
            // Parent transport is connected immediately - no handshake needed
            CO_RETURN rpc::error::ZONE_NOT_SUPPORTED();
        }
        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // Outbound i_marshaller interface - sends from child to parent
        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(back_channel_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(back_channel_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(back_channel_result) outbound_release(release_params params) override;

        // New methods from i_marshaller interface
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;

        // Forwards the request up to the parent zone's child_transport inbound handler.
        CORO_TASK(get_new_zone_id_result) outbound_get_new_zone_id(get_new_zone_id_params params) override;
    };

    // Transport from parent zone to child zone
    // Used by parent to communicate with child

    class child_transport : public rpc::transport
    {
        stdex::member_ptr<parent_transport> child_;

        typedef std::function<CORO_TASK(int)(rpc::connection_settings input_descr,
            rpc::interface_descriptor& output_descr,
            const std::shared_ptr<child_transport>& parent,
            std::shared_ptr<parent_transport>& child)>
            child_entry_point_factory_fn;

        child_entry_point_factory_fn child_entry_point_factory_fn_;

    public:
        child_transport(std::string name, std::shared_ptr<rpc::service> service)
            : rpc::transport(name, service)
        {
            set_status(rpc::transport_status::CONNECTED);
        }

        ~child_transport() override CANOPY_DEFAULT_DESTRUCTOR;

        // Called by parent_transport when child zone disconnects
        void on_child_disconnected();

        CORO_TASK(int)
        inner_connect(const std::shared_ptr<rpc::object_stub>& stub,
            connection_settings& input_descr,
            rpc::interface_descriptor& output_descr) override
        {
            auto svc = get_service();
            // get a new adjacent_zone_id
            get_new_zone_id_params params;
            params.protocol_version = rpc::get_version();
            auto result = CO_AWAIT svc->get_new_zone_id(std::move(params));
            if (result.error_code != rpc::error::OK())
            {
                RPC_ERROR("[local] get_new_zone_id failed: {}", result.error_code);
                RPC_ASSERT(false);
                CO_RETURN result.error_code;
            }
            rpc::zone adjacent_zone_id = result.zone_id;

            set_adjacent_zone_id(adjacent_zone_id);

            svc->add_transport(adjacent_zone_id, shared_from_this());

            if (stub)
            {
                auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id);
                if (ret != rpc::error::OK())
                {
                    CO_RETURN ret;
                }
            }

            assert(child_entry_point_factory_fn_);
            std::shared_ptr<parent_transport> child;
            auto ret = CO_AWAIT child_entry_point_factory_fn_(
                input_descr, output_descr, std::static_pointer_cast<child_transport>(shared_from_this()), child);
            child_entry_point_factory_fn_ = nullptr;
            if (ret == rpc::error::OK())
                child_ = child;

            // as each transport links a stub to a proxy there will always be a positive count in both ways

            // as the parent has not got its proxy bound we do not include the output_descr state yet
            auto expected_parent_count = input_descr.get_object_id() != 0 ? 1 : 0;

            // as the child should be fully initialised by this time so we add the output count too
            auto expected_child_count = expected_parent_count + (output_descr.get_object_id() != 0 ? 1 : 0);

            RPC_ASSERT(get_destination_count() >= expected_parent_count);
            RPC_ASSERT(child->get_destination_count() >= expected_child_count);

            CO_RETURN ret;
        }
        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // Outbound i_marshaller interface - sends from parent to child
        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(back_channel_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(back_channel_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(back_channel_result) outbound_release(release_params params) override;

        // New methods from i_marshaller interface
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;

        template<class in_param_type, class out_param_type>
        void set_child_entry_point(std::function<CORO_TASK(int)(const rpc::shared_ptr<in_param_type>&,
                rpc::shared_ptr<out_param_type>&,
                const std::shared_ptr<rpc::child_service>&)>&& child_entry_point_fn)
        {

            child_entry_point_factory_fn_
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                = [child_entry_point_fn = std::move(child_entry_point_fn)](rpc::connection_settings input_descr,
                      rpc::interface_descriptor& output_descr,
                      const std::shared_ptr<child_transport>& parent,
                      std::shared_ptr<parent_transport>& child) mutable -> CORO_TASK(int)
            {
                child = std::make_shared<parent_transport>("child", parent);

                auto err_code = CO_AWAIT rpc::child_service::create_child_zone<in_param_type, out_param_type>("child",
                    child,
                    input_descr,
                    output_descr,
                    std::move(child_entry_point_fn)
#ifdef CANOPY_BUILD_COROUTINE
                        ,
                    parent->get_service()->get_scheduler()
#endif
                );
                if (err_code != rpc::error::OK())
                {
                    child = nullptr;
                }
                CO_RETURN err_code;
            };
        }
    };

}
