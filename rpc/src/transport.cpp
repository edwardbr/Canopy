/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>
#include <mutex>
#include <memory>

// RPC headers
#include <rpc/rpc.h>

namespace rpc
{
    // NOTE: The local service MUST be registered in pass_thoughs_ map
    // during transport initialization:
    //   pass_thoughs_[local_zone][local_zone] = service
    //
    // Pass-throughs are registered in BOTH directions:
    //   pass_thoughs_[A][B] = pass_through  (for A↔B communication)
    //   pass_thoughs_[B][A] = pass_through  (same pass-through)
    //
    // This provides O(1) lookup for all routing scenarios.
    // Destination management

    transport::transport(std::string name, std::shared_ptr<service> service, zone adjacent_zone_id)
        : name_(name)
        , zone_id_(service->get_zone_id())
        , adjacent_zone_id_(adjacent_zone_id)
        , service_(service)
    {
        // Register local service: pass_thoughs_[local][local] = service
        // auto local_zone = zone_id_.get_val();
        // pass_thoughs_[local_zone][adjacent_zone_id.get_val()] = std::static_pointer_cast<i_marshaller>(service);

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_creation(
                name_, zone_id_, adjacent_zone_id_, status_.load(std::memory_order_acquire));
#endif
    }

    transport::transport(std::string name, zone zone_id_, zone adjacent_zone_id)
        : name_(name)
        , zone_id_(zone_id_)
        , adjacent_zone_id_(adjacent_zone_id)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_creation(
                name_, zone_id_, adjacent_zone_id_, status_.load(std::memory_order_acquire));
#endif
    }

    transport::~transport()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_deletion(zone_id_, adjacent_zone_id_);
