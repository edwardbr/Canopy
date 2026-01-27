/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/rpc.h>
#include <rpc/internal/pass_through.h>
#include <rpc/internal/transport.h>
#include <limits>

namespace rpc
{

    pass_through::pass_through(std::shared_ptr<transport> forward,
        std::shared_ptr<transport> reverse,
        std::shared_ptr<service> service,
        destination_zone forward_dest,
        destination_zone reverse_dest)
        : forward_destination_(forward_dest)
        , reverse_destination_(reverse_dest)
        , forward_transport_(forward)
        , reverse_transport_(reverse)
        , service_(service)
        , status_(pass_through_status::CONNECTED)
        , function_count_(0)
        , zone_id_(service->get_zone_id())
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_creation(zone_id_,
                forward_dest,
                reverse_dest,
                shared_count_.load(std::memory_order_acquire),
                optimistic_count_.load(std::memory_order_acquire));
#endif
    }

    std::shared_ptr<pass_through> pass_through::create(std::shared_ptr<transport> forward,
        std::shared_ptr<transport> reverse,
        std::shared_ptr<service> service,
        destination_zone forward_dest,
        destination_zone reverse_dest)
    {
        std::shared_ptr<pass_through> pt(
            new rpc::pass_through(forward, // forward_transport: handles messages TO final destination
                reverse,                   // reverse_transport: handles messages back to caller
                service,                   // service
                forward_dest,
                reverse_dest // reverse_destination: where reverse messages go
                ));
        pt->self_ref_ = pt; // keep self alive based on reference counts
        return pt;
    }

    void pass_through::queue_pending_release(
        uint64_t protocol_version, destination_zone destination_zone_id, caller_zone caller_zone_id, release_options options)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& entry : pending_releases_)
        {
            if (entry.protocol_version == protocol_version && entry.destination_zone_id == destination_zone_id
                && entry.caller_zone_id == caller_zone_id && entry.options == options)
            {
                entry.count++;
                return;
            }
        }
        pending_release_entry entry;
        entry.protocol_version = protocol_version;
        entry.destination_zone_id = destination_zone_id;
        entry.caller_zone_id = caller_zone_id;
        entry.options = options;
        entry.count = 1;
        pending_releases_.push_back(entry);
    }

    CORO_TASK(void) pass_through::drain_pending_releases(uint64_t protocol_version)
    {
        if (draining_pending_.exchange(true))
        {
            CO_RETURN;
        }

        if (function_count_.load(std::memory_order_acquire) != 0)
        {
            draining_pending_.store(false, std::memory_order_release);
            CO_RETURN;
        }

        std::vector<pending_release_entry> pending;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending.swap(pending_releases_);
        }

        for (const auto& entry : pending)
        {
            for (uint64_t i = 0; i < entry.count; ++i)
            {
                std::vector<rpc::back_channel_entry> empty_in;
                std::vector<rpc::back_channel_entry> empty_out;
                CO_AWAIT release(entry.protocol_version ? entry.protocol_version : protocol_version,
                    entry.destination_zone_id,
                    object{std::numeric_limits<uint64_t>::max()},
                    entry.caller_zone_id,
                    entry.options,
                    empty_in,
                    empty_out);
            }
        }

        draining_pending_.store(false, std::memory_order_release);
    }

    pass_through::~pass_through()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_deletion(zone_id_, forward_destination_, reverse_destination_);
