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

        virtual ~parent_transport() DEFAULT_DESTRUCTOR;

        // Override to propagate disconnect to parent zone
        void set_status(rpc::transport_status status) override;

        CORO_TASK(int)
        inner_connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr) override
        {
            std::ignore = input_descr;
            std::ignore = output_descr;
            // Parent transport is connected immediately - no handshake needed
            CO_RETURN rpc::error::OK();
        }
        // Outbound i_marshaller interface - sends from child to parent
        CORO_TASK(int)
        outbound_send(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        outbound_post(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(int)
        outbound_try_cast(uint64_t protocol_version,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_add_ref(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            rpc::known_direction_zone known_direction_zone_id,
            rpc::add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_release(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            rpc::release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        // New methods from i_marshaller interface
        CORO_TASK(void)
        outbound_object_released(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(void)
        outbound_transport_down(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;
    };

    // Transport from parent zone to child zone
    // Used by parent to communicate with child

    class child_transport : public rpc::transport
    {
        stdex::member_ptr<parent_transport> child_;

        typedef std::function<CORO_TASK(int)(rpc::interface_descriptor input_descr,
            rpc::interface_descriptor& output_descr,
            const std::shared_ptr<child_transport>& parent,
            std::shared_ptr<parent_transport>& child)>
            child_entry_point_factory_fn;

        child_entry_point_factory_fn child_entry_point_factory_fn_;

    public:
        child_transport(std::string name, std::shared_ptr<rpc::service> service, rpc::zone adjacent_zone_id)
            : rpc::transport(name, service, adjacent_zone_id)
        {
            set_status(rpc::transport_status::CONNECTED);
        }

        virtual ~child_transport() DEFAULT_DESTRUCTOR;

        // Called by parent_transport when child zone disconnects
        void on_child_disconnected();

        CORO_TASK(int)
        inner_connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr) override
        {
            assert(child_entry_point_factory_fn_);
            std::shared_ptr<parent_transport> child;
            auto ret = CO_AWAIT child_entry_point_factory_fn_(
                input_descr, output_descr, std::static_pointer_cast<child_transport>(shared_from_this()), child);
            child_entry_point_factory_fn_ = nullptr;
            if (ret == rpc::error::OK())
                child_ = child;

            // as each transport links a stub to a proxy there will always be a positive count in both ways

            // as the parent has not got its proxy bound we do not include the output_descr state yet
            auto expected_parent_count = input_descr.object_id != 0 ? 1 : 0;

            // as the child should be fully initialised by this time so we add the output count too
            auto expected_child_count = expected_parent_count + (output_descr.object_id != 0 ? 1 : 0);

            RPC_ASSERT(get_destination_count() >= expected_parent_count);
            RPC_ASSERT(child->get_destination_count() >= expected_child_count);

            CO_RETURN ret;
        }

        // Outbound i_marshaller interface - sends from parent to child
        CORO_TASK(int)
        outbound_send(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        outbound_post(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(int)
        outbound_try_cast(uint64_t protocol_version,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_add_ref(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            rpc::known_direction_zone known_direction_zone_id,
            rpc::add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_release(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            rpc::release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        // New methods from i_marshaller interface
        CORO_TASK(void)
        outbound_object_released(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(void)
        outbound_transport_down(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        template<class in_param_type, class out_param_type>
        void set_child_entry_point(std::function<CORO_TASK(int)(const rpc::shared_ptr<in_param_type>&,
                rpc::shared_ptr<out_param_type>&,
                const std::shared_ptr<rpc::child_service>&)>&& child_entry_point_fn)
        {

            child_entry_point_factory_fn_
                = [child_entry_point_fn = std::move(child_entry_point_fn)](rpc::interface_descriptor input_descr,
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