#endif
    }

    void transport::set_service(std::shared_ptr<service> service)
    {
        RPC_ASSERT(service);
        service_ = service;
    }

    bool transport::inner_add_destination(destination_zone dest, caller_zone caller, std::weak_ptr<i_marshaller> handler)
    {
        // note this is protected by a mutex in the caller
        auto dest_val = dest.get_val();
        auto caller_val = caller.get_val();

        // Check if entry already exists
        auto outer_it = pass_thoughs_.find(dest_val);
        if (outer_it != pass_thoughs_.end())
        {
            auto inner_it = outer_it->second.find(caller_val);
            if (inner_it != outer_it->second.end())
            {
                RPC_ASSERT(false);
                return false; // Already exists
            }
        }

        // Add entry
        pass_thoughs_[dest_val][caller_val] = handler;
        inner_increment_outbound_proxy_count(caller.as_destination());

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_add_destination(zone_id_, adjacent_zone_id_, dest, caller);
#endif

        return true;
    }

    bool transport::add_destination(destination_zone dest, caller_zone caller, std::weak_ptr<i_marshaller> handler)
    {
        std::unique_lock lock(destinations_mutex_);
        return inner_add_destination(dest, caller, handler);
    }

    void transport::increment_outbound_proxy_count(destination_zone dest)
    {
        std::unique_lock lock(destinations_mutex_);
        return inner_increment_outbound_proxy_count(dest);
    }
    void transport::decrement_outbound_proxy_count(destination_zone dest)
    {
        std::unique_lock lock(destinations_mutex_);
        return inner_decrement_outbound_proxy_count(dest);
    }

    void transport::increment_inbound_stub_count(caller_zone dest)
    {
        std::unique_lock lock(destinations_mutex_);
        return inner_increment_inbound_stub_count(dest);
    }
    void transport::decrement_inbound_stub_count(caller_zone dest)
    {
        std::unique_lock lock(destinations_mutex_);
        return inner_decrement_inbound_stub_count(dest);
    }

    void transport::inner_increment_outbound_proxy_count(destination_zone dest)
    {
        ++destination_count_;
        RPC_DEBUG("increment_outbound_proxy_count {} -> {} count = {}",
            zone_id_.get_val(),
            dest.get_val(),
            get_destination_count());
        auto found = outbound_proxy_count_.find(dest);
        if (found == outbound_proxy_count_.end())
        {
            outbound_proxy_count_[dest] = 1;
        }
        else
        {
            found->second++;
        }
    }
    void transport::inner_decrement_outbound_proxy_count(destination_zone dest)
    {
        auto found = outbound_proxy_count_.find(dest);
        if (found == outbound_proxy_count_.end())
        {
            RPC_WARNING("inner_decrement_outbound_proxy_count: No outbound proxy count found for dest={} in zone={}",
                dest.get_val(),
                zone_id_.get_val());
        }
        else
        {
            --destination_count_;

            RPC_DEBUG("decrement_outbound_proxy_count {} -> {} count = {}",
                zone_id_.get_val(),
                dest.get_val(),
                get_destination_count());
            auto count = --found->second;
            if (count == 0)
            {
                outbound_proxy_count_.erase(found);
            }
        }
    }

    void transport::inner_increment_inbound_stub_count(caller_zone dest)
    {
        ++destination_count_;
        RPC_DEBUG(
            "increment_inbound_stub_count {} -> {} count = {}", zone_id_.get_val(), dest.get_val(), get_destination_count());

        auto found = inbound_stub_count_.find(dest);
        if (found == inbound_stub_count_.end())
        {
            inbound_stub_count_[dest] = 1;
        }
        else
        {
            found->second++;
        }
    }
    void transport::inner_decrement_inbound_stub_count(caller_zone dest)
    {
        auto found = inbound_stub_count_.find(dest);
        if (found == inbound_stub_count_.end())
        {
            RPC_WARNING("inner_decrement_outbound_proxy_count: No outbound proxy count found for dest={}", dest.get_val());
        }
        else
        {
            --destination_count_;

            RPC_DEBUG("decrement_outbound_proxy_count {} -> {} count = {}",
                zone_id_.get_val(),
                dest.get_val(),
                get_destination_count());

            auto count = --found->second;
            if (count == 0)
            {
                inbound_stub_count_.erase(found);
            }
        }
    }

    void transport::remove_destination(destination_zone dest, caller_zone caller)
    {
        std::unique_lock lock(destinations_mutex_);
        auto dest_val = dest.get_val();
        auto caller_val = caller.get_val();

        auto outer_it = pass_thoughs_.find(dest_val);
        if (outer_it != pass_thoughs_.end())
        {
            outer_it->second.erase(caller_val);
            // Clean up outer map if inner map is empty
            if (outer_it->second.empty())
            {
                pass_thoughs_.erase(outer_it);
            }
        }

        inner_decrement_outbound_proxy_count(caller.as_destination());

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_remove_destination(zone_id_, adjacent_zone_id_, dest, caller);
#endif
    }

    std::shared_ptr<i_marshaller> transport::create_pass_through(std::shared_ptr<transport> forward,
        const std::shared_ptr<transport>& reverse,
        const std::shared_ptr<service>& service,
        destination_zone forward_dest,
        destination_zone reverse_dest)
    {
        // Validate: pass-through should only be created between different zones
        if (forward_dest == reverse_dest)
        {
            RPC_ERROR("create_pass_through: Invalid pass-through request - forward_dest and reverse_dest are the same "
                      "({})! Pass-throughs should only route between different zones.",
                forward_dest.get_val());
            return nullptr;
        }

        // Validate: forward and reverse transports should be different objects
        if (forward.get() == reverse.get())
        {
            RPC_ERROR("create_pass_through: Invalid pass-through request - forward and reverse transports are the same "
                      "object! forward_dest={}, reverse_dest={}",
                forward_dest.get_val(),
                reverse_dest.get_val());
            return nullptr;
        }

        std::shared_ptr<pass_through> pt(
            new rpc::pass_through(forward, // forward_transport: handles messages TO final destination
                reverse,                   // reverse_transport: handles messages back to caller
                service,                   // service
                forward_dest,
                reverse_dest // reverse_destination: where reverse messages go
                ));
        pt->self_ref_ = pt; // keep self alive based on reference counts

        // we need to lock both transports destination mutexes without deadlock when adding the destinations
        // we do this by locking them in zone id order
        std::unique_ptr<std::lock_guard<std::shared_mutex>> g1;
        std::unique_ptr<std::lock_guard<std::shared_mutex>> g2;

        if (forward->get_adjacent_zone_id() < reverse->get_adjacent_zone_id())
        {
            g1 = std::make_unique<std::lock_guard<std::shared_mutex>>(forward->destinations_mutex_);
            g2 = std::make_unique<std::lock_guard<std::shared_mutex>>(reverse->destinations_mutex_);
        }
        else
        {
            g1 = std::make_unique<std::lock_guard<std::shared_mutex>>(reverse->destinations_mutex_);
            g2 = std::make_unique<std::lock_guard<std::shared_mutex>>(forward->destinations_mutex_);
        }

        // Check if pass-through already exists for this zone pair
        auto forward_handler = forward->inner_get_destination_handler(reverse_dest, forward_dest.as_caller());
        auto reverse_handler = reverse->inner_get_destination_handler(forward_dest, reverse_dest.as_caller());

        // check that they are the same
        RPC_ASSERT(!forward_handler == !reverse_handler);

        if (forward_handler)
        {
            RPC_DEBUG("create_pass_through: Found existing pass-through for forward_dest={}, reverse_dest={}",
                forward_dest.get_val(),
                reverse_dest.get_val());
            return forward_handler;
        }
        else
        {
            RPC_DEBUG("create_pass_through: Creating NEW pass-through, forward_dest={}, reverse_dest={}, pt={}",
                forward_dest.get_val(),
                reverse_dest.get_val(),
                (void*)pt.get());
            // Register pass-through on both transports
            // inner_add_destination automatically registers both directions for pass-throughs
            forward->inner_add_destination(
                reverse_dest, forward_dest.as_caller(), std::static_pointer_cast<i_marshaller>(pt));
            reverse->inner_add_destination(
                forward_dest, reverse_dest.as_caller(), std::static_pointer_cast<i_marshaller>(pt));

            // Note: We do NOT register forward_dest in service->transports here because:
            // 1. The pass-through might fail to reach the destination (zone doesn't exist downstream)
            // 2. Registering it would create routing loops
            // 3. The registration should happen after successful routing, by the caller

            return std::static_pointer_cast<i_marshaller>(pt);
        }
        return pt;
    }

    // Status management
    transport_status transport::get_status() const
    {
        return status_.load(std::memory_order_acquire);
    }

    void transport::set_status(transport_status new_status)
    {
        [[maybe_unused]] auto old_status = status_.load(std::memory_order_acquire);
        status_.store(new_status, std::memory_order_release);

#ifdef CANOPY_USE_TELEMETRY
        if (old_status != new_status)
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_status_change(name_, zone_id_, adjacent_zone_id_, old_status, new_status);
        }
