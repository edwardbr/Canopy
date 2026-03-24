/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/**
 * @file stub.h
 * @brief Server-side RPC endpoints that wrap local objects for remote access
 *
 * Stubs are the server-side counterpart to proxies in the Canopy RPC system.
 * They handle incoming RPC calls from remote zones and dispatch them to local
 * C++ objects.
 *
 * Key Responsibilities:
 * - Unmarshal incoming RPC calls and parameters
 * - Dispatch calls to wrapped local objects
 * - Marshal return values back to remote callers
 * - Track reference counts per zone (shared and optimistic)
 * - Keep service alive via strong reference
 *
 * Reference Counting Modes:
 * - Shared: RAII-based reference counting (rpc::shared_ptr behavior)
 * - Optimistic: Non-owning references (rpc::optimistic_ptr behavior)
 *
 * Stub Lifetime Management:
 * - Stubs hold strong reference to service (keeps service alive)
 * - Service holds strong references to stubs (in stubs_ map)
 * - Stubs deleted when shared_count_ reaches zero
 * - Per-zone reference tracking enables cleanup on transport_down
 *
 * See documents/architecture/05-proxies-and-stubs.md for complete details.
 */

#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>

#include <rpc/internal/assert.h>
#include <rpc/internal/types.h>
#include <rpc/internal/marshaller.h>
#include <rpc/internal/remote_pointer.h>
#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/i_telemetry_service.h>
#endif
#include <rpc/internal/service.h>

namespace rpc
{
    class object_stub;
    class service;
    class service_proxy;
    class casting_interface;

    /**
     * @brief Server-side RPC endpoint wrapping a local object
     *
     * object_stub is the container for all interface stubs that wrap a single
     * C++ object. It manages:
     * - Object lifetime via reference counting (shared and optimistic)
     * - Per-zone reference tracking for cleanup
     * - Interface casting between different interface types
     * - RPC call routing to appropriate interface stub
     *
     * Reference Counting:
     * - shared_count_: Total shared (RAII) references across all zones
     * - optimistic_count_: Total optimistic (non-owning) references
     * - shared_references_: Per-zone shared reference counts
     * - optimistic_references_: Per-zone optimistic reference counts
     *
     * When shared_count_ reaches zero, the stub is deleted from the service.
     * Per-zone tracking enables cleanup when a zone disconnects (transport_down).
     *
     * Outcall Parameter:
     * The 'outcall' parameter in add_ref() indicates whether the reference
     * is being created as an out parameter during an RPC call. This affects
     * reference counting logic in passthrough scenarios where the caller_zone
     * parameter must be used correctly.
     *
     * Thread Safety:
     * - Reference counts use atomic operations
     * - Interface map protected by map_control_ mutex
     * - Per-zone reference maps protected by references_mutex_
     *
     * Lifetime Management:
     * - Holds strong reference to service (zone_) to keep service alive
     * - Service holds strong references to stubs (in stubs_ map)
     * - Self-reference (p_keep_self_alive_) enables shared_from_this pattern
     *
     * See documents/architecture/04-memory-management.md for reference counting details.
     */
    class object_stub : public std::enable_shared_from_this<object_stub>
    {
        // Unique object ID within the zone
        object id_ = {0};

        // Root interface target for this local object
        rpc::shared_ptr<rpc::casting_interface> target_;

        // Self-reference for shared_from_this pattern
        std::shared_ptr<object_stub> p_keep_self_alive_;

        // Global reference counts (sum across all zones)
        std::atomic<uint64_t> shared_count_ = 0;     // RAII references (rpc::shared_ptr)
        std::atomic<uint64_t> optimistic_count_ = 0; // Non-owning references (rpc::optimistic_ptr)

        // Per-zone reference tracking for transport_down cleanup
        std::unordered_map<caller_zone, std::atomic<uint64_t>> optimistic_references_;
        std::unordered_map<caller_zone, std::atomic<uint64_t>> shared_references_;
        mutable std::mutex references_mutex_; // Protects both reference maps

        // CRITICAL: Strong reference to service keeps service alive while stub exists
        std::shared_ptr<service> zone_;

    public:
        object_stub(
            object id,
            const std::shared_ptr<service>& zone,
            const rpc::shared_ptr<rpc::casting_interface>& target);
        ~object_stub();