#endif
    }

    std::shared_ptr<transport> pass_through::get_directional_transport(destination_zone dest)
    {
        if (dest.get_val() == forward_destination_.get_val())
        {
            return forward_transport_;
        }
        else if (dest.get_val() == reverse_destination_.get_val())
        {
            return reverse_transport_;
        }
        return nullptr;
    }

    CORO_TASK(int)
    pass_through::send(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        // Check if we're in the process of disconnecting
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(destination_zone_id);
        if (!target_transport)
        {
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        // Check transport status before routing
        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            // Transport error - trigger self-deletion
            trigger_self_destruction();
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Increment function count to track this active call
        function_count_.fetch_add(1, std::memory_order_acq_rel);

        // Forward the call directly to the transport
        auto result = CO_AWAIT target_transport->send(protocol_version,
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

        // Decrement function count after completing the call
        uint64_t remaining_count = function_count_.fetch_sub(1, std::memory_order_acq_rel);

        // If transport error, trigger self-deletion
        if (result == error::TRANSPORT_ERROR())
        {
            trigger_self_destruction();
        }
        // Check if we need to trigger cleanup after call completion
        else if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED && remaining_count == 1)
        {
            trigger_self_destruction();
        }
        else if (remaining_count == 1 && !draining_pending_.load(std::memory_order_acquire))
        {
            CO_AWAIT drain_pending_releases(protocol_version);
        }

        CO_RETURN result;
    }

    CORO_TASK(void)
    pass_through::post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        // Check if we're in the process of disconnecting
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED)
        {
            CO_RETURN;
        }
        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(destination_zone_id);
        if (!target_transport)
        {
            CO_RETURN;
        }

        // Increment function count to track this active call
        function_count_.fetch_add(1, std::memory_order_acq_rel);

        // Forward the post message directly to the transport
        CO_AWAIT target_transport->post(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            in_back_channel);

        // Decrement function count after completing the call
        uint64_t remaining_count = function_count_.fetch_sub(1, std::memory_order_acq_rel);

        // Check if we need to trigger cleanup after call completion
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED && remaining_count == 1)
        {
            trigger_self_destruction();
        }
        else if (remaining_count == 1 && !draining_pending_.load(std::memory_order_acquire))
        {
            CO_AWAIT drain_pending_releases(protocol_version);
        }
    }

    CORO_TASK(int)
    pass_through::try_cast(uint64_t protocol_version,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        // Check if we're in the process of disconnecting
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(destination_zone_id);
        if (!target_transport)
        {
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        // Check transport status before routing
        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            // Transport error - trigger self-deletion
            trigger_self_destruction();
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Increment function count to track this active call
        function_count_.fetch_add(1, std::memory_order_acq_rel);

        // Forward the call directly to the transport
        auto result = CO_AWAIT target_transport->try_cast(
            protocol_version, caller_zone_id, destination_zone_id, object_id, interface_id, in_back_channel, out_back_channel);

        // Decrement function count after completing the call
        uint64_t remaining_count = function_count_.fetch_sub(1, std::memory_order_acq_rel);

        // If transport error, trigger self-deletion
        if (result == error::TRANSPORT_ERROR())
        {
            trigger_self_destruction();
        }
        // Check if we need to trigger cleanup after call completion
        else if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED && remaining_count == 1)
        {
            trigger_self_destruction();
        }
        else if (remaining_count == 1 && !draining_pending_.load(std::memory_order_acquire))
        {
            CO_AWAIT drain_pending_releases(protocol_version);
        }

        CO_RETURN result;
    }

    CORO_TASK(int)
    pass_through::add_ref(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        known_direction_zone known_direction_zone_id,
        add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        bool no_local_add_ref = !!(build_out_param_channel & add_ref_options::build_caller_route)
                                && !!(build_out_param_channel & add_ref_options::build_destination_route);

        bool build_caller_channel = !!(build_out_param_channel & add_ref_options::build_caller_route);
        bool build_dest_channel = !!(build_out_param_channel & add_ref_options::build_destination_route)
                                  || build_out_param_channel == add_ref_options::normal
                                  || build_out_param_channel == add_ref_options::optimistic;

        std::shared_ptr<rpc::transport> caller_transport;
        std::shared_ptr<rpc::transport> destination_transport;

        RPC_INFO("pass_through::add_ref zone={}, fwd={}, rev={}, dest={}, caller={}, options={}, build_dest={}, "
                 "build_caller={}, no_local={}",
            zone_id_.get_val(),
            forward_destination_.get_val(),
            reverse_destination_.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            static_cast<uint64_t>(build_out_param_channel),
            build_dest_channel,
            build_caller_channel,
            no_local_add_ref);

        // Determine target transport based on destination_zone
        if (build_dest_channel)
        {
            destination_transport = get_directional_transport(destination_zone_id);
            if (!destination_transport)
            {
                CO_RETURN error::ZONE_NOT_FOUND();
            }
            // Check transport status before routing
            if (destination_transport->get_status() != transport_status::CONNECTED)
            {
                // Transport error - trigger self-deletion
                trigger_self_destruction();
                CO_RETURN error::TRANSPORT_ERROR();
            }
        }

        if (build_caller_channel)
        {
            caller_transport = get_directional_transport(caller_zone_id.as_destination());
            if (!caller_transport)
            {
                CO_RETURN error::ZONE_NOT_FOUND();
            }
            // Check transport status before routing
            if (caller_transport->get_status() != transport_status::CONNECTED)
            {
                // Transport error - trigger self-deletion
                trigger_self_destruction();
                CO_RETURN error::TRANSPORT_ERROR();
            }
        }

        // Check if we're in the process of disconnecting
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Increment function count to track this active call
        function_count_.fetch_add(1, std::memory_order_acq_rel);

        if (build_dest_channel)
        {
            // Forward the add_ref call to the target transport
            auto result = CO_AWAIT destination_transport->add_ref(protocol_version,
                destination_zone_id,
                object_id,
                caller_zone_id,
                known_direction_zone_id,
                build_out_param_channel & ~add_ref_options::build_caller_route,
                in_back_channel,
                out_back_channel);

            // ONLY increment pass_through reference count if the forward succeeded
            // This ensures our count matches the actual established references
            if (result != error::OK())
            {
                // Decrement function count after completing the call
                function_count_.fetch_sub(1, std::memory_order_acq_rel);
                trigger_self_destruction();
                CO_RETURN result;
            }
        }

        if (build_caller_channel)
        {
            // Forward the add_ref call to the target transport
            auto result = CO_AWAIT caller_transport->add_ref(protocol_version,
                destination_zone_id,
                object_id,
                caller_zone_id,
                known_direction_zone_id,
                build_out_param_channel & ~add_ref_options::build_destination_route,
                in_back_channel,
                out_back_channel);

            // ONLY increment pass_through reference count if the forward succeeded
            // This ensures our count matches the actual established references
            if (result != error::OK())
            {
                // Decrement function count after completing the call
                function_count_.fetch_sub(1, std::memory_order_acq_rel);
                trigger_self_destruction();
                CO_RETURN result;
            }
        }
        // Decrement function count after completing the call
        uint64_t remaining_count = function_count_.fetch_sub(1, std::memory_order_acq_rel);

        // Use bitwise AND to check flags, not exact equality
        // because build_out_param_channel may have additional build flags
        if (no_local_add_ref && destination_zone_id.as_caller() == caller_zone_id)
        {
            // this is a passthrough addref and should not be included in either count
        }
        else if (!!(build_out_param_channel & add_ref_options::optimistic))
        {
            optimistic_count_.fetch_add(1, std::memory_order_acq_rel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_pass_through_add_ref(
                    zone_id_, forward_destination_, reverse_destination_, build_out_param_channel, 0, 1);
#endif
        }
        else
        {
            shared_count_.fetch_add(1, std::memory_order_acq_rel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_pass_through_add_ref(
                    zone_id_, forward_destination_, reverse_destination_, build_out_param_channel, 1, 0);
#endif
        }

        // Check if we need to trigger cleanup after call completion
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED && remaining_count == 1)
        {
            trigger_self_destruction();
        }
        else if (remaining_count == 1 && !draining_pending_.load(std::memory_order_acquire))
        {
            CO_AWAIT drain_pending_releases(protocol_version);
        }
        CO_RETURN error::OK();
    }

    CORO_TASK(int)
    pass_through::release(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_INFO("pass_through::release zone={}, fwd={}, rev={}, dest={}, caller={}, options={}",
            zone_id_.get_val(),
            forward_destination_.get_val(),
            reverse_destination_.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            static_cast<uint64_t>(options));

        // Check if we're in the process of disconnecting
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(destination_zone_id);
        if (!target_transport)
        {
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        // Check transport status before routing
        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            // Transport error - trigger self-deletion
            trigger_self_destruction();
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Increment function count to track this active call
        function_count_.fetch_add(1, std::memory_order_acq_rel);

        auto result = CO_AWAIT target_transport->release(
            protocol_version, destination_zone_id, object_id, caller_zone_id, options, in_back_channel, out_back_channel);

        // Decrement function count after completing the call
        uint64_t remaining_count = function_count_.fetch_sub(1, std::memory_order_acq_rel);

        // If the forward failed, trigger cleanup and return the error
        if (result != error::OK())
        {
            trigger_self_destruction();
            CO_RETURN result;
        }

        // Update pass_through reference count ONLY if forward succeeded
        bool should_delete = false;

        // Check for optimistic first, then default to normal (shared_count)
        // This mirrors the add_ref logic and handles normal=0 correctly
        if (!!(options & release_options::optimistic))
        {
            uint64_t prev = optimistic_count_.fetch_sub(1, std::memory_order_acq_rel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_pass_through_release(zone_id_, forward_destination_, reverse_destination_, 0, -1);
#endif
            if (prev == 1 && shared_count_.load(std::memory_order_acquire) == 0)
            {
                should_delete = true;
            }
        }
        else // Default to normal (shared_count) when no optimistic flag
        {
            uint64_t prev = shared_count_.fetch_sub(1, std::memory_order_acq_rel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_pass_through_release(zone_id_, forward_destination_, reverse_destination_, -1, 0);
#endif
            if (prev == 1 && optimistic_count_.load(std::memory_order_acquire) == 0)
            {
                should_delete = true;
            }
        }

        // Trigger self-destruction if counts are zero
        if (should_delete)
        {
            trigger_self_destruction();
        }
        // Check if we need to trigger cleanup after call completion
        else if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED && remaining_count == 1)
        {
            trigger_self_destruction();
        }
        else if (remaining_count == 1 && !draining_pending_.load(std::memory_order_acquire))
        {
            CO_AWAIT drain_pending_releases(protocol_version);
        }

        CO_RETURN result;
    }

    CORO_TASK(void)
    pass_through::object_released(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        // Check if we're in the process of disconnecting
        if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED)
        {
            CO_RETURN;
        }

        // Increment function count to track this active call
        function_count_.fetch_add(1, std::memory_order_acq_rel);

        // In the case of object_released, the notification goes to the caller side
        // Determine target transport based on caller_zone (reverse direction)
        auto target_transport = get_directional_transport(caller_zone_id.as_destination());
        if (target_transport)
        {
            // Check transport status before routing
            if (target_transport->get_status() != transport_status::CONNECTED)
            {
                // Transport error - trigger self-destruction
                function_count_.fetch_sub(1, std::memory_order_acq_rel);
                trigger_self_destruction();
                CO_RETURN;
            }

            // Forward the call directly to the transport
            CO_AWAIT target_transport->object_released(
                protocol_version, destination_zone_id, object_id, caller_zone_id, in_back_channel);
        }

        // Decrement function count after completing the call
        uint64_t remaining_count = function_count_.fetch_sub(1, std::memory_order_acq_rel);

        // Decrement optimistic count by one and trigger garbage collection if total count is zero
        uint64_t prev_optimistic = optimistic_count_.fetch_sub(1, std::memory_order_acq_rel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_release(zone_id_, forward_destination_, reverse_destination_, 0, -1);
#endif
        if (prev_optimistic == 1 && shared_count_.load(std::memory_order_acquire) == 0)
        {
            // If no more active functions, trigger cleanup
            if (remaining_count == 1) // We just decremented from 1 to 0
            {
                trigger_self_destruction();
            }
        }
        // Also trigger cleanup if we're in DISCONNECTED state and no more functions
        else if (status_.load(std::memory_order_acquire) == pass_through_status::DISCONNECTED && remaining_count == 1)
        {
            trigger_self_destruction();
        }
        else if (remaining_count == 1 && !draining_pending_.load(std::memory_order_acquire))
        {
            CO_AWAIT drain_pending_releases(protocol_version);
        }
    }

    CORO_TASK(void)
    pass_through::transport_down(uint64_t protocol_version,
        destination_zone destination_zone_id,
        caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(destination_zone_id);
        if (target_transport)
        {
            // Notify the target transport first
            CO_AWAIT target_transport->transport_down(
                protocol_version, destination_zone_id, caller_zone_id, in_back_channel);
        }

        // Change state to DISCONNECTED to prevent new calls from starting
        status_.store(pass_through_status::DISCONNECTED, std::memory_order_release);

        // Check if there are no active functions - if so, trigger cleanup immediately
        if (function_count_.load(std::memory_order_acquire) == 0)
        {
            trigger_self_destruction();
        }
    }

    void pass_through::trigger_self_destruction()
    {
        // Change status to DISCONNECTED to prevent new calls from starting
        auto old_status = status_.exchange(pass_through_status::DISCONNECTED, std::memory_order_acq_rel);

        // If we were already disconnected, nothing more to do
        if (old_status == pass_through_status::DISCONNECTED)
        {
            return;
        }

        RPC_INFO(
            "pass_through: deleting, zone={}, forward_dest={}, reverse_dest={}, shared={}, optimistic={}, active={}",
            zone_id_.get_val(),
            forward_destination_.get_val(),
            reverse_destination_.get_val(),
            shared_count_.load(),
            optimistic_count_.load(),
            function_count_.load());
        RPC_INFO(
            "pass_through: deleting, zone={}, forward_dest={}, reverse_dest={}, shared={}, optimistic={}, active={}",
            zone_id_.get_val(),
            forward_destination_.get_val(),
            reverse_destination_.get_val(),
            shared_count_.load(),
            optimistic_count_.load(),
            function_count_.load());

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_deletion(zone_id_, forward_destination_, reverse_destination_);
#endif

        // Check if there are any active functions running
        uint64_t current_function_count = function_count_.load(std::memory_order_acquire);
        if (current_function_count > 0)
        {
            // There are active functions, cleanup will happen when function_count_ reaches 0
            return;
        }

        // No active functions, perform cleanup now
        // Remove destinations from transports in BOTH directions
        if (forward_transport_)
        {
            forward_transport_->remove_passthrough(forward_destination_, reverse_destination_);
        }
        if (reverse_transport_)
        {
            reverse_transport_->remove_passthrough(reverse_destination_, forward_destination_);
        }

        // Release transport and service pointers
        forward_transport_.reset();
        reverse_transport_.reset();
        service_.reset();

        // Release self-reference - this will delete the pass_through when last external reference is gone
        self_ref_.reset();
    }

} // namespace rpc
