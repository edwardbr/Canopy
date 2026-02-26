/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include <rpc/rpc_types.h>

namespace canopy::network_config
{

    // Allocates zone and object IDs within a bounded subnet range.
    //
    // In local-only mode (routing_prefix=0, subnet_base=0, subnet_range=max),
    // this behaves identically to the legacy std::atomic<uint64_t> counter:
    //   rpc::zone{++zone_gen}
    //
    // In network mode, routing_prefix identifies the physical node and
    // subnet_base/subnet_range carve out a slice of the subnet_id space
    // reserved for this process instance.
    //
    // The allocator is designed to remain valid if zone_address migrates from
    // separate (routing_prefix, subnet_id, object_id) fields to a single
    // 128-bit address field: allocation is done entirely through the
    // zone_address setter API rather than field-count-specific constructors,
    // so only the prefix_ initialisation needs updating when the layout changes.
    class zone_address_allocator
    {
        rpc::zone_address prefix_; // routing_prefix set; subnet_id and object_id = 0
        uint64_t subnet_base_;
        uint64_t subnet_range_;
        std::atomic<uint64_t> next_subnet_;
        std::atomic<uint64_t> next_object_;

    public:
        // Construct from a raw routing prefix value (current uint64_t encoding).
        // subnet_base/subnet_range default to the full available subnet space.
        explicit zone_address_allocator(
            uint64_t routing_prefix = 0, uint64_t subnet_base = 0, uint64_t subnet_range = rpc::zone_address::max_subnet)
            : prefix_()
            , subnet_base_(subnet_base)
            , subnet_range_(subnet_range)
            , next_subnet_(0)
            , next_object_(1)
        {
            prefix_.set_routing_prefix(routing_prefix);
        }

        // Construct from an existing zone_address (routing_prefix portion only).
        // Useful when the caller already holds a zone_address from auto-detection.
        explicit zone_address_allocator(const rpc::zone_address& prefix,
            uint64_t subnet_base = 0,
            uint64_t subnet_range = rpc::zone_address::max_subnet)
            : prefix_(prefix.zone_only())
            , subnet_base_(subnet_base)
            , subnet_range_(subnet_range)
            , next_subnet_(0)
            , next_object_(1)
        {
            prefix_.set_subnet(0);
        }

        // Allocate the next zone address (object_id = 0).
        // Uses the zone_address setter API so this stays correct if the internal
        // layout changes to a single 128-bit field.
        rpc::zone_address allocate_zone()
        {
            auto offset = next_subnet_.fetch_add(1, std::memory_order_relaxed);
            if (offset >= subnet_range_)
            {
                throw std::overflow_error("zone_address_allocator: subnet range exhausted");
            }
            rpc::zone_address addr = prefix_;
            addr.set_subnet(subnet_base_ + offset);
            return addr;
        }

        // Allocate the next object ID within the current zone.
        rpc::object allocate_object()
        {
            auto id = next_object_.fetch_add(1, std::memory_order_relaxed);
            return rpc::object{id};
        }

        uint64_t routing_prefix() const { return prefix_.get_routing_prefix(); }
        uint64_t subnet_base() const { return subnet_base_; }
        uint64_t subnet_range() const { return subnet_range_; }
    };

} // namespace canopy::network_config
