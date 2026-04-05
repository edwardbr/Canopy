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
    namespace
    {
        pass_through_key make_pass_through_key(
            destination_zone zone1,
            destination_zone zone2)
        {
            return zone1 < zone2 ? pass_through_key{FLD(zone1) zone1, FLD(zone2) zone2}
                                 : pass_through_key{FLD(zone1) zone2, FLD(zone2) zone1};
        }
    }

    transport::transport(
        std::string name,
        std::shared_ptr<service> service)
        : name_(name)
        , zone_id_(service->get_zone_id())
        , service_(service)
    {
    }

    transport::transport(
        std::string name,
        zone zone_id_)
        : name_(name)
        , zone_id_(zone_id_)
    {
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
#ifdef CANOPY_USE_TELEMETRY
        if (adjacent_zone_id_.get_subnet() != 0)
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_creation(
                    name_, zone_id_, adjacent_zone_id_, status_.load(std::memory_order_acquire));
        }
#endif

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

    CORO_TASK(connect_result)
    transport::connect(
        std::shared_ptr<rpc::object_stub> stub,
        connection_settings input_descr)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (input_descr.get_object_id().is_set())
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_outbound_add_ref(
                    zone_id_,
                    adjacent_zone_id_,
                    adjacent_zone_id_.with_object(input_descr.get_object_id()),
                    zone_id_,
                    zone_id_,
                    rpc::add_ref_options::normal);
        }
#endif
        auto result = CO_AWAIT inner_connect(stub, input_descr);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (result.output_descriptor.get_object_id().is_set())
        {
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_inbound_add_ref(
                    zone_id_,
                    adjacent_zone_id_,
                    zone_id_.with_object(result.output_descriptor.get_object_id()),
                    adjacent_zone_id_,
                    adjacent_zone_id_,
                    rpc::add_ref_options::normal);
        }