        object get_id() const { return id_; }
        rpc::shared_ptr<rpc::casting_interface> get_castable_interface(interface_ordinal interface_id = {0}) const;

        /**
         * @brief Activate lifetime management for this stub
         * @param stub Shared pointer to this object_stub
         *
         * Called after stub is added to service's stubs_ map. Enables the
         * shared_from_this pattern by storing self-reference.
         */
        void keep_self_alive() { p_keep_self_alive_ = shared_from_this(); }
        void dont_keep_alive() { p_keep_self_alive_.reset(); }

        uint64_t get_shared_count() const { return shared_count_; }
        uint64_t get_optimistic_count() const { return optimistic_count_; }

        std::shared_ptr<service> get_zone() const { return zone_; }

        /**
         * @brief Dispatch incoming RPC call to appropriate interface stub
         * @param params Owned parameter bundle containing protocol_version, encoding, caller_zone_id,
         *               interface_id, method_id, and serialized input data
         * @return send_result containing error_code and out_buf
         *
         * Thread-Safety: Protected by map_control_ for interface lookup
         */
        CORO_TASK(send_result) call(send_params params);

        /**
         * @brief Check if this object supports the requested interface
         * @param interface_id Interface ordinal to check
         * @return error::OK() if supported, error code otherwise
         */
        int try_cast(interface_ordinal interface_id);

        /**
         * @brief Add reference to this stub
         * @param is_optimistic true for optimistic reference, false for shared reference
         * @param outcall true if reference is being created as out parameter in RPC call
         * @param caller_zone_id Zone adding the reference
         * @return New reference count (shared or optimistic depending on is_optimistic)
         *
         * The outcall parameter affects reference counting in passthrough scenarios.
         * When outcall=true, the reference is part of an out parameter being passed
         * back through the call chain, and the caller_zone must be used correctly
         * to track which zone owns the reference.
         *
         * Thread-Safety: Uses atomic operations for counts, mutex for per-zone maps
         */
        CORO_TASK(int)
        add_ref(
            bool is_optimistic,
            bool outcall,
            caller_zone caller_zone_id);

        /**
         * @brief Release reference to this stub
         * @param is_optimistic true for optimistic reference, false for shared reference
         * @param caller_zone_id Zone releasing the reference
         * @return New reference count (shared or optimistic depending on is_optimistic)
         *
         * When shared_count_ reaches zero, the stub will be deleted from the service.
         *
         * Thread-Safety: Uses atomic operations for counts, mutex for per-zone maps
         */
        uint64_t release(
            bool is_optimistic,
            caller_zone caller_zone_id);

        /**
         * @brief Release all references from a specific zone (called by service)
         * @param caller_zone_id Zone whose references should be released
         *
         * Used during service shutdown to clean up references.
         */
        CORO_TASK(void) release_from_service(caller_zone caller_zone_id);

        /**
         * @brief Check if zone has any references to this stub
         * @param caller_zone_id Zone to check
         * @return true if zone has shared or optimistic references, false otherwise
         *
         * Used to determine if stub should be cleaned up when a zone disconnects.
         *
         * Thread-Safety: Protected by references_mutex_
         */
        bool has_references_from_zone(caller_zone caller_zone_id) const;

        /**
         * @brief Release all references (shared and optimistic) from a specific zone
         * @param caller_zone_id Zone whose references should be released
         * @return true if stub should be deleted (shared count reached zero), false otherwise
         *
         * Called during transport_down to clean up all references from a disconnected zone.
         * This prevents dangling references when a zone becomes unreachable.
         *
         * Thread-Safety: Uses atomic operations for counts, mutex for per-zone maps
         */
        bool release_all_from_zone(caller_zone caller_zone_id);

        /**
         * @brief Snapshot of all zones with at least one active optimistic reference
         * @return Vector of caller_zone values whose optimistic reference count is > 0
         *
         * Used by the service when the shared count reaches zero to discover which zones
         * still hold optimistic references so that object_released notifications can be
         * dispatched to them.
         *
         * Thread-Safety: Protected internally by references_mutex_
         */
        std::vector<caller_zone> get_zones_with_optimistic_refs() const;
    };
}
