/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/rpc.h>

namespace rpc
{

    pass_through::pass_through(
        std::shared_ptr<transport> forward,
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
            telemetry_service->on_pass_through_creation(
                zone_id_,
                forward_dest,
                reverse_dest,
                shared_count_.load(std::memory_order_acquire),
                optimistic_count_.load(std::memory_order_acquire));
#endif
    }

    std::shared_ptr<pass_through> pass_through::create(
        std::shared_ptr<transport> forward,
        std::shared_ptr<transport> reverse,
        std::shared_ptr<service> service,
        destination_zone forward_dest,
        destination_zone reverse_dest)
    {
        std::shared_ptr<pass_through> pt(new rpc::pass_through(
            forward, // forward_transport: handles messages TO final destination
            reverse, // reverse_transport: handles messages back to caller
            service, // service
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

    CORO_TASK(send_result)
    pass_through::send(send_params params)
    {
        if (!begin_call())
        {
            CO_RETURN send_result{error::TRANSPORT_ERROR(), {}, {}};
        }

        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(params.remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN send_result{error::ZONE_NOT_FOUND(), {}, {}};
        }

        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            end_call();
            trigger_self_destruction();
            CO_RETURN send_result{error::TRANSPORT_ERROR(), {}, {}};
        }

        auto result = CO_AWAIT target_transport->send(std::move(params));

        end_call();

        if (error::is_error(result.error_code))
        {
            trigger_self_destruction();
        }

        CO_RETURN result;
    }

    CORO_TASK(void)
    pass_through::post(post_params params)
    {
        if (!begin_call())
        {
            CO_RETURN;
        }

        auto target_transport = get_directional_transport(params.remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN;
        }

        CO_AWAIT target_transport->post(std::move(params));

        end_call();
    }

    CORO_TASK(standard_result)
    pass_through::try_cast(try_cast_params params)
    {
        if (!begin_call())
        {
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        auto target_transport = get_directional_transport(params.remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
        }

        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            end_call();
            trigger_self_destruction();
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        auto result = CO_AWAIT target_transport->try_cast(std::move(params));

        end_call();

        if (error::is_critical(result.error_code))
        {
            trigger_self_destruction();
        }

        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    pass_through::add_ref(add_ref_params params)
    {
        add_ref_options build_out_param_channel = params.build_out_param_channel;
        remote_object remote_object_id = params.remote_object_id;
        caller_zone caller_zone_id = params.caller_zone_id;
        requesting_zone requesting_zone_id = params.requesting_zone_id;

        bool no_local_add_ref = !!(build_out_param_channel & add_ref_options::build_caller_route)
                                && !!(build_out_param_channel & add_ref_options::build_destination_route);

        bool build_caller_channel = !!(build_out_param_channel & add_ref_options::build_caller_route);
        bool build_dest_channel = !!(build_out_param_channel & add_ref_options::build_destination_route)
                                  || build_out_param_channel == add_ref_options::normal
                                  || build_out_param_channel == add_ref_options::optimistic;

        std::shared_ptr<rpc::transport> caller_transport;
        std::shared_ptr<rpc::transport> destination_transport;

        RPC_DEBUG(
            "pass_through::add_ref zone={}, fwd={}, rev={}, dest={}, caller={}, options={}, build_dest={}, "
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
                CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
            }
            // Check transport status before routing
            if (destination_transport->get_status() != transport_status::CONNECTED)
            {
                // Transport error - trigger self-deletion
                trigger_self_destruction();
                CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
            }
        }

        if (build_caller_channel)
        {
            caller_transport = get_directional_transport(caller_zone_id);
            if (!caller_transport)
            {
                CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
            }
            // Check transport status before routing
            if (caller_transport->get_status() != transport_status::CONNECTED)
            {
                // Transport error - trigger self-deletion
                trigger_self_destruction();
                CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
            }
        }

        if (!begin_call())
        {
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        // We build the result by merging out_back_channels from both calls
        standard_result final_result{error::OK(), {}};

        if (build_dest_channel)
        {
            add_ref_params dest_params;
            dest_params.protocol_version = params.protocol_version;
            dest_params.remote_object_id = remote_object_id;
            dest_params.caller_zone_id = caller_zone_id;
            dest_params.requesting_zone_id = requesting_zone_id;
            dest_params.build_out_param_channel = build_out_param_channel & ~add_ref_options::build_caller_route;
            dest_params.in_back_channel = params.in_back_channel;
            dest_params.request_id = params.request_id;

            auto dest_result = CO_AWAIT destination_transport->add_ref(std::move(dest_params));

            if (dest_result.error_code != error::OK())
            {
                end_call();
                trigger_self_destruction();
                CO_RETURN dest_result;
            }
            final_result.out_back_channel = std::move(dest_result.out_back_channel);
        }

        if (build_caller_channel)
        {
            add_ref_params caller_params;
            caller_params.protocol_version = params.protocol_version;
            caller_params.remote_object_id = remote_object_id;
            caller_params.caller_zone_id = caller_zone_id;
            caller_params.requesting_zone_id = requesting_zone_id;
            caller_params.build_out_param_channel = build_out_param_channel & ~add_ref_options::build_destination_route;
            caller_params.in_back_channel = params.in_back_channel;
            caller_params.request_id = params.request_id;

            auto caller_result = CO_AWAIT caller_transport->add_ref(std::move(caller_params));

            if (caller_result.error_code != error::OK())
            {
                end_call();
                trigger_self_destruction();
                CO_RETURN caller_result;
            }
            // merge out_back_channels
            final_result.out_back_channel.insert(
                final_result.out_back_channel.end(),
                std::make_move_iterator(caller_result.out_back_channel.begin()),
                std::make_move_iterator(caller_result.out_back_channel.end()));
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
        CO_RETURN final_result;
    }

    CORO_TASK(standard_result)
    pass_through::release(release_params params)
    {
        remote_object remote_object_id = params.remote_object_id;
        [[maybe_unused]] caller_zone caller_zone_id = params.caller_zone_id;
        release_options options = params.options;

        RPC_DEBUG(
            "pass_through::release zone={}, fwd={}, rev={}, dest={}, caller={}, options={}",
            zone_id_.get_subnet(),
            forward_destination_.get_subnet(),
            reverse_destination_.get_subnet(),
            remote_object_id.get_subnet(),
            caller_zone_id.get_subnet(),
            static_cast<uint64_t>(options));

        if (!begin_call())
        {
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        auto target_transport = get_directional_transport(remote_object_id.as_zone());
        if (!target_transport)
        {
            end_call();
            CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
        }

        if (target_transport->get_status() != transport_status::CONNECTED)
        {
            end_call();
            trigger_self_destruction();
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        auto result = CO_AWAIT target_transport->release(std::move(params));

        end_call();

        if (result.error_code != error::OK())
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
    pass_through::object_released(object_released_params params)
    {
        caller_zone caller_zone_id = params.caller_zone_id;

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

            CO_AWAIT target_transport->object_released(std::move(params));
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
    pass_through::transport_down(transport_down_params params)
    {
        destination_zone destination_zone_id = params.destination_zone_id;

        // Determine target transport based on destination_zone
        auto target_transport = get_directional_transport(destination_zone_id);
        if (target_transport)
        {
            // Notify the target transport first
            CO_AWAIT target_transport->transport_down(std::move(params));
        }

        // Atomically mark shutdown. do_cleanup() will run either immediately (no
        // active calls) or when the last in-flight call's end_call() fires.
        trigger_self_destruction();
    }

    CORO_TASK(new_zone_id_result)
    pass_through::get_new_zone_id(get_new_zone_id_params params)
    {
        std::ignore = params;
        CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_SUPPORTED(), {}, {}};
    }

    CORO_TASK(void)
    pass_through::local_transport_down(std::shared_ptr<transport> local_transport)
    {
        if (forward_transport_ != local_transport)
        {
            transport_down_params fwd_params;
            fwd_params.protocol_version = rpc::get_version();
            fwd_params.destination_zone_id = forward_destination_;
            fwd_params.caller_zone_id = reverse_destination_;
            CO_AWAIT forward_transport_->transport_down(std::move(fwd_params));
        }
        if (reverse_transport_)
        {
            transport_down_params rev_params;
            rev_params.protocol_version = rpc::get_version();
            rev_params.destination_zone_id = reverse_destination_;
            rev_params.caller_zone_id = forward_destination_;
            CO_AWAIT reverse_transport_->transport_down(std::move(rev_params));
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

        RPC_DEBUG(
            "pass_through: trigger_self_destruction for passthrough {}->{}, zone={}, shared={}, optimistic={}, "
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