#endif

        CO_RETURN result;
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
        std::weak_ptr<pass_through> pt,
        destination_zone outbound_dest,
        destination_zone inbound_source)
    {
        // Caller must hold destinations_mutex_
        auto lookup_val = make_pass_through_key(outbound_dest, inbound_source);

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
        zone outbound_zone = outbound_dest;
        auto& outbound_counts = zone_counts_[outbound_zone];
        outbound_counts.outbound_passthrough_count++;

        // Increment inbound passthrough count
        zone inbound_zone = inbound_source;
        auto& inbound_counts = zone_counts_[inbound_zone];
        inbound_counts.inbound_passthrough_count++;

        RPC_DEBUG(
            "inner_add_passthrough: zone={}, adjacent={}, outbound_dest={}, inbound_source={}, "
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
                zone_id_, adjacent_zone_id_, lookup_val.zone1, lookup_val.zone2);
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
        bool should_shutdown = false;
        {
            std::unique_lock lock(destinations_mutex_);
            should_shutdown = inner_decrement_outbound_proxy_count(dest);
        }

        // Initiate graceful shutdown when no destinations remain.
        // Note: on_destination_count_zero() is called while destinations_mutex_ is not held.
        if (should_shutdown && destination_count_ == 0 && get_status() == transport_status::CONNECTED)
        {
            RPC_DEBUG(
                "transport::inner_decrement_outbound_proxy_count: destination_count reached 0, triggering shutdown "
                "for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    void transport::increment_inbound_stub_count(caller_zone dest)
    {
        std::unique_lock lock(destinations_mutex_);
        return inner_increment_inbound_stub_count(dest);
    }
    void transport::decrement_inbound_stub_count(caller_zone dest)
    {
        bool should_shutdown = false;
        {
            std::unique_lock lock(destinations_mutex_);
            should_shutdown = inner_decrement_inbound_stub_count(dest);
        }
        if (should_shutdown)
        {
            RPC_DEBUG(
                "transport::decrement_inbound_stub_count: destination_count reached 0, triggering shutdown "
                "for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    void transport::inner_increment_outbound_proxy_count(destination_zone dest)
    {
        ++destination_count_;
        RPC_DEBUG(
            "increment_outbound_proxy_count {} -> {} count = {}",
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count());

        zone dest_zone = dest;
        auto& counts = zone_counts_[dest_zone];
        counts.proxy_count++;
    }
    bool transport::inner_decrement_outbound_proxy_count(destination_zone dest)
    {
        zone dest_zone = dest;
        auto found = zone_counts_.find(dest_zone);
        if (found == zone_counts_.end())
        {
            RPC_WARNING(
                "inner_decrement_outbound_proxy_count: No zone count found for dest={} in zone={}",
                dest.get_subnet(),
                zone_id_.get_subnet());
            return false;
        }

        auto& counts = found->second;
        if (counts.proxy_count == 0)
        {
            RPC_WARNING(
                "inner_decrement_outbound_proxy_count: Proxy count already zero for dest={} in zone={}",
                dest.get_subnet(),
                zone_id_.get_subnet());
            return false;
        }

        --destination_count_;

        RPC_DEBUG(
            "decrement_outbound_proxy_count {} -> {} count = {}",
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
                auto registered_transport = svc->get_transport(dest_zone);
                if (registered_transport && registered_transport.get() == this)
                {
                    svc->remove_transport(dest_zone);
                    RPC_DEBUG(
                        "inner_decrement_outbound_proxy_count: All counts reached 0 for zone={}, removed transport",
                        dest.get_subnet());
                }
                else
                {
                    RPC_DEBUG(
                        "inner_decrement_outbound_proxy_count: All counts reached 0 for zone={}, but not "
                        "registered transport, "
                        "not removing from service (zone={}, adjacent={})",
                        dest.get_subnet(),
                        zone_id_.get_subnet(),
                        adjacent_zone_id_.get_subnet());
                }
            }
        }

        return true;
    }

    void transport::inner_increment_inbound_stub_count(caller_zone dest)
    {
        ++destination_count_;
        RPC_DEBUG(
            "increment_inbound_stub_count {} -> {} count = {}",
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count());

        zone dest_zone = dest;
        auto& counts = zone_counts_[dest_zone];
        counts.stub_count++;
    }
    bool transport::inner_decrement_inbound_stub_count(caller_zone dest)
    {
        zone dest_zone = dest;
        auto found = zone_counts_.find(dest_zone);
        if (found == zone_counts_.end())
        {
            RPC_WARNING("inner_decrement_inbound_stub_count: No zone count found for dest={}", dest.get_subnet());
            return false;
        }

        auto& counts = found->second;
        if (counts.stub_count == 0)
        {
            RPC_WARNING(
                "inner_decrement_inbound_stub_count: Stub count already zero for dest={} in zone={}",
                dest.get_subnet(),
                zone_id_.get_subnet());
            return false;
        }

        --destination_count_;

        RPC_DEBUG(
            "decrement_inbound_stub_count {} -> {} count = {}",
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
                auto registered_transport = svc->get_transport(dest_zone);
                if (registered_transport && registered_transport.get() == this)
                {
                    svc->remove_transport(dest_zone);
                    RPC_DEBUG(
                        "inner_decrement_inbound_stub_count: All counts reached 0 for zone={}, removed transport",
                        dest.get_subnet());
                }
                else
                {
                    RPC_DEBUG(
                        "inner_decrement_inbound_stub_count: All counts reached 0 for zone={}, but not "
                        "registered transport, "
                        "not removing from service (zone={}, adjacent={})",
                        dest.get_subnet(),
                        zone_id_.get_subnet(),
                        adjacent_zone_id_.get_subnet());
                }
            }
        }

        return destination_count_ == 0 && get_status() == transport_status::CONNECTED;
    }

    // Returns true if on_destination_count_zero() should be called by the caller
    // after releasing destinations_mutex_, so the virtual call cannot re-enter the lock.
    bool transport::inner_decrement_inbound_stub_count_by(
        caller_zone dest,
        uint64_t count)
    {
        zone dest_zone = dest;
        auto found = zone_counts_.find(dest_zone);
        if (found == zone_counts_.end())
        {
            RPC_WARNING("inner_decrement_inbound_stub_count_by: No zone count found for dest={}", dest.get_subnet());
            return false;
        }

        auto& counts = found->second;
        if (counts.stub_count < count)
        {
            RPC_WARNING(
                "inner_decrement_inbound_stub_count_by: Stub count {} less than requested decrement {} "
                "for dest={} in zone={}",
                counts.stub_count.load(),
                count,
                dest.get_subnet(),
                zone_id_.get_subnet());
            count = counts.stub_count.load();
        }

        if (count == 0)
            return false;

        destination_count_ -= static_cast<int64_t>(count);

        RPC_DEBUG(
            "decrement_inbound_stub_count_by {} -> {} count={} by={}",
            zone_id_.get_subnet(),
            dest.get_subnet(),
            get_destination_count(),
            count);

        counts.stub_count -= count;

        if (counts.stub_count == 0 && counts.proxy_count.load() == 0 && counts.outbound_passthrough_count.load() == 0
            && counts.inbound_passthrough_count.load() == 0)
        {
            zone_counts_.erase(found);

            if (auto svc = service_.lock())
            {
                auto registered_transport = svc->get_transport(dest_zone);
                if (registered_transport && registered_transport.get() == this)
                {
                    svc->remove_transport(dest_zone);
                    RPC_DEBUG(
                        "inner_decrement_inbound_stub_count_by: All counts reached 0 for zone={}, removed transport",
                        dest.get_subnet());
                }
                else
                {
                    RPC_DEBUG(
                        "inner_decrement_inbound_stub_count_by: All counts reached 0 for zone={}, "
                        "but not registered transport, not removing from service (zone={}, adjacent={})",
                        dest.get_subnet(),
                        zone_id_.get_subnet(),
                        adjacent_zone_id_.get_subnet());
                }
            }
        }

        return destination_count_ == 0 && get_status() == transport_status::CONNECTED;
    }

    void transport::decrement_inbound_stub_count_by(
        caller_zone dest,
        uint64_t count)
    {
        if (count == 0)
            return;
        bool trigger_shutdown = false;
        {
            std::unique_lock lock(destinations_mutex_);
            trigger_shutdown = inner_decrement_inbound_stub_count_by(dest, count);
        }
        if (trigger_shutdown)
        {
            RPC_DEBUG(
                "transport::decrement_inbound_stub_count_by: destination_count reached 0, "
                "triggering shutdown for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    void transport::remove_passthrough(
        destination_zone outbound_dest,
        destination_zone inbound_source)
    {
        bool should_shutdown = false;
        {
            std::unique_lock lock(destinations_mutex_);
            auto lookup_val = make_pass_through_key(outbound_dest, inbound_source);

            auto outer_it = pass_thoughs_.find(lookup_val);
            if (outer_it != pass_thoughs_.end())
            {
                pass_thoughs_.erase(outer_it);
            }
            else
            {
                RPC_ERROR(
                    "remove_passthrough: Passthrough not found for outbound_dest={}, inbound_source={} in zone={}",
                    outbound_dest.get_subnet(),
                    inbound_source.get_subnet(),
                    zone_id_.get_subnet());
                RPC_ASSERT(false);
                return;
            }

            zone outbound_zone = outbound_dest;
            zone inbound_zone = inbound_source;

            // Decrement outbound passthrough count
            auto outbound_found = zone_counts_.find(outbound_zone);
            if (outbound_found == zone_counts_.end())
            {
                RPC_WARNING(
                    "remove_passthrough: No zone count found for outbound_dest={} in zone={}",
                    outbound_dest.get_subnet(),
                    zone_id_.get_subnet());
            }
            else
            {
                auto& outbound_counts = outbound_found->second;
                if (outbound_counts.outbound_passthrough_count == 0)
                {
                    RPC_WARNING(
                        "remove_passthrough: Outbound count already zero for dest={} in zone={}",
                        outbound_dest.get_subnet(),
                        zone_id_.get_subnet());
                }
                else
                {
                    --destination_count_;
                    --outbound_counts.outbound_passthrough_count;

                    // Check if all counts are zero for outbound zone
                    if (outbound_counts.proxy_count == 0 && outbound_counts.stub_count == 0
                        && outbound_counts.outbound_passthrough_count == 0
                        && outbound_counts.inbound_passthrough_count == 0)
                    {
                        zone_counts_.erase(outbound_found);

                        // Only remove transport from service if WE are the registered transport for this zone
                        // Multiple transports can have the same zone in zone_counts_ (for passthrough routing),
                        // but only the transport actually registered in service::transports_[zone] should remove it
                        if (auto svc = service_.lock())
                        {
                            auto registered_transport = svc->get_transport(outbound_zone);
                            if (registered_transport && registered_transport.get() == this)
                            {
                                RPC_DEBUG(
                                    "remove_passthrough: Removing transport! zone={}, adjacent={}, target_zone={}, "
                                    "passthrough={}->{}, reason=outbound_counts_zero",
                                    zone_id_.get_subnet(),
                                    adjacent_zone_id_.get_subnet(),
                                    outbound_dest.get_subnet(),
                                    outbound_dest.get_subnet(),
                                    inbound_source.get_subnet());
                                svc->remove_transport(outbound_zone);
                            }
                            else
                            {
                                RPC_DEBUG(
                                    "remove_passthrough: All counts reached 0 for outbound zone={}, but not "
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
                RPC_WARNING(
                    "remove_passthrough: No zone count found for inbound_source={} in zone={}",
                    inbound_source.get_subnet(),
                    zone_id_.get_subnet());
            }
            else
            {
                auto& inbound_counts = inbound_found->second;
                if (inbound_counts.inbound_passthrough_count == 0)
                {
                    RPC_WARNING(
                        "remove_passthrough: Inbound count already zero for source={} in zone={}",
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
                            auto registered_transport = svc->get_transport(inbound_zone);
                            if (registered_transport && registered_transport.get() == this)
                            {
                                RPC_WARNING(
                                    "remove_passthrough: Removing transport! zone={}, adjacent={}, target_zone={}, "
                                    "passthrough={}->{}, reason=inbound_counts_zero",
                                    zone_id_.get_subnet(),
                                    adjacent_zone_id_.get_subnet(),
                                    inbound_source.get_subnet(),
                                    outbound_dest.get_subnet(),
                                    inbound_source.get_subnet());
                                svc->remove_transport(inbound_zone);
                                RPC_DEBUG(
                                    "remove_passthrough: All counts reached 0 for inbound zone={}, removed transport",
                                    inbound_source.get_subnet());
                            }
                            else
                            {
                                RPC_DEBUG(
                                    "remove_passthrough: All counts reached 0 for inbound zone={}, but not "
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

            RPC_DEBUG(
                "remove_passthrough: zone={}, adjacent={}, outbound_dest={}, inbound_source={}, "
                "remaining_dest_count={}",
                zone_id_.get_subnet(),
                adjacent_zone_id_.get_subnet(),
                outbound_dest.get_subnet(),
                inbound_source.get_subnet(),
                get_destination_count());

#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_remove_destination(
                    zone_id_, adjacent_zone_id_, lookup_val.zone1, lookup_val.zone2);
#endif

            should_shutdown = destination_count_ == 0 && get_status() == transport_status::CONNECTED;
        } // destinations_mutex_ released here

        if (should_shutdown)
        {
            RPC_DEBUG(
                "transport::remove_passthrough: destination_count reached 0, triggering shutdown for zone={}",
                zone_id_.get_subnet());
            on_destination_count_zero();
        }
    }

    std::shared_ptr<pass_through> transport::create_pass_through(
        std::shared_ptr<transport> forward,
        const std::shared_ptr<transport>& reverse,
        const std::shared_ptr<service>& service,
        destination_zone forward_dest,
        destination_zone reverse_dest)
    {
        // Validate: pass-through should only be created between different zones
        if (forward_dest == reverse_dest)
        {
            RPC_ERROR(
                "create_pass_through: Invalid pass-through request - forward_dest and reverse_dest are the same "
                "({})! Pass-throughs should only route between different zones.",
                forward_dest.get_subnet());
            return nullptr;
        }

        // Validate: forward and reverse transports should be different objects
        if (forward.get() == reverse.get())
        {
            RPC_ERROR(
                "create_pass_through: Invalid pass-through request - forward and reverse transports are the same "
                "object! forward_dest={}, reverse_dest={}",
                forward_dest.get_subnet(),
                reverse_dest.get_subnet());
            return nullptr;
        }

        // Lock both transport destination tables before checking or inserting a shared pass-through.
        std::scoped_lock lock(forward->destinations_mutex_, reverse->destinations_mutex_);

        // Check if pass-through already exists for this zone pair
        auto forward_passthrough = forward->inner_get_passthrough(reverse_dest, forward_dest);
        auto reverse_passthrough = reverse->inner_get_passthrough(forward_dest, reverse_dest);

        // check that they are the same
        RPC_ASSERT(!forward_passthrough == !reverse_passthrough);

        if (forward_passthrough)
        {
            RPC_DEBUG(
                "create_pass_through: Found existing pass-through for forward_dest={}, reverse_dest={}",
                forward_dest.get_subnet(),
                reverse_dest.get_subnet());
            return forward_passthrough;
        }

        std::shared_ptr<pass_through> pt(new rpc::pass_through(
            forward, // forward_transport: handles messages TO final destination
            reverse, // reverse_transport: handles messages back to caller
            service, // service
            forward_dest,
            reverse_dest // reverse_destination: where reverse messages go
            ));
        pt->self_ref_ = pt; // keep self alive based on reference counts

        RPC_DEBUG(
            "create_pass_through: Creating NEW passthrough {}->{}, "
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

    std::shared_ptr<pass_through> transport::inner_get_passthrough(
        destination_zone zone1,
        destination_zone zone2) const
    {
        auto lookup_val = make_pass_through_key(zone1, zone2);

        auto it = pass_thoughs_.find(lookup_val);
        if (it != pass_thoughs_.end())
        {
            auto pt = it->second.lock();
            if (!pt)
            {
                RPC_WARNING(
                    "inner_get_passthrough: weak_ptr expired for zone1={}, zone2={} on transport "
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
                if (auto handler = pt.lock())
                    return handler->get_directional_transport(destination_zone_id);
            }
        }
        return nullptr;
    }

    // Helper to route incoming messages to registered handlers
    std::shared_ptr<pass_through> transport::get_passthrough(
        destination_zone zone1,
        destination_zone zone2) const
    {
        RPC_ASSERT(zone1 != zone_id_);
        RPC_ASSERT(zone2 != zone_id_);

        std::shared_lock lock(destinations_mutex_);
        auto handler = inner_get_passthrough(zone1, zone2);
        RPC_DEBUG(
            "get_passthrough: zone1={}, zone2={}, transport zone={}, adjacent_zone={}, found={}",
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
                RPC_ERROR(
                    "notify_all_destinations_of_disconnect: Local service no longer exists on transport zone={} "
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
                zones_to_notify.push_back(zone_item.first);
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
            RPC_DEBUG(
                "notify_all_destinations_of_disconnect: Notifying service about zone={} going down",
                remote_zone.get_subnet());
            CO_AWAIT service->notify_transport_down(self, remote_zone);
        }
    }

    // inbound i_marshaller interface implementation
    // Routes via pass_thoughs_ map (service is registered as a destination)
    CORO_TASK(send_result)
    transport::inbound_send(send_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_send(
                zone_id_, adjacent_zone_id_, params.remote_object_id, params.caller_zone_id, params.interface_id, params.method_id);
        }
#endif

        // Reject new RPC calls when shutting down; cleanup methods are exempt
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN send_result{error::TRANSPORT_ERROR(), {}, {}};
        }

        std::shared_ptr<i_marshaller> dest;
        if (params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(params.remote_object_id.as_zone(), params.caller_zone_id);
            if (!dest)
            {
                CO_RETURN send_result{error::ZONE_NOT_FOUND(), {}, {}};
            }
        }

        CO_RETURN CO_AWAIT dest->send(std::move(params));
    }

    CORO_TASK(void)
    transport::inbound_post(post_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_post(
                zone_id_, adjacent_zone_id_, params.remote_object_id, params.caller_zone_id, params.interface_id, params.method_id);
        }
#endif

        // Reject new RPC calls when shutting down; cleanup methods are exempt
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN;
        }

        std::shared_ptr<i_marshaller> dest;
        if (params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(params.remote_object_id.as_zone(), params.caller_zone_id);
            if (!dest)
            {
                CO_RETURN;
            }
        }

        CO_AWAIT dest->post(std::move(params));
    }

    CORO_TASK(standard_result)
    transport::inbound_try_cast(try_cast_params params)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_try_cast(
                zone_id_, adjacent_zone_id_, params.remote_object_id, params.caller_zone_id, params.interface_id);
        }
#endif

        // Reject new RPC calls when shutting down; cleanup methods are exempt
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        std::shared_ptr<i_marshaller> dest;
        if (params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(params.remote_object_id.as_zone(), params.caller_zone_id);
            if (!dest)
            {
                CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
            }
        }

        CO_RETURN CO_AWAIT dest->try_cast(std::move(params));
    }

    CORO_TASK(standard_result)
    transport::inbound_add_ref(add_ref_params params)
    {
        // Reject new add_ref calls when shutting down
        if (get_status() >= transport_status::DISCONNECTING)
        {
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        bool build_caller_channel = !!(params.build_out_param_channel & add_ref_options::build_caller_route);
        bool build_dest_channel = !!(params.build_out_param_channel & add_ref_options::build_destination_route)
                                  || params.build_out_param_channel == add_ref_options::normal
                                  || params.build_out_param_channel == add_ref_options::optimistic;

        auto svc = service_.lock();
        if (!svc)
        {
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }

        RPC_DEBUG(
            "inbound_add_ref: svc_zone={}, dest_zone={}, caller_zone={}, build_caller_channel={}, "
            "build_dest_channel={}, requesting_zone_id={}",
            svc->get_zone_id().get_subnet(),
            params.remote_object_id.get_subnet(),
            params.caller_zone_id.get_subnet(),
            build_caller_channel,
            build_dest_channel,
            params.requesting_zone_id.get_subnet());

        // Returns a copy of params with a modified build_out_param_channel flag
        auto make_add_ref_params = [&](add_ref_options opts) -> add_ref_params
        {
            add_ref_params p = params;
            p.build_out_param_channel = opts;
            return p;
        };

        if (!params.remote_object_id.get_address().same_zone(svc->get_zone_id().get_address())
            && params.caller_zone_id != svc->get_zone_id())
        {
            auto dest_transport = svc->get_transport(params.remote_object_id.as_zone());
            if (params.remote_object_id.get_address().same_zone(params.caller_zone_id.get_address()))
            {
                // caller and destination are the same zone, so we just call the transport to pass the call along and not involve a pass through
                if (!dest_transport)
                {
                    CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
                }

                // here we
                auto result = CO_AWAIT dest_transport->add_ref(make_add_ref_params(params.build_out_param_channel));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_transport_inbound_add_ref(
                        zone_id_,
                        adjacent_zone_id_,
                        params.remote_object_id,
                        params.caller_zone_id,
                        params.requesting_zone_id,
                        params.build_out_param_channel);
                }
#endif
                CO_RETURN result;
            }
            if (!dest_transport)
            {
                if (build_dest_channel)
                {
                    dest_transport = inner_get_transport_from_passthroughs(params.remote_object_id.as_zone());
                    if (!dest_transport)
                    {
                        dest_transport = svc->get_transport(params.requesting_zone_id);
                    }
                    if (!dest_transport && params.remote_object_id != params.requesting_zone_id)
                    {
                        dest_transport = inner_get_transport_from_passthroughs(params.requesting_zone_id);
                    }
                    if (!dest_transport)
                    {
                        CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
                    }
                }
                else
                {
                    dest_transport = shared_from_this();
                }
                svc->add_transport(params.remote_object_id.as_zone(), dest_transport);
            }

            // otherwise we are going to use or create a pass-through
            {
                auto passthrough
                    = dest_transport->get_passthrough(params.caller_zone_id, params.remote_object_id.as_zone());
                if (passthrough)
                {
                    CO_RETURN CO_AWAIT passthrough->add_ref(make_add_ref_params(params.build_out_param_channel));
                }
            }

            auto caller_transport = svc->get_transport(params.caller_zone_id);
            if (!caller_transport)
            {
                if (!build_dest_channel && build_caller_channel)
                {
                    caller_transport = inner_get_transport_from_passthroughs(params.caller_zone_id);
                    if (!caller_transport)
                    {
                        caller_transport = svc->get_transport(params.requesting_zone_id);
                    }
                    if (!caller_transport && params.caller_zone_id != params.requesting_zone_id)
                    {
                        caller_transport = inner_get_transport_from_passthroughs(params.requesting_zone_id);
                    }
                    if (!caller_transport)
                    {
                        CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
                    }
                }
                else
                {
                    caller_transport = shared_from_this();
                }
                svc->add_transport(params.caller_zone_id, caller_transport);
            }

            if (dest_transport == caller_transport)
            {
                // here we directly call the destination
                auto result = CO_AWAIT dest_transport->add_ref(make_add_ref_params(params.build_out_param_channel));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_transport_inbound_add_ref(
                        zone_id_,
                        adjacent_zone_id_,
                        params.remote_object_id,
                        params.caller_zone_id,
                        params.requesting_zone_id,
                        params.build_out_param_channel);
                }
#endif
                CO_RETURN result;
            }

            auto passthrough = transport::create_pass_through(
                dest_transport, caller_transport, svc, params.remote_object_id.as_zone(), params.caller_zone_id);

            auto result = CO_AWAIT passthrough->add_ref(make_add_ref_params(params.build_out_param_channel));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_inbound_add_ref(
                    zone_id_,
                    adjacent_zone_id_,
                    params.remote_object_id,
                    params.caller_zone_id,
                    params.requesting_zone_id,
                    params.build_out_param_channel);
            }
#endif
            CO_RETURN result;
        }

        // else it is a special case that the service needs to deal with

        auto result = CO_AWAIT svc->add_ref(make_add_ref_params(params.build_out_param_channel));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_add_ref(
                zone_id_,
                adjacent_zone_id_,
                params.remote_object_id,
                params.caller_zone_id,
                params.requesting_zone_id,
                params.build_out_param_channel);
        }
#endif
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    transport::inbound_release(release_params params)
    {
        std::shared_ptr<i_marshaller> dest;
        if (params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(params.remote_object_id.as_zone(), params.caller_zone_id);
            if (!dest)
            {
                CO_RETURN standard_result{error::ZONE_NOT_FOUND(), {}};
            }
        }

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        auto telemetry_remote_object_id = params.remote_object_id;
        auto telemetry_caller_zone_id = params.caller_zone_id;
        auto telemetry_options = params.options;
#endif
        auto result = CO_AWAIT dest->release(std::move(params));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_release(
                zone_id_, adjacent_zone_id_, telemetry_remote_object_id, telemetry_caller_zone_id, telemetry_options);
        }
#endif
        CO_RETURN result;
    }

    CORO_TASK(void)
    transport::inbound_object_released(object_released_params params)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_object_released(
                zone_id_, adjacent_zone_id_, params.remote_object_id, params.caller_zone_id);
        }
#endif

        if (params.caller_zone_id == get_zone_id())
        {
            CO_AWAIT get_service() -> object_released(params);
            CO_RETURN;
        }

        // Try zone pair lookup
        std::shared_ptr<i_marshaller> dest;
        if (params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(params.remote_object_id.as_zone(), params.caller_zone_id);
            if (!dest)
            {
                // Zone not found - just return without propagating error
                CO_RETURN;
            }
        }

        CO_AWAIT dest->object_released(std::move(params));
    }

    CORO_TASK(void)
    transport::inbound_transport_down(transport_down_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_inbound_transport_down(
                zone_id_, adjacent_zone_id_, params.destination_zone_id, params.caller_zone_id);
        }
#endif

        std::shared_ptr<i_marshaller> dest;
        if (params.destination_zone_id == zone_id_)
        {
            dest = service_.lock();
        }
        else
        {
            // Try zone pair lookup first
            dest = get_passthrough(params.destination_zone_id, params.caller_zone_id);
            if (!dest)
            {
                CO_RETURN;
            }
        }

        auto caller_zone_id = params.caller_zone_id;
        CO_AWAIT dest->transport_down(std::move(params));

        {
            std::unique_lock lock(destinations_mutex_);
            zone_counts_.erase(caller_zone_id);
        }
    }

    CORO_TASK(send_result) transport::send(send_params params)
    {
        [[maybe_unused]] auto remote_object_id = params.remote_object_id;
        [[maybe_unused]] auto caller_zone_id = params.caller_zone_id;
        [[maybe_unused]] auto interface_id = params.interface_id;
        [[maybe_unused]] auto method_id = params.method_id;
        auto result = CO_AWAIT outbound_send(std::move(params));
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (result.error_code == error::OK() || result.error_code == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_send(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, interface_id, method_id);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call outbound_send");
            }
        }
#endif
        CO_RETURN result;
    }

    CORO_TASK(void) transport::post(post_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        auto remote_object_id = params.remote_object_id;
        auto caller_zone_id = params.caller_zone_id;
        auto interface_id = params.interface_id;
        auto method_id = params.method_id;
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_post(
                get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, interface_id, method_id);
        }
#endif
        CO_AWAIT outbound_post(std::move(params));
    }

    CORO_TASK(standard_result) transport::try_cast(try_cast_params params)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        auto remote_object_id = params.remote_object_id;
        auto interface_id = params.interface_id;
#endif
        auto result = CO_AWAIT outbound_try_cast(std::move(params));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (result.error_code == error::OK() || result.error_code == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_try_cast(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, get_zone_id(), interface_id);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call outbound_try_cast");
            }
        }
#endif
        CO_RETURN result;
    }

    CORO_TASK(standard_result) transport::add_ref(add_ref_params params)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        auto remote_object_id = params.remote_object_id;
        auto caller_zone_id = params.caller_zone_id;
        auto requesting_zone_id = params.requesting_zone_id;
        auto build_out_param_channel = params.build_out_param_channel;
#endif
        auto result = CO_AWAIT outbound_add_ref(std::move(params));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (result.error_code == error::OK() || result.error_code == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_add_ref(
                    get_zone_id(),
                    get_adjacent_zone_id(),
                    remote_object_id,
                    caller_zone_id,
                    requesting_zone_id,
                    build_out_param_channel);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call outbound_add_ref");
            }
        }
#endif
        CO_RETURN result;
    }

    CORO_TASK(standard_result) transport::release(release_params params)
    {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        auto remote_object_id = params.remote_object_id;
        auto caller_zone_id = params.caller_zone_id;
        auto options = params.options;
#endif
        auto result = CO_AWAIT outbound_release(std::move(params));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            if (result.error_code == error::OK() || result.error_code == error::OBJECT_GONE())
            {
                telemetry_service->on_transport_outbound_release(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, options);
            }
            else
            {
                // RPC_ASSERT(false);
                telemetry_service->message(rpc::i_telemetry_service::level_enum::err, "failed to call outbound_release");
            }
        }
#endif
        CO_RETURN result;
    }

    CORO_TASK(void) transport::object_released(object_released_params params)
    {
        auto caller_zone_id = params.caller_zone_id;
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        auto remote_object_id = params.remote_object_id;
#endif
        CO_AWAIT outbound_object_released(std::move(params));
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_object_released(
                get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id);
        }
#endif
        decrement_inbound_stub_count(caller_zone_id);
    }

    CORO_TASK(void) transport::transport_down(transport_down_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        auto destination_zone_id = params.destination_zone_id;
        auto caller_zone_id = params.caller_zone_id;
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_transport_down(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id);
        }
#endif
        CO_AWAIT outbound_transport_down(std::move(params));
    }

    CORO_TASK(new_zone_id_result) transport::get_new_zone_id(get_new_zone_id_params params)
    {
        CO_RETURN CO_AWAIT outbound_get_new_zone_id(std::move(params));
    }

    CORO_TASK(new_zone_id_result) transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        // Default: delegate to the local service.
        // For a root service this allocates locally; for a child_service it
        // forwards the request up to the parent zone.
        auto svc = service_.lock();
        if (!svc)
            CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        CO_RETURN CO_AWAIT svc->get_new_zone_id(std::move(params));
    }

} // namespace rpc
