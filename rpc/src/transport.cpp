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
    transport::transport(std::string name, std::shared_ptr<service> service, zone adjacent_zone_id)
        : name_(name)
        , zone_id_(service->get_zone_id())
        , adjacent_zone_id_(adjacent_zone_id)
        , service_(service)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (adjacent_zone_id_.get_subnet() != 0)
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_creation(
                    name_, zone_id_, adjacent_zone_id_, status_.load(std::memory_order_acquire));
        }
#endif
    }

    transport::transport(std::string name, zone zone_id_, zone adjacent_zone_id)
        : name_(name)
        , zone_id_(zone_id_)
        , adjacent_zone_id_(adjacent_zone_id)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (adjacent_zone_id_.get_subnet() != 0)
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_creation(
                    name_, zone_id_, adjacent_zone_id_, status_.load(std::memory_order_acquire));
        }
#endif
    }

    transport::~transport()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (adjacent_zone_id_.get_subnet() != 0)
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_deletion(zone_id_, adjacent_zone_id_);
        }
#endif
    }

    void transport::set_service(std::shared_ptr<service> service)
    {
        RPC_ASSERT(service);
        service_ = service;
    }

    void transport::set_adjacent_zone_id(zone new_adjacent_zone_id)
    {
        auto old_adjacent_zone_id = adjacent_zone_id_;
        if (old_adjacent_zone_id == new_adjacent_zone_id)
            return;

        adjacent_zone_id_ = new_adjacent_zone_id;

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (old_adjacent_zone_id.get_subnet() != 0)
            {
                telemetry_service->on_transport_deletion(zone_id_, old_adjacent_zone_id);
            }
            if (adjacent_zone_id_.get_subnet() != 0)
            {
                telemetry_service->on_transport_creation(
                    name_, zone_id_, adjacent_zone_id_, status_.load(std::memory_order_acquire));
            }
        }
#endif
    }

    CORO_TASK(int) transport::connect(connection_settings input_descr, interface_descriptor& output_descr)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (input_descr.get_object_id().is_set())
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_outbound_add_ref(zone_id_,
                    adjacent_zone_id_,
                    adjacent_zone_id_.as_destination().with_object(input_descr.get_object_id()),
                    zone_id_.as_caller(),
                    zone_id_,
                    rpc::add_ref_options::normal);
        }
#endif
        int ret = CO_AWAIT inner_connect(input_descr, output_descr);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (output_descr.get_object_id().is_set())
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_inbound_add_ref(zone_id_,
                    adjacent_zone_id_,
                    zone_id_.as_destination().with_object(output_descr.get_object_id()),
                    adjacent_zone_id_.as_caller(),
                    adjacent_zone_id_,
                    rpc::add_ref_options::normal);
        }
