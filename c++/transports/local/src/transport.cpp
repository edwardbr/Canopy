/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/local/transport.h>
#include <rpc/rpc.h>

namespace rpc::local
{
    parent_transport::parent_transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<child_transport> parent)
        : rpc::transport(
              name,
              service)
        , parent_(parent)
    {
        // Local transports are always immediately available (in-process)
        set_status(rpc::transport_status::CONNECTED);
    }

    parent_transport::parent_transport(
        std::string name,
        std::shared_ptr<child_transport> parent)
        : rpc::transport(
              name,
              parent->get_adjacent_zone_id())
        , parent_(parent)
    {
        set_adjacent_zone_id(parent->get_zone_id());
        // Local transports are always immediately available (in-process)
        set_status(rpc::transport_status::CONNECTED);
    }

    void parent_transport::set_status(rpc::transport_status status)
    {
        // Call base class to update status
        rpc::transport::set_status(status);

        // If disconnecting, notify parent zone to break circular reference
        if (status == rpc::transport_status::DISCONNECTED)
        {
            auto parent = parent_.get_nullable();
            if (parent)
            {
                // Notify parent zone's child_transport to break its child_ reference
                parent->on_child_disconnected();
            }
            // Break our reference to parent
            parent_.reset();
        }
    }

    // Outbound i_marshaller interface - sends from child to parent
    CORO_TASK(send_result)
    parent_transport::outbound_send(send_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_send: parent is NULL!");
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        }

        // Forward to parent transport's inbound handler
        CO_RETURN CO_AWAIT parent->inbound_send(std::move(params));
    }

    CORO_TASK(void)
    parent_transport::outbound_post(post_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_post: parent is NULL!");
            CO_RETURN;
        }

        CO_AWAIT parent->inbound_post(std::move(params));
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_try_cast(try_cast_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_try_cast: parent is NULL!");
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        }

        CO_RETURN CO_AWAIT parent->inbound_try_cast(std::move(params));
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_add_ref(add_ref_params params)
    {
        RPC_DEBUG(
            "parent_transport::outbound_add_ref: my_zone={}, adjacent_zone={}, destination_zone={}, caller_zone={}",
            get_zone_id().get_subnet(),
            get_adjacent_zone_id().get_subnet(),
            params.remote_object_id.get_subnet(),
            params.caller_zone_id.get_subnet());

        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_add_ref: parent is NULL!");
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        }

        RPC_DEBUG(
            "parent_transport::outbound_add_ref: Calling parent->inbound_add_ref for zone {}",
            params.remote_object_id.get_subnet());
        CO_RETURN CO_AWAIT parent->inbound_add_ref(std::move(params));
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_release(release_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_release: parent is NULL!");
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        }

        CO_RETURN CO_AWAIT parent->inbound_release(std::move(params));
    }

    CORO_TASK(void)
    parent_transport::outbound_object_released(object_released_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_object_released: parent is NULL!");
            CO_RETURN;
        }

        CO_AWAIT parent->inbound_object_released(std::move(params));
    }

    CORO_TASK(void)
    parent_transport::outbound_transport_down(transport_down_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_transport_down: parent is NULL!");
            CO_RETURN;
        }

        CO_AWAIT parent->inbound_transport_down(std::move(params));
    }

    CORO_TASK(new_zone_id_result)
    parent_transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        auto parent = parent_.get_nullable();
        if (!parent)
        {
            RPC_ERROR("parent_transport::outbound_get_new_zone_id: parent is NULL!");
            CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        }
        CO_RETURN CO_AWAIT parent->get_new_zone_id(std::move(params));
    }

    // Transport from parent zone to child zone
    // Used by parent to communicate with child

    void child_transport::on_child_disconnected()
    {
        // Break circular reference when child zone disconnects
        // Safe because stack-based shared_ptr in outbound_* methods keeps parent_transport alive
        child_.reset();
    }

    // Outbound i_marshaller interface - sends from parent to child
    CORO_TASK(send_result)
    child_transport::outbound_send(send_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        }

        // Forward to child service's inbound handler via its transport
        // Since child_service IS an i_marshaller, we can call directly
        CO_RETURN CO_AWAIT child->inbound_send(std::move(params));
    }

    CORO_TASK(void)
    child_transport::outbound_post(post_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN;
        }

        CO_AWAIT child->inbound_post(std::move(params));
    }

    CORO_TASK(standard_result)
    child_transport::outbound_try_cast(try_cast_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        }

        CO_RETURN CO_AWAIT child->inbound_try_cast(std::move(params));
    }

    CORO_TASK(standard_result)
    child_transport::outbound_add_ref(add_ref_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        }

        CO_RETURN CO_AWAIT child->inbound_add_ref(std::move(params));
    }

    CORO_TASK(standard_result)
    child_transport::outbound_release(release_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        }

        CO_RETURN CO_AWAIT child->inbound_release(std::move(params));
    }

    CORO_TASK(void)
    child_transport::outbound_object_released(object_released_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN;
        }

        CO_AWAIT child->inbound_object_released(std::move(params));
    }

    CORO_TASK(void)
    child_transport::outbound_transport_down(transport_down_params params)
    {
        auto child = child_.get_nullable();
        if (!child)
        {
            CO_RETURN;
        }

        CO_AWAIT child->inbound_transport_down(std::move(params));
    }
}