#endif
    }

    std::shared_ptr<i_marshaller> transport::inner_get_destination_handler(destination_zone dest, caller_zone caller) const
    {
        auto dest_val = dest.get_val();
        auto caller_val = caller.get_val();

        // O(1) nested map lookup
        auto outer_it = pass_thoughs_.find(dest_val);
        if (outer_it != pass_thoughs_.end())
        {
            auto inner_it = outer_it->second.find(caller_val);
            if (inner_it != outer_it->second.end())
            {
                auto handler = inner_it->second.lock();
                if (!handler)
                {
                    RPC_WARNING("inner_get_destination_handler: weak_ptr expired for dest={}, caller={} on transport "
                                "zone={} adjacent_zone={}",
                        dest_val,
                        caller_val,
                        zone_id_.get_val(),
                        adjacent_zone_id_.get_val());
                }
                return handler;
            }
        }
        return nullptr;
    }

    // Helper to route incoming messages to registered handlers
    std::shared_ptr<i_marshaller> transport::get_destination_handler(destination_zone dest, caller_zone caller) const
    {
        if (dest == zone_id_.as_destination())
        {
            RPC_DEBUG("get_destination_handler: Requested destination is local zone {}, returning local service",
                zone_id_.get_val());
            return service_.lock();
        }
        std::shared_lock lock(destinations_mutex_);
        auto handler = inner_get_destination_handler(dest, caller);
        RPC_DEBUG("get_destination_handler: dest={}, caller={}, transport zone={}, adjacent_zone={}, found={}",
            dest.get_val(),
            caller.get_val(),
            zone_id_.get_val(),
            adjacent_zone_id_.get_val(),
            handler != nullptr);
        return handler;
    }

    // Find any pass-through that has the specified destination, regardless of caller
    // O(1) lookup: just get pass_thoughs_[dest] and return first non-expired entry
    std::shared_ptr<i_marshaller> transport::inner_find_any_passthrough_for_destination(destination_zone dest) const
    {
        auto outer_it = pass_thoughs_.find(dest);
        if (outer_it != pass_thoughs_.end())
        {
            // Iterate through all callers for this destination
            for (const auto& [caller_val, handler_weak] : outer_it->second)
            {
                auto handler = handler_weak.lock();
                if (handler)
                {
                    RPC_DEBUG(
                        "inner_find_any_passthrough_for_destination: Found pass-through for dest={} with caller={}",
                        dest.get_val(),
                        caller_val.get_val());
                    return handler;
                }
            }
        }
        return nullptr;
    }

    std::shared_ptr<i_marshaller> transport::find_any_passthrough_for_destination(destination_zone dest) const
    {
        std::shared_lock lock(destinations_mutex_);
        return inner_find_any_passthrough_for_destination(dest);
    }

    void transport::notify_all_destinations_of_disconnect()
    {
        std::shared_lock lock(destinations_mutex_);
#ifdef CANOPY_BUILD_COROUTINE
        auto service = service_.lock();
        if (!service)
        {
            RPC_ERROR("notify_all_destinations_of_disconnect: Local service no longer exists on transport zone={} "
                      "adjacent_zone={}",
                zone_id_.get_val(),
                adjacent_zone_id_.get_val());
            return;
        }
#endif
        // Iterate through nested map to notify all handlers
        for (const auto& [dest_zone, inner_map] : pass_thoughs_)
        {
            for (const auto& [caller_zone_val, handler_weak] : inner_map)
            {
                if (auto handler = handler_weak.lock())
                {
#ifdef CANOPY_BUILD_COROUTINE
                    if (!service->spawn(
#endif
                            // Send zone_terminating post
                            handler->transport_down(VERSION_3, rpc::destination_zone{dest_zone}, rpc::caller_zone{0}, {})
#ifdef CANOPY_BUILD_COROUTINE
                                ))
                    {
                        RPC_ERROR(
                            "notify_all_destinations_of_disconnect: Failed to spawn coroutine to notify handler of "
                            "zone termination for dest_zone={} caller_zone={} on transport zone={} adjacent_zone={}",
                            dest_zone.get_val(),
                            caller_zone_val.get_val(),
                            zone_id_.get_val(),
                            adjacent_zone_id_.get_val());
                        RPC_ASSERT(false);
                    }
#endif
                    ;
                }
            }
        }
    }

    // inbound i_marshaller interface implementation
    // Routes via pass_thoughs_ map (service is registered as a destination)
    CORO_TASK(int)
    transport::inbound_send(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_send(
                zone_id_, adjacent_zone_id_, destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif

        // Check transport status before attempting to route
        if (get_status() == transport_status::DISCONNECTED)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        // Try zone pair lookup first
        auto dest = get_destination_handler(destination_zone_id, caller_zone_id);
        if (!dest)
        {
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        CO_RETURN CO_AWAIT dest->send(protocol_version,
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
    transport::inbound_post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<back_channel_entry>& in_back_channel)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_post(
                zone_id_, adjacent_zone_id_, destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif

        // Try zone pair lookup
        auto dest = get_destination_handler(destination_zone_id, caller_zone_id);
        if (!dest)
        {
            CO_RETURN;
        }

        CO_AWAIT dest->post(protocol_version,
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
    transport::inbound_try_cast(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_try_cast(
                zone_id_, adjacent_zone_id_, destination_zone_id, {0}, object_id, interface_id);
        }
#endif

        auto dest = get_destination_handler(destination_zone_id, {0});
        if (!dest)
        {
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        CO_RETURN CO_AWAIT dest->try_cast(
            protocol_version, destination_zone_id, object_id, interface_id, in_back_channel, out_back_channel);
    }

    CORO_TASK(int)
    transport::inbound_add_ref(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        known_direction_zone known_direction_zone_id,
        add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        reference_count = 0;

        // Check transport status before attempting to route
        if (get_status() == transport_status::DISCONNECTED)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        bool build_caller_channel = !!(build_out_param_channel & add_ref_options::build_caller_route);
        bool build_dest_channel = !!(build_out_param_channel & add_ref_options::build_destination_route)
                                  || build_out_param_channel == add_ref_options::normal
                                  || build_out_param_channel == add_ref_options::optimistic;

        auto svc = service_.lock();
        if (!svc)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        RPC_DEBUG("inbound_add_ref: svc_zone={}, dest_zone={}, caller_zone={}, build_caller_channel={}, "
                  "build_dest_channel={}, known_direction_zone_id={}",
            svc->get_zone_id().get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            build_caller_channel,
            build_dest_channel,
            known_direction_zone_id.get_val());

        if (destination_zone_id != svc->get_zone_id().as_destination() && caller_zone_id != svc->get_zone_id().as_caller())
        {
            auto dest_transport = svc->get_transport(destination_zone_id);
            if (destination_zone_id == caller_zone_id.as_destination())
            {
                // caller and destination are the same zone, so we just call the transport to pass the call along and not involve a pass through
                if (!dest_transport)
                {
                    CO_RETURN error::ZONE_NOT_FOUND();
                }

                // here we
                auto error_code = CO_AWAIT dest_transport->add_ref(protocol_version,
                    destination_zone_id,
                    object_id,
                    caller_zone_id,
                    known_direction_zone_id,
                    build_out_param_channel,
                    reference_count,
                    in_back_channel,
                    out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_transport_inbound_add_ref(zone_id_,
                        adjacent_zone_id_,
                        destination_zone_id,
                        caller_zone_id,
                        object_id,
                        known_direction_zone_id,
                        build_out_param_channel,
                        reference_count);
                }
#endif

                if (error_code != error::OK())
                {
                    CO_RETURN error_code;
                }
            }
            if (!dest_transport)
            {
                if (build_dest_channel)
                {
                    dest_transport = svc->get_transport(known_direction_zone_id.as_destination());
                    if (!dest_transport)
                    {
                        CO_RETURN error::ZONE_NOT_FOUND();
                    }
                }
                else
                {
                    dest_transport = shared_from_this();
                }
                svc->add_transport(destination_zone_id, dest_transport);
            }

            // otherwise we are going to use or create a pass-through
            auto passthrough = dest_transport->get_destination_handler(destination_zone_id, caller_zone_id);
            if (passthrough)
            {
                CO_RETURN CO_AWAIT passthrough->add_ref(protocol_version,
                    destination_zone_id,
                    object_id,
                    caller_zone_id,
                    known_direction_zone_id,
                    build_out_param_channel,
                    reference_count,
                    in_back_channel,
                    out_back_channel);
            }

            auto caller_transport = svc->get_transport(caller_zone_id.as_destination());
            if (!caller_transport)
            {
                if (!build_dest_channel && build_caller_channel)
                {
                    caller_transport = svc->get_transport(known_direction_zone_id.as_destination());
                    if (!dest_transport)
                    {
                        CO_RETURN error::ZONE_NOT_FOUND();
                    }
                }
                else
                {
                    caller_transport = shared_from_this();
                }
                svc->add_transport(caller_zone_id.as_destination(), caller_transport);
            }

            if (dest_transport == caller_transport)
            {
                // here we directly call the destination
                auto error_code = CO_AWAIT dest_transport->add_ref(protocol_version,
                    destination_zone_id,
                    object_id,
                    caller_zone_id,
                    known_direction_zone_id,
                    build_out_param_channel,
                    reference_count,
                    in_back_channel,
                    out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_transport_inbound_add_ref(zone_id_,
                        adjacent_zone_id_,
                        destination_zone_id,
                        caller_zone_id,
                        object_id,
                        known_direction_zone_id,
                        build_out_param_channel,
                        reference_count);
                }
#endif
                CO_RETURN error_code;
            }

            passthrough = transport::create_pass_through(
                dest_transport, caller_transport, svc, destination_zone_id, caller_zone_id.as_destination());

            auto error_code = CO_AWAIT passthrough->add_ref(protocol_version,
                destination_zone_id,
                object_id,
                caller_zone_id,
                known_direction_zone_id,
                build_out_param_channel,
                reference_count,
                in_back_channel,
                out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_inbound_add_ref(zone_id_,
                    adjacent_zone_id_,
                    destination_zone_id,
                    caller_zone_id,
                    object_id,
                    known_direction_zone_id,
                    build_out_param_channel,
                    reference_count);
            }
#endif
            CO_RETURN error_code;
        }

        // else it is a special case that the service needs to deal with

        auto error_code = CO_AWAIT svc->add_ref(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            known_direction_zone_id,
            build_out_param_channel,
            reference_count,
            in_back_channel,
            out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_add_ref(zone_id_,
                adjacent_zone_id_,
                destination_zone_id,
                caller_zone_id,
                object_id,
                known_direction_zone_id,
                build_out_param_channel,
                reference_count);
        }
#endif
        CO_RETURN error_code;
    }

    CORO_TASK(int)
    transport::inbound_release(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        release_options options,
        uint64_t& reference_count,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        reference_count = 0;

        // Try zone pair lookup
        auto dest = get_destination_handler(destination_zone_id, caller_zone_id);
        if (!dest)
        {
            CO_RETURN error::ZONE_NOT_FOUND();
        }

        auto error_code = CO_AWAIT dest->release(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            options,
            reference_count,
            in_back_channel,
            out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_release(
                zone_id_, adjacent_zone_id_, destination_zone_id, caller_zone_id, object_id, options, reference_count);
        }
#endif
        CO_RETURN error_code;
    }

    CORO_TASK(void)
    transport::inbound_object_released(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        const std::vector<back_channel_entry>& in_back_channel)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_object_released(
                zone_id_, adjacent_zone_id_, destination_zone_id, caller_zone_id, object_id);
        }
#endif

        if (caller_zone_id == get_zone_id().as_caller())
        {
            CO_AWAIT get_service()
                -> object_released(protocol_version, destination_zone_id, object_id, caller_zone_id, in_back_channel);
            CO_RETURN;
        }

        // Try zone pair lookup
        auto dest = get_destination_handler(caller_zone_id.as_destination(), destination_zone_id.as_caller());
        if (!dest)
        {
            CO_RETURN;
        }

        CO_AWAIT dest->object_released(protocol_version, destination_zone_id, object_id, caller_zone_id, in_back_channel);
    }

    CORO_TASK(void)
    transport::inbound_transport_down(uint64_t protocol_version,
        destination_zone destination_zone_id,
        caller_zone caller_zone_id,
        const std::vector<back_channel_entry>& in_back_channel)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_transport_down(
                zone_id_, adjacent_zone_id_, destination_zone_id, caller_zone_id);
        }
#endif

        // Try zone pair lookup
        auto dest = get_destination_handler(destination_zone_id, caller_zone_id);
        if (!dest)
        {
            CO_RETURN;
        }

        CO_AWAIT dest->transport_down(protocol_version, destination_zone_id, caller_zone_id, in_back_channel);
    }

    CORO_TASK(int)
    transport::send(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_send(protocol_version,
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
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK())
            {
                telemetry_service->on_transport_outbound_send(
                    get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
            }
            else
            {
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(void)
    transport::post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<back_channel_entry>& in_back_channel)
    {
        CO_AWAIT outbound_post(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            in_back_channel);
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_post(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif
    }

    CORO_TASK(int)
    transport::try_cast(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_try_cast(
            protocol_version, destination_zone_id, object_id, interface_id, in_back_channel, out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK())
            {
                telemetry_service->on_transport_outbound_try_cast(
                    get_zone_id(), get_adjacent_zone_id(), destination_zone_id, get_zone_id().as_caller(), object_id, interface_id);
            }
            else
            {
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(int)
    transport::add_ref(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        known_direction_zone known_direction_zone_id,
        add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_add_ref(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            known_direction_zone_id,
            build_out_param_channel,
            reference_count,
            in_back_channel,
            out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK())
            {
                telemetry_service->on_transport_outbound_add_ref(get_zone_id(),
                    get_adjacent_zone_id(),
                    destination_zone_id,
                    caller_zone_id,
                    object_id,
                    known_direction_zone_id,
                    build_out_param_channel,
                    reference_count);
            }
            else
            {
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(int)
    transport::release(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        release_options options,
        uint64_t& reference_count,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_release(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            options,
            reference_count,
            in_back_channel,
            out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK())
            {
                telemetry_service->on_transport_outbound_release(
                    get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, options, reference_count);
            }
            else
            {
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(void)
    transport::object_released(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        const std::vector<back_channel_entry>& in_back_channel)
    {
        CO_AWAIT outbound_object_released(
            protocol_version, destination_zone_id, object_id, caller_zone_id, in_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_object_released(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id);
        }
#endif
    }

    CORO_TASK(void)
    transport::transport_down(uint64_t protocol_version,
        destination_zone destination_zone_id,
        caller_zone caller_zone_id,
        const std::vector<back_channel_entry>& in_back_channel)
    {
        CO_AWAIT outbound_transport_down(protocol_version, destination_zone_id, caller_zone_id, in_back_channel);
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_transport_down(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id);
        }
#endif
    }

} // namespace rpc