#endif

        CO_RETURN ret;
    }

    CORO_TASK(int) transport::accept()
    {
        int ret = CO_AWAIT inner_accept();

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_accept(zone_id_, adjacent_zone_id_, ret);
#endif

        CO_RETURN ret;
    }

    bool transport::inner_add_passthrough(
        std::weak_ptr<pass_through> pt, destination_zone outbound_dest, destination_zone inbound_source)
    {
        // Caller must hold destinations_mutex_
        pass_through_key lookup_val;

        if (outbound_dest < inbound_source)
            lookup_val = {outbound_dest, inbound_source};
        else
            lookup_val = {inbound_source, outbound_dest};

        // Check if entry already exists
        auto outer_it = pass_thoughs_.find(lookup_val);
        if (outer_it != pass_thoughs_.end())
        {
            RPC_ASSERT(false);
            return false; // Already exists
        }

        // Add entry to passthrough map
        pass_thoughs_[lookup_val] = pt;

        // Increment destination_count twice (once for each direction)
        destination_count_ += 2;

        // Increment outbound passthrough count
        zone outbound_zone = outbound_dest.as_zone();
        auto& outbound_counts = zone_counts_[outbound_zone];
        outbound_counts.outbound_passthrough_count++;

        // Increment inbound passthrough count
        zone inbound_zone = inbound_source.as_zone();
        auto& inbound_counts = zone_counts_[inbound_zone];
        inbound_counts.inbound_passthrough_count++;

        RPC_DEBUG("inner_add_passthrough: zone={}, adjacent={}, outbound_dest={}, inbound_source={}, "
                  "outbound_pt_count={}, inbound_pt_count={}, total_dest_count={}",
            zone_id_.get_subnet(),
            adjacent_zone_id_.get_subnet(),
            outbound_dest.get_subnet(),
            inbound_source.get_subnet(),
            outbound_counts.outbound_passthrough_count.load(),
            inbound_counts.inbound_passthrough_count.load(),
            get_destination_count());

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_add_destination(
                zone_id_, adjacent_zone_id_, lookup_val.zone1, lookup_val.zone2.as_caller());
#endif

        return true;
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
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count());

        zone dest_zone = dest.as_zone();
        auto& counts = zone_counts_[dest_zone];
        counts.proxy_count++;
    }
    void transport::inner_decrement_outbound_proxy_count(destination_zone dest)
    {
        zone dest_zone = dest.as_zone();
        auto found = zone_counts_.find(dest_zone);
        if (found == zone_counts_.end())
        {
            RPC_WARNING("inner_decrement_outbound_proxy_count: No zone count found for dest={} in zone={}",
                dest.get_subnet(),
                zone_id_.get_subnet());
            return;
        }

        auto& counts = found->second;
        if (counts.proxy_count == 0)
        {
            RPC_WARNING("inner_decrement_outbound_proxy_count: Proxy count already zero for dest={} in zone={}",
                dest.get_subnet(),
                zone_id_.get_subnet());
            return;
        }

        --destination_count_;

        RPC_DEBUG("decrement_outbound_proxy_count {} -> {} count = {}",
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count());

        auto proxy_count = --counts.proxy_count;
        auto stub_count = counts.stub_count.load();
        auto outbound_pt_count = counts.outbound_passthrough_count.load();
        auto inbound_pt_count = counts.inbound_passthrough_count.load();

        // If all counts are zero, remove the zone entry and trigger cleanup
        if (proxy_count == 0 && stub_count == 0 && outbound_pt_count == 0 && inbound_pt_count == 0)
        {
            zone_counts_.erase(found);

            // Only remove transport from service if WE are the registered transport for this zone
            // Multiple transports can have the same zone in zone_counts_ (for passthrough routing),
            // but only the transport actually registered in service::transports_[zone] should remove it
            if (auto svc = service_.lock())
            {
                auto registered_transport = svc->get_transport(dest_zone.as_destination());
                if (registered_transport && registered_transport.get() == this)
                {
                    svc->remove_transport(dest_zone.as_destination());
                    RPC_DEBUG(
                        "inner_decrement_outbound_proxy_count: All counts reached 0 for zone={}, removed transport",
                        dest.get_subnet());
                }
                else
                {
                    RPC_DEBUG("inner_decrement_outbound_proxy_count: All counts reached 0 for zone={}, but not "
                              "registered transport, "
                              "not removing from service (zone={}, adjacent={})",
                        dest.get_subnet(),
                        zone_id_.get_subnet(),
                        adjacent_zone_id_.get_subnet());
                }
            }
        }

        // Initiate graceful shutdown when no destinations remain
        if (destination_count_ == 0 && get_status() == transport_status::CONNECTED)
        {
            RPC_DEBUG(
                "transport::inner_decrement_outbound_proxy_count: destination_count reached 0, triggering shutdown "
                "for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    void transport::inner_increment_inbound_stub_count(caller_zone dest)
    {
        ++destination_count_;
        RPC_DEBUG("increment_inbound_stub_count {} -> {} count = {}",
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count());

        zone dest_zone = dest.as_destination().as_zone();
        auto& counts = zone_counts_[dest_zone];
        counts.stub_count++;
    }
    void transport::inner_decrement_inbound_stub_count(caller_zone dest)
    {
        zone dest_zone = dest.as_destination().as_zone();
        auto found = zone_counts_.find(dest_zone);
        if (found == zone_counts_.end())
        {
            RPC_WARNING("inner_decrement_inbound_stub_count: No zone count found for dest={}", dest.get_subnet());
            return;
        }

        auto& counts = found->second;
        if (counts.stub_count == 0)
        {
            RPC_WARNING("inner_decrement_inbound_stub_count: Stub count already zero for dest={} in zone={}",
                dest.get_subnet(),
                zone_id_.get_subnet());
            return;
        }

        --destination_count_;

        RPC_DEBUG("decrement_inbound_stub_count {} -> {} count = {}",
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count());

        auto stub_count = --counts.stub_count;
        auto proxy_count = counts.proxy_count.load();
        auto outbound_pt_count = counts.outbound_passthrough_count.load();
        auto inbound_pt_count = counts.inbound_passthrough_count.load();

        // If all counts are zero, remove the zone entry and trigger cleanup
        if (stub_count == 0 && proxy_count == 0 && outbound_pt_count == 0 && inbound_pt_count == 0)
        {
            zone_counts_.erase(found);

            // Only remove transport from service if WE are the registered transport for this zone
            // Multiple transports can have the same zone in zone_counts_ (for passthrough routing),
            // but only the transport actually registered in service::transports_[zone] should remove it
            if (auto svc = service_.lock())
            {
                auto registered_transport = svc->get_transport(dest_zone.as_destination());
                if (registered_transport && registered_transport.get() == this)
                {
                    svc->remove_transport(dest_zone.as_destination());
                    RPC_DEBUG("inner_decrement_inbound_stub_count: All counts reached 0 for zone={}, removed transport",
                        dest.get_subnet());
                }
                else
                {
                    RPC_DEBUG("inner_decrement_inbound_stub_count: All counts reached 0 for zone={}, but not "
                              "registered transport, "
                              "not removing from service (zone={}, adjacent={})",
                        dest.get_subnet(),
                        zone_id_.get_subnet(),
                        adjacent_zone_id_.get_subnet());
                }
            }
        }

        // Initiate graceful shutdown when no destinations remain
        if (destination_count_ == 0 && get_status() == transport_status::CONNECTED)
        {
            RPC_DEBUG("transport::inner_decrement_inbound_stub_count: destination_count reached 0, triggering shutdown "
                      "for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    void transport::remove_passthrough(destination_zone outbound_dest, destination_zone inbound_source)
    {
        std::unique_lock lock(destinations_mutex_);
        pass_through_key lookup_val;
        if (outbound_dest < inbound_source)
            lookup_val = {outbound_dest, inbound_source};
        else
            lookup_val = {inbound_source, outbound_dest};

        auto outer_it = pass_thoughs_.find(lookup_val);
        if (outer_it != pass_thoughs_.end())
        {
            pass_thoughs_.erase(outer_it);
        }
        else
        {
            RPC_ASSERT(false);
            return;
        }

        zone outbound_zone = outbound_dest.as_zone();
        zone inbound_zone = inbound_source.as_zone();

        // Decrement outbound passthrough count
        auto outbound_found = zone_counts_.find(outbound_zone);
        if (outbound_found == zone_counts_.end())
        {
            RPC_WARNING("remove_passthrough: No zone count found for outbound_dest={} in zone={}",
                outbound_dest.get_subnet(),
                zone_id_.get_subnet());
        }
        else
        {
            auto& outbound_counts = outbound_found->second;
            if (outbound_counts.outbound_passthrough_count == 0)
            {
                RPC_WARNING("remove_passthrough: Outbound count already zero for dest={} in zone={}",
                    outbound_dest.get_subnet(),
                    zone_id_.get_subnet());
            }
            else
            {
                --destination_count_;
                --outbound_counts.outbound_passthrough_count;

                // Check if all counts are zero for outbound zone
                if (outbound_counts.proxy_count == 0 && outbound_counts.stub_count == 0
                    && outbound_counts.outbound_passthrough_count == 0 && outbound_counts.inbound_passthrough_count == 0)
                {
                    zone_counts_.erase(outbound_found);

                    // Only remove transport from service if WE are the registered transport for this zone
                    // Multiple transports can have the same zone in zone_counts_ (for passthrough routing),
                    // but only the transport actually registered in service::transports_[zone] should remove it
                    if (auto svc = service_.lock())
                    {
                        auto registered_transport = svc->get_transport(outbound_zone.as_destination());
                        if (registered_transport && registered_transport.get() == this)
                        {
                            RPC_DEBUG("remove_passthrough: Removing transport! zone={}, adjacent={}, target_zone={}, "
                                      "passthrough={}->{}, reason=outbound_counts_zero",
                                zone_id_.get_subnet(),
                                adjacent_zone_id_.get_subnet(),
                                outbound_dest.get_subnet(),
                                outbound_dest.get_subnet(),
                                inbound_source.get_subnet());
                            svc->remove_transport(outbound_zone.as_destination());
                        }
                        else
                        {
                            RPC_DEBUG("remove_passthrough: All counts reached 0 for outbound zone={}, but not "
                                      "registered transport, "
                                      "not removing from service (zone={}, adjacent={})",
                                outbound_dest.get_subnet(),
                                zone_id_.get_subnet(),
                                adjacent_zone_id_.get_subnet());
                        }
                    }
                }
            }
        }

        // Decrement inbound passthrough count
        auto inbound_found = zone_counts_.find(inbound_zone);
        if (inbound_found == zone_counts_.end())
        {
            RPC_WARNING("remove_passthrough: No zone count found for inbound_source={} in zone={}",
                inbound_source.get_subnet(),
                zone_id_.get_subnet());
        }
        else
        {
            auto& inbound_counts = inbound_found->second;
            if (inbound_counts.inbound_passthrough_count == 0)
            {
                RPC_WARNING("remove_passthrough: Inbound count already zero for source={} in zone={}",
                    inbound_source.get_subnet(),
                    zone_id_.get_subnet());
            }
            else
            {
                --destination_count_;
                --inbound_counts.inbound_passthrough_count;

                // Check if all counts are zero for inbound zone
                if (inbound_counts.proxy_count == 0 && inbound_counts.stub_count == 0
                    && inbound_counts.outbound_passthrough_count == 0 && inbound_counts.inbound_passthrough_count == 0)
                {
                    zone_counts_.erase(inbound_found);

                    // Only remove transport from service if WE are the registered transport for this zone
                    // Multiple transports can have the same zone in zone_counts_ (for passthrough routing),
                    // but only the transport actually registered in service::transports_[zone] should remove it
                    if (auto svc = service_.lock())
                    {
                        auto registered_transport = svc->get_transport(inbound_zone.as_destination());
                        if (registered_transport && registered_transport.get() == this)
                        {
                            RPC_WARNING("remove_passthrough: Removing transport! zone={}, adjacent={}, target_zone={}, "
                                        "passthrough={}->{}, reason=inbound_counts_zero",
                                zone_id_.get_subnet(),
                                adjacent_zone_id_.get_subnet(),
                                inbound_source.get_subnet(),
                                outbound_dest.get_subnet(),
                                inbound_source.get_subnet());
                            svc->remove_transport(inbound_zone.as_destination());
                            RPC_DEBUG("remove_passthrough: All counts reached 0 for inbound zone={}, removed transport",
                                inbound_source.get_subnet());
                        }
                        else
                        {
                            RPC_DEBUG("remove_passthrough: All counts reached 0 for inbound zone={}, but not "
                                      "registered transport, "
                                      "not removing from service (zone={}, adjacent={})",
                                inbound_source.get_subnet(),
                                zone_id_.get_subnet(),
                                adjacent_zone_id_.get_subnet());
                        }
                    }
                }
            }
        }

        RPC_DEBUG("remove_passthrough: zone={}, adjacent={}, outbound_dest={}, inbound_source={}, "
                  "remaining_dest_count={}",
            zone_id_.get_subnet(),
            adjacent_zone_id_.get_subnet(),
            outbound_dest.get_subnet(),
            inbound_source.get_subnet(),
            get_destination_count());

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_transport_remove_destination(
                zone_id_, adjacent_zone_id_, lookup_val.zone1, lookup_val.zone2.as_caller());
#endif

        // Initiate graceful shutdown when no destinations remain
        if (destination_count_ == 0 && get_status() == transport_status::CONNECTED)
        {
            RPC_DEBUG("transport::remove_passthrough: destination_count reached 0, triggering shutdown for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    std::shared_ptr<pass_through> transport::create_pass_through(std::shared_ptr<transport> forward,
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
                forward_dest.get_subnet());
            return nullptr;
        }

        // Validate: forward and reverse transports should be different objects
        if (forward.get() == reverse.get())
        {
            RPC_ERROR("create_pass_through: Invalid pass-through request - forward and reverse transports are the same "
                      "object! forward_dest={}, reverse_dest={}",
                forward_dest.get_subnet(),
                reverse_dest.get_subnet());
            return nullptr;
        }

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
        auto forward_passthrough = forward->inner_get_passthrough(reverse_dest, forward_dest);
        auto reverse_passthrough = reverse->inner_get_passthrough(forward_dest, reverse_dest);

        // check that they are the same
        RPC_ASSERT(!forward_passthrough == !reverse_passthrough);

        if (forward_passthrough)
        {
            RPC_DEBUG("create_pass_through: Found existing pass-through for forward_dest={}, reverse_dest={}",
                forward_dest.get_subnet(),
                reverse_dest.get_subnet());
            return forward_passthrough;
        }
        else
        {
            std::shared_ptr<pass_through> pt(
                new rpc::pass_through(forward, // forward_transport: handles messages TO final destination
                    reverse,                   // reverse_transport: handles messages back to caller
                    service,                   // service
                    forward_dest,
                    reverse_dest // reverse_destination: where reverse messages go
                    ));
            pt->self_ref_ = pt; // keep self alive based on reference counts

            RPC_DEBUG("create_pass_through: Creating NEW passthrough {}->{}, "
                      "forward_transport=(zone={}, adjacent={}), "
                      "reverse_transport=(zone={}, adjacent={}), pt={}",
                reverse_dest.get_subnet(),
                forward_dest.get_subnet(),
                forward->zone_id_.get_subnet(),
                forward->adjacent_zone_id_.get_subnet(),
                reverse->zone_id_.get_subnet(),
                reverse->adjacent_zone_id_.get_subnet(),
                (void*)pt.get());
            // Register pass-through on both transports and increment passthrough counts
            // Forward transport: routes TO forward_dest (outbound), FROM reverse_dest (inbound)
            // Reverse transport: routes TO reverse_dest (outbound), FROM forward_dest (inbound)
            forward->inner_add_passthrough(pt, forward_dest, reverse_dest);
            reverse->inner_add_passthrough(pt, reverse_dest, forward_dest);

            // Note: We do NOT register forward_dest in service->transports here because:
            // 1. The pass-through might fail to reach the destination (zone doesn't exist downstream)
            // 2. Registering it would create routing loops
            // 3. The registration should happen after successful routing, by the caller

            return pt;
        }
    }

    // Status management
    transport_status transport::get_status() const
    {
        return status_.load(std::memory_order_acquire);
    }

    void transport::set_status(transport_status new_status)
    {
        [[maybe_unused]] auto old_status = status_.load(std::memory_order_acquire);
        if (old_status == new_status)
        {
            return; // Already at target status, idempotent
        }
        if (old_status > new_status)
        {
            RPC_ASSERT(false); // Regressive transition is always a bug
            return;
        }
        status_.store(new_status, std::memory_order_release);

#ifdef CANOPY_USE_TELEMETRY
        if (adjacent_zone_id_.get_subnet() != 0 && old_status != new_status)
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_status_change(name_, zone_id_, adjacent_zone_id_, old_status, new_status);
        }
#endif
    }

    std::shared_ptr<pass_through> transport::inner_get_passthrough(destination_zone zone1, destination_zone zone2) const
    {
        pass_through_key lookup_val;
        if (zone1 < zone2)
            lookup_val = {zone1, zone2};
        else
            lookup_val = {zone2, zone1};

        auto it = pass_thoughs_.find(lookup_val);
        if (it != pass_thoughs_.end())
        {
            auto pt = it->second.lock();
            if (!pt)
            {
                RPC_WARNING("inner_get_passthrough: weak_ptr expired for zone1={}, zone2={} on transport "
                            "zone={} adjacent_zone={}",
                    zone1.get_subnet(),
                    zone2.get_subnet(),
                    zone_id_.get_subnet(),
                    adjacent_zone_id_.get_subnet());
            }
            return pt;
        }
        return nullptr;
    }

    std::shared_ptr<transport> transport::inner_get_transport_from_passthroughs(destination_zone destination_zone_id) const
    {
        std::shared_lock lock(destinations_mutex_);
        for (auto& [key, pt] : pass_thoughs_)
        {
            if (key.zone1 == destination_zone_id || key.zone2 == destination_zone_id)
            {
                return pt.lock()->get_directional_transport(destination_zone_id);
            }
        }
        return nullptr;
    }

    // Helper to route incoming messages to registered handlers
    std::shared_ptr<pass_through> transport::get_passthrough(destination_zone zone1, destination_zone zone2) const
    {
        RPC_ASSERT(zone1 != zone_id_.as_destination());
        RPC_ASSERT(zone2 != zone_id_.as_destination());

        std::shared_lock lock(destinations_mutex_);
        auto handler = inner_get_passthrough(zone1, zone2);
        RPC_DEBUG("get_passthrough: zone1={}, zone2={}, transport zone={}, adjacent_zone={}, found={}",
            zone1.get_subnet(),
            zone2.get_subnet(),
            zone_id_.get_subnet(),
            adjacent_zone_id_.get_subnet(),
            handler != nullptr);
        return handler;
    }

    CORO_TASK(void) transport::notify_all_destinations_of_disconnect()
    {
        std::shared_ptr<service> service;
        std::vector<std::shared_ptr<pass_through>> handlers_to_notify;
        std::vector<destination_zone> zones_to_notify;

        {
            std::shared_lock lock(destinations_mutex_);
            service = service_.lock();
            if (!service)
            {
                RPC_ERROR("notify_all_destinations_of_disconnect: Local service no longer exists on transport zone={} "
                          "adjacent_zone={}",
                    zone_id_.get_subnet(),
                    adjacent_zone_id_.get_subnet());
                CO_RETURN;
            }

            handlers_to_notify.reserve(pass_thoughs_.size());
            for (const auto& [dest_zone, pt] : pass_thoughs_)
            {
                std::ignore = dest_zone;
                if (auto handler = pt.lock())
                {
                    handlers_to_notify.push_back(std::move(handler));
                }
            }

            zones_to_notify.reserve(zone_counts_.size());
            for (const auto& zone_item : zone_counts_)
            {
                zones_to_notify.push_back(zone_item.first.as_destination());
            }
        }

        auto self = shared_from_this();

        // Iterate through passthrough handlers and notify them
        for (const auto& handler : handlers_to_notify)
        {
            // Send zone_terminating post
            CO_AWAIT handler->local_transport_down(self);
        }

        // Notify service about transport down for each connected remote zone
        // Using zone_counts_ provides direct knowledge of which zones were connected,
        // enabling efficient forward cleanup
        for (const auto& remote_zone : zones_to_notify)
        {
            RPC_DEBUG("notify_all_destinations_of_disconnect: Notifying service about zone={} going down",
                remote_zone.get_subnet());
            CO_AWAIT service->notify_transport_down(self, remote_zone);
        }
    }

    // inbound i_marshaller interface implementation
    // Routes via pass_thoughs_ map (service is registered as a destination)
    CORO_TASK(int)
    transport::inbound_send(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
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
                zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, interface_id, method_id);
        }
#endif

        // Reject new RPC calls when shutting down; cleanup methods are exempt
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        std::shared_ptr<i_marshaller> dest;
        if (remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(remote_object_id, caller_zone_id.as_destination());
            if (!dest)
            {
                CO_RETURN error::ZONE_NOT_FOUND();
            }
        }

        CO_RETURN CO_AWAIT dest->send(protocol_version,
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
    }

    CORO_TASK(void)
    transport::inbound_post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<back_channel_entry>& in_back_channel)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_post(
                zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, interface_id, method_id);
        }
#endif

        // Reject new RPC calls when shutting down; cleanup methods are exempt
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN;
        }

        std::shared_ptr<i_marshaller> dest;
        if (remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(remote_object_id, caller_zone_id.as_destination());
            if (!dest)
            {
                CO_RETURN;
            }
        }

        CO_AWAIT dest->post(
            protocol_version, encoding, tag, caller_zone_id, remote_object_id, interface_id, method_id, in_data, in_back_channel);
    }

    CORO_TASK(int)
    transport::inbound_try_cast(uint64_t protocol_version,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
        interface_ordinal interface_id,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_try_cast(
                zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, interface_id);
        }
