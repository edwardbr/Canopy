/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>

#include <rpc/rpc.h>

namespace rpc
{
    // Allocates zone and object IDs within the subnet range encoded in a zone_address prefix.
    //
    // The prefix passed to the constructor determines the routing prefix and, in the
    // flexible layout, the subnet and object field offsets.  Each call to allocate_zone()
    // increments the subnet counter and embeds the new value into a copy of the prefix
    // via zone_address::set_subnet(), which enforces the field-width limit and returns
    // false when the range is exhausted.
    class zone_id_allocator
    {
        zone_address prefix_; // routing prefix with subnet = 0
        std::atomic<uint64_t> next_subnet_;
        std::atomic<uint64_t> next_object_;

    public:
        explicit zone_id_allocator(const zone_address& prefix)
            : prefix_(prefix.zone_only())
            , next_subnet_(prefix.get_subnet() + 1)
            , next_object_(1)
        {
        }

        // Allocate the next zone address (object_id = 0).
        // Returns rpc::error::OK() on success, rpc::error::INVALID_DATA() when the
        // subnet field of the prefix is exhausted.
        int allocate_zone(zone_address& addr)
        {
            auto subnet = next_subnet_.fetch_add(1, std::memory_order_relaxed);
            addr = prefix_;
            if (!addr.set_subnet(subnet))
                return rpc::error::INVALID_DATA();
            return rpc::error::OK();
        }

        // Allocate the next object ID within the current zone.
        object allocate_object() { return object{next_object_.fetch_add(1, std::memory_order_relaxed)}; }

        uint64_t routing_prefix() const { return prefix_.get_routing_prefix(); }
    };

} // namespace rpc
