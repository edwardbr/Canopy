/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/rpc.h>

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

    pass_through::~pass_through()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_deletion(zone_id_, forward_destination_, reverse_destination_);
#endif
    }

    std::shared_ptr<transport> pass_through::get_directional_transport(destination_zone dest)
    {
        if (dest.get_subnet() == forward_destination_.get_subnet())
        {
            return forward_transport_;
        }
        else if (dest.get_subnet() == reverse_destination_.get_subnet())
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
        remote_object remote_object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        if (!begin_call())
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            end_call();
            trigger_self_destruction();
            CO_RETURN error::TRANSPORT_ERROR();
        }

        auto result = CO_AWAIT target_transport->send(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            remote_object_id,
            interface_id,
            method_id,
            in_data,
            out_buf_,
            in_back_channel,
            out_back_channel);

        end_call();

        if (result == error::TRANSPORT_ERROR())
        {
            trigger_self_destruction();
        }

        CO_RETURN result;
    }

    CORO_TASK(void)
    pass_through::post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        if (!begin_call())
        {
            CO_RETURN;
        }

        auto target_transport = get_directional_transport(remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN;
        }

        CO_AWAIT target_transport->post(
            protocol_version, encoding, tag, caller_zone_id, remote_object_id, interface_id, method_id, in_data, in_back_channel);

        end_call();
    }

    CORO_TASK(int)
    pass_through::try_cast(uint64_t protocol_version,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
        interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        if (!begin_call())
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        auto target_transport = get_directional_transport(remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            end_call();
            trigger_self_destruction();
            CO_RETURN error::TRANSPORT_ERROR();
        }

        auto result = CO_AWAIT target_transport->try_cast(
            protocol_version, caller_zone_id, remote_object_id, interface_id, in_back_channel, out_back_channel);

        end_call();

        if (result == error::TRANSPORT_ERROR())
        {
            trigger_self_destruction();
        }

        CO_RETURN result;
    }

    CORO_TASK(int)
    pass_through::add_ref(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        requesting_zone requesting_zone_id,
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

        RPC_DEBUG("pass_through::add_ref zone={}, fwd={}, rev={}, dest={}, caller={}, options={}, build_dest={}, "
                  "build_caller={}, no_local={}",
            zone_id_.get_subnet(),
            forward_destination_.get_subnet(),
            reverse_destination_.get_subnet(),
            remote_object_id.get_subnet(),
            caller_zone_id.get_subnet(),
            static_cast<uint64_t>(build_out_param_channel),
            build_dest_channel,
            build_caller_channel,
            no_local_add_ref);

        // Determine target transport based on destination_zone
        if (build_dest_channel)
        {
            destination_transport = get_directional_transport(remote_object_id.as_zone());
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
            caller_transport = get_directional_transport(caller_zone_id);
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

        if (!begin_call())
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        if (build_dest_channel)
        {
            auto result = CO_AWAIT destination_transport->add_ref(protocol_version,
                remote_object_id,
                caller_zone_id,
                requesting_zone_id,
                build_out_param_channel & ~add_ref_options::build_caller_route,
                in_back_channel,
                out_back_channel);

            if (result != error::OK())
            {
                end_call();
                trigger_self_destruction();
                CO_RETURN result;
            }
        }

        if (build_caller_channel)
        {
            auto result = CO_AWAIT caller_transport->add_ref(protocol_version,
                remote_object_id,
                caller_zone_id,
                requesting_zone_id,
                build_out_param_channel & ~add_ref_options::build_destination_route,
                in_back_channel,
                out_back_channel);

            if (result != error::OK())
            {
                end_call();
                trigger_self_destruction();
                CO_RETURN result;
            }
        }

        // Use bitwise AND to check flags, not exact equality
        // because build_out_param_channel may have additional build flags
        if (no_local_add_ref && remote_object_id == caller_zone_id)
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

        end_call();
        CO_RETURN error::OK();
    }

    CORO_TASK(int)
    pass_through::release(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        RPC_DEBUG("pass_through::release zone={}, fwd={}, rev={}, dest={}, caller={}, options={}",
            zone_id_.get_subnet(),
            forward_destination_.get_subnet(),
            reverse_destination_.get_subnet(),
            remote_object_id.get_subnet(),
            caller_zone_id.get_subnet(),
            static_cast<uint64_t>(options));

        if (!begin_call())
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        auto target_transport = get_directional_transport(remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            end_call();
            trigger_self_destruction();
            CO_RETURN error::TRANSPORT_ERROR();
        }

        auto result = CO_AWAIT target_transport->release(
            protocol_version, remote_object_id, caller_zone_id, options, in_back_channel, out_back_channel);

        end_call();

        if (result != error::OK())
        {
            trigger_self_destruction();
            CO_RETURN result;
        }

        // Update pass_through RAII reference count only on success.
        bool should_delete = false;

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
        else
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

        if (should_delete)
        {
            trigger_self_destruction();
        }

        CO_RETURN result;
    }

    CORO_TASK(void)
    pass_through::object_released(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        // Check if we're in the process of disconnecting
        if (!begin_call())
        {
            CO_RETURN;
        }

        // The notification goes to the caller side (reverse direction).
        auto target_transport = get_directional_transport(caller_zone_id);
        if (target_transport)
        {
            if (target_transport->get_status() != transport_status::CONNECTED)
            {
                end_call();
                trigger_self_destruction();
                CO_RETURN;
            }

            CO_AWAIT target_transport->object_released(protocol_version, remote_object_id, caller_zone_id, in_back_channel);
        }

        end_call();

        // Decrement the optimistic RAII count and trigger destruction if both counts hit zero.
        uint64_t prev_optimistic = optimistic_count_.fetch_sub(1, std::memory_order_acq_rel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_release(zone_id_, forward_destination_, reverse_destination_, 0, -1);
#endif
        if (prev_optimistic == 1 && shared_count_.load(std::memory_order_acquire) == 0)
        {
            trigger_self_destruction();
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

        // Atomically mark shutdown. do_cleanup() will run either immediately (no
        // active calls) or when the last in-flight call's end_call() fires.
        trigger_self_destruction();
    }

    CORO_TASK(int)
    pass_through::get_new_zone_id(uint64_t protocol_version,
        zone& zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        std::ignore = protocol_version;
        std::ignore = zone_id;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;
        CO_RETURN rpc::error::ZONE_NOT_SUPPORTED();
    }

    CORO_TASK(void)
    pass_through::local_transport_down(const std::shared_ptr<transport>& local_transport)
    {
        if (forward_transport_ != local_transport)
        {
            CO_AWAIT forward_transport_->transport_down(rpc::get_version(), forward_destination_, reverse_destination_, {});
        }
        if (reverse_transport_)
        {
            CO_AWAIT reverse_transport_->transport_down(rpc::get_version(), reverse_destination_, forward_destination_, {});
        }
        trigger_self_destruction();
    }

    bool pass_through::begin_call()
    {
        // Atomically check SHUTDOWN_BIT and increment the active-call counter together.
        // If SHUTDOWN_BIT is already set the increment is rolled back and the call is rejected.
        uint64_t prev = combined_.fetch_add(1, std::memory_order_acq_rel);
        if (prev & SHUTDOWN_BIT)
        {
            combined_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    void pass_through::end_call()
    {
        // Decrement the active-call counter.  If we were the last active call while
        // SHUTDOWN_BIT is set, trigger the one-time cleanup.
        uint64_t prev = combined_.fetch_sub(1, std::memory_order_acq_rel);
        if ((prev & SHUTDOWN_BIT) && ((prev & ~SHUTDOWN_BIT) == 1))
        {
            do_cleanup();
        }
    }

    void pass_through::trigger_self_destruction()
    {
        // Atomically set SHUTDOWN_BIT.  If it was already set someone else is handling
        // shutdown, so we have nothing to do.
        uint64_t prev = combined_.fetch_or(SHUTDOWN_BIT, std::memory_order_acq_rel);
        if (prev & SHUTDOWN_BIT)
        {
            return;
        }

        RPC_DEBUG("pass_through: trigger_self_destruction for passthrough {}->{}, zone={}, shared={}, optimistic={}, "
                  "active={}",
            reverse_destination_.get_subnet(),
            forward_destination_.get_subnet(),
            zone_id_.get_subnet(),
            shared_count_.load(),
            optimistic_count_.load(),
            prev & ~SHUTDOWN_BIT);

        // If there are no active calls, perform cleanup immediately.
        // Otherwise end_call() will call do_cleanup() when the last call exits.
        if ((prev & ~SHUTDOWN_BIT) == 0)
        {
            do_cleanup();
        }
    }

    void pass_through::do_cleanup()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_pass_through_deletion(zone_id_, forward_destination_, reverse_destination_);
#endif

        // Remove this passthrough from both transports so no new calls are routed here.
        if (forward_transport_)
        {
            forward_transport_->remove_passthrough(forward_destination_, reverse_destination_);
        }
        if (reverse_transport_)
        {
            reverse_transport_->remove_passthrough(reverse_destination_, forward_destination_);
        }

        // Release all strong references.
        forward_transport_.reset();
        reverse_transport_.reset();
        service_.reset();

        // Drop the self-reference; the pass_through is deleted once all external
        // shared_ptrs to it are also released.
        self_ref_.reset();
    }

} // namespace rpc