#endif

        // Reject new RPC calls when shutting down; cleanup methods are exempt
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN error::TRANSPORT_ERROR();
        }

        std::shared_ptr<i_marshaller> dest;
        if (remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(remote_object_id, caller_zone_id.as_destination());
            if (!dest)
            {
                CO_RETURN error::ZONE_NOT_FOUND();
            }
        }

        CO_RETURN CO_AWAIT dest->try_cast(
            protocol_version, caller_zone_id, remote_object_id, interface_id, in_back_channel, out_back_channel);
    }

    CORO_TASK(int)
    transport::inbound_add_ref(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        requesting_zone requesting_zone_id,
        add_ref_options build_out_param_channel,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        // Reject new add_ref calls when shutting down
        if (get_status() >= transport_status::DISCONNECTING)
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
                  "build_dest_channel={}, requesting_zone_id={}",
            svc->get_zone_id().get_subnet(),
            remote_object_id.get_subnet(),
            caller_zone_id.get_subnet(),
            build_caller_channel,
            build_dest_channel,
            requesting_zone_id.get_subnet());

        if (!remote_object_id.get_address().same_zone(svc->get_zone_id().get_address())
            && caller_zone_id != svc->get_zone_id().as_caller())
        {
            auto dest_transport = svc->get_transport(remote_object_id);
            if (remote_object_id.get_address().same_zone(caller_zone_id.as_destination().get_address()))
            {
                // caller and destination are the same zone, so we just call the transport to pass the call along and not involve a pass through
                if (!dest_transport)
                {
                    CO_RETURN error::ZONE_NOT_FOUND();
                }

                // here we
                auto error_code = CO_AWAIT dest_transport->add_ref(protocol_version,
                    remote_object_id,
                    caller_zone_id,
                    requesting_zone_id,
                    build_out_param_channel,
                    in_back_channel,
                    out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_transport_inbound_add_ref(
                        zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, requesting_zone_id, build_out_param_channel);
                }
#endif
                CO_RETURN error_code;
            }
            if (!dest_transport)
            {
                if (build_dest_channel)
                {
                    dest_transport = inner_get_transport_from_passthroughs(remote_object_id);
                    if (!dest_transport)
                    {
                        dest_transport = svc->get_transport(requesting_zone_id.as_destination());
                    }
                    if (!dest_transport && remote_object_id != requesting_zone_id.as_destination())
                    {
                        dest_transport = inner_get_transport_from_passthroughs(requesting_zone_id.as_destination());
                    }
                    if (!dest_transport)
                    {
                        CO_RETURN error::ZONE_NOT_FOUND();
                    }
                }
                else
                {
                    dest_transport = shared_from_this();
                }
                svc->add_transport(remote_object_id, dest_transport);
            }

            // otherwise we are going to use or create a pass-through
            {
                auto passthrough = dest_transport->get_passthrough(caller_zone_id.as_destination(), remote_object_id);
                if (passthrough)
                {
                    CO_RETURN CO_AWAIT passthrough->add_ref(protocol_version,
                        remote_object_id,
                        caller_zone_id,
                        requesting_zone_id,
                        build_out_param_channel,
                        in_back_channel,
                        out_back_channel);
                }
            }

            auto caller_transport = svc->get_transport(caller_zone_id.as_destination());
            if (!caller_transport)
            {
                if (!build_dest_channel && build_caller_channel)
                {
                    caller_transport = inner_get_transport_from_passthroughs(caller_zone_id.as_destination());
                    if (!caller_transport)
                    {
                        caller_transport = svc->get_transport(requesting_zone_id.as_destination());
                    }
                    if (!dest_transport && caller_zone_id.as_destination() != requesting_zone_id.as_destination())
                    {
                        caller_transport = inner_get_transport_from_passthroughs(requesting_zone_id.as_destination());
                    }
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
                    remote_object_id,
                    caller_zone_id,
                    requesting_zone_id,
                    build_out_param_channel,
                    in_back_channel,
                    out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_transport_inbound_add_ref(
                        zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, requesting_zone_id, build_out_param_channel);
                }
#endif
                CO_RETURN error_code;
            }

            auto passthrough = transport::create_pass_through(
                dest_transport, caller_transport, svc, remote_object_id, caller_zone_id.as_destination());

            auto error_code = CO_AWAIT passthrough->add_ref(protocol_version,
                remote_object_id,
                caller_zone_id,
                requesting_zone_id,
                build_out_param_channel,
                in_back_channel,
                out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_inbound_add_ref(
                    zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, requesting_zone_id, build_out_param_channel);
            }
#endif
            CO_RETURN error_code;
        }

        // else it is a special case that the service needs to deal with

        auto error_code = CO_AWAIT svc->add_ref(protocol_version,
            remote_object_id,
            caller_zone_id,
            requesting_zone_id,
            build_out_param_channel,
            in_back_channel,
            out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_add_ref(
                zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, requesting_zone_id, build_out_param_channel);
        }
