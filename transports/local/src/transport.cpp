/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/local/transport.h>
#include <rpc/rpc.h>

namespace rpc::local
{
    parent_transport::parent_transport(
        std::string name, std::shared_ptr<rpc::service> service, std::shared_ptr<child_transport> parent)
        : rpc::transport(name, service, parent->get_zone_id())
        , parent_(parent)
    {
        // Local transports are always immediately available (in-process)
        set_status(rpc::transport_status::CONNECTED);
    }

    parent_transport::parent_transport(std::string name, rpc::zone zone_id, std::shared_ptr<child_transport> parent)
        : rpc::transport(name, zone_id, parent->get_zone_id())
        , parent_(parent)
    {
        // Local transports are always immediately available (in-process)
        set_status(rpc::transport_status::CONNECTED);
    }

    // Outbound i_marshaller interface - sends from child to parent
    CORO_TASK(int)
    parent_transport::outbound_send(uint64_t protocol_version,
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
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {

        auto parent = parent_.get_nullable();
        if (!parent)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        // Forward to parent transport's inbound handler
        CO_RETURN CO_AWAIT parent->inbound_send(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            out_buf_,
            in_back_channel,
            out_back_channel);
    }

    CORO_TASK(void)
    parent_transport::outbound_post(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {

        auto parent = parent_.get_nullable();
        if (!parent)
        {
            CO_RETURN;
        }

        CO_AWAIT parent->inbound_post(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            in_back_channel);
    }

    CORO_TASK(int)
    parent_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        CO_RETURN CO_AWAIT parent->inbound_try_cast(
            protocol_version, destination_zone_id, object_id, interface_id, in_back_channel, out_back_channel);
    }

    CORO_TASK(int)
    parent_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG(
            "parent_transport::outbound_add_ref: my_zone={}, adjacent_zone={}, destination_zone={}, caller_zone={}",
            get_zone_id().get_val(),
            get_adjacent_zone_id().get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val());

        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_add_ref: parent is NULL!");
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        RPC_DEBUG("parent_transport::outbound_add_ref: Calling parent->inbound_add_ref for zone {}",
            destination_zone_id.get_val());
        auto error_code = CO_AWAIT parent->inbound_add_ref(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            known_direction_zone_id,
            build_out_param_channel,
            reference_count,
            in_back_channel,
            out_back_channel);

        CO_RETURN error_code;
    }

    CORO_TASK(int)
    parent_transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        auto error_code = CO_AWAIT parent->inbound_release(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            options,
            reference_count,
            in_back_channel,
            out_back_channel);

        CO_RETURN error_code;
    }

    CORO_TASK(void)
    parent_transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {

        auto parent = parent_.get_nullable();
        if (!parent)
        {
            CO_RETURN;
        }

        CO_AWAIT parent->inbound_object_released(
            protocol_version, destination_zone_id, object_id, caller_zone_id, in_back_channel);
    }

    CORO_TASK(void)
    parent_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {

        auto parent = parent_.get_nullable();
        if (!parent)
        {
            CO_RETURN;
        }

        CO_AWAIT parent->inbound_transport_down(protocol_version, destination_zone_id, caller_zone_id, in_back_channel);
    }

    // Transport from parent zone to child zone
    // Used by parent to communicate with child

    // Outbound i_marshaller interface - sends from parent to child
    CORO_TASK(int)
    child_transport::outbound_send(uint64_t protocol_version,
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
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {

        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        // Forward to child service's inbound handler via its transport
        // Since child_service IS an i_marshaller, we can call directly
        CO_RETURN CO_AWAIT child->inbound_send(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            out_buf_,
            in_back_channel,
            out_back_channel);
    }

    CORO_TASK(void)
    child_transport::outbound_post(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN;
        }

        CO_AWAIT child->inbound_post(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            in_back_channel);
    }

    CORO_TASK(int)
    child_transport::outbound_try_cast(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        CO_RETURN CO_AWAIT child->inbound_try_cast(
            protocol_version, destination_zone_id, object_id, interface_id, in_back_channel, out_back_channel);
    }

    CORO_TASK(int)
    child_transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {

        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        CO_RETURN CO_AWAIT child->inbound_add_ref(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            known_direction_zone_id,
            build_out_param_channel,
            reference_count,
            in_back_channel,
            out_back_channel);
    }

    CORO_TASK(int)
    child_transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        }

        CO_RETURN CO_AWAIT child->inbound_release(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            options,
            reference_count,
            in_back_channel,
            out_back_channel);
    }

    CORO_TASK(void)
    child_transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN;
        }

        CO_AWAIT child->inbound_object_released(
            protocol_version, destination_zone_id, object_id, caller_zone_id, in_back_channel);
    }

    CORO_TASK(void)
    child_transport::outbound_transport_down(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN;
        }

        CO_AWAIT child->inbound_transport_down(protocol_version, destination_zone_id, caller_zone_id, in_back_channel);
    }
}