#endif
        CO_RETURN error_code;
    }

    CORO_TASK(int)
    transport::inbound_release(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        release_options options,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        std::shared_ptr<i_marshaller> dest;
        if (remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(remote_object_id, caller_zone_id.as_destination());
            if (!dest)
            {
                CO_RETURN error::ZONE_NOT_FOUND();
            }
        }

        auto error_code = CO_AWAIT dest->release(
            protocol_version, remote_object_id, caller_zone_id, options, in_back_channel, out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_release(
                zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id, options);
        }
#endif
        CO_RETURN error_code;
    }

    CORO_TASK(void)
    transport::inbound_object_released(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        const std::vector<back_channel_entry>& in_back_channel)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_object_released(
                zone_id_, adjacent_zone_id_, remote_object_id, caller_zone_id);
        }
#endif

        if (caller_zone_id == get_zone_id().as_caller())
        {
            CO_AWAIT get_service() -> object_released(protocol_version, remote_object_id, caller_zone_id, in_back_channel);
            CO_RETURN;
        }

        // Try zone pair lookup
        std::shared_ptr<i_marshaller> dest;
        if (remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(remote_object_id, caller_zone_id.as_destination());
            if (!dest)
            {
                // Zone not found - just return without propagating error
                CO_RETURN;
            }
        }

        CO_AWAIT dest->object_released(protocol_version, remote_object_id, caller_zone_id, in_back_channel);
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

        std::shared_ptr<i_marshaller> dest;
        if (destination_zone_id == zone_id_.as_destination())
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(destination_zone_id, caller_zone_id.as_destination());
            if (!dest)
            {
                CO_RETURN;
            }
        }

        CO_AWAIT dest->transport_down(protocol_version, destination_zone_id, caller_zone_id, in_back_channel);

        std::shared_lock lock(destinations_mutex_);
        zone_counts_.erase(caller_zone_id.get_subnet());
    }

    CORO_TASK(int)
    transport::send(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
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
            remote_object_id,
            interface_id,
            method_id,
            in_data,
            out_buf_,
            in_back_channel,
            out_back_channel);
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK() || ret == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_send(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, interface_id, method_id);
            }
            else
            {
                // RPC_ASSERT(false);
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
        remote_object remote_object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<back_channel_entry>& in_back_channel)
    {
        CO_AWAIT outbound_post(
            protocol_version, encoding, tag, caller_zone_id, remote_object_id, interface_id, method_id, in_data, in_back_channel);
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_post(
                get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, interface_id, method_id);
        }
#endif
    }

    CORO_TASK(int)
    transport::try_cast(uint64_t protocol_version,
        caller_zone caller_zone_id,
        remote_object remote_object_id,
        interface_ordinal interface_id,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_try_cast(
            protocol_version, caller_zone_id, remote_object_id, interface_id, in_back_channel, out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK() || ret == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_try_cast(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, get_zone_id().as_caller(), interface_id);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(int)
    transport::add_ref(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        requesting_zone requesting_zone_id,
        add_ref_options build_out_param_channel,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_add_ref(protocol_version,
            remote_object_id,
            caller_zone_id,
            requesting_zone_id,
            build_out_param_channel,
            in_back_channel,
            out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK() || ret == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_add_ref(get_zone_id(),
                    get_adjacent_zone_id(),
                    remote_object_id,
                    caller_zone_id,
                    requesting_zone_id,
                    build_out_param_channel);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(int)
    transport::release(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        release_options options,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        auto ret = CO_AWAIT outbound_release(
            protocol_version, remote_object_id, caller_zone_id, options, in_back_channel, out_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (ret == error::OK() || ret == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_release(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, options);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call transport_down");
            }
        }
#endif
        CO_RETURN ret;
    }

    CORO_TASK(void)
    transport::object_released(uint64_t protocol_version,
        remote_object remote_object_id,
        caller_zone caller_zone_id,
        const std::vector<back_channel_entry>& in_back_channel)
    {
        CO_AWAIT outbound_object_released(protocol_version, remote_object_id, caller_zone_id, in_back_channel);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_object_released(
                get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id);
        }
#endif
        decrement_inbound_stub_count(caller_zone_id);
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

    CORO_TASK(int)
    transport::get_new_zone_id(uint64_t protocol_version,
        zone& zone_id,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        CO_RETURN CO_AWAIT outbound_get_new_zone_id(protocol_version, zone_id, in_back_channel, out_back_channel);
    }

    CORO_TASK(int)
    transport::outbound_get_new_zone_id(uint64_t protocol_version,
        zone& zone_id,
        const std::vector<back_channel_entry>& in_back_channel,
        std::vector<back_channel_entry>& out_back_channel)
    {
        // Default: delegate to the local service.
        // For a root service this allocates locally; for a child_service it
        // forwards the request up to the parent zone.
        auto svc = service_.lock();
        if (!svc)
            CO_RETURN rpc::error::ZONE_NOT_FOUND();
        CO_RETURN CO_AWAIT svc->get_new_zone_id(protocol_version, zone_id, in_back_channel, out_back_channel);
    }

} // namespace rpc
