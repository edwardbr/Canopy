/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/**
 * @file transport.h
 * @brief Transport abstraction for bidirectional zone-to-zone communication
 *
 * Transports connect two adjacent zones and handle the marshalling/unmarshalling
 * of RPC calls between them. Each transport manages:
 * - Bidirectional communication (inbound and outbound methods)
 * - Reference counting across zone boundaries
 * - Passthrough routing to non-adjacent zones
 * - Connection lifecycle (CONNECTING → CONNECTED → DISCONNECTED)
 *
 * Transport Ownership Model:
 * - Service proxies own transports (strong member_ptr reference)
 * - Passthroughs own both forward and reverse transports
 * - Child services hold strong references to parent transports
 * - Services hold only weak references (registry for lookup)
 *
 * See documents/architecture/06-transports-and-passthroughs.md and
 * documents/transports/ for transport implementations.
 */

#pragma once

#include <rpc/internal/marshaller.h>
#include <rpc/internal/types.h>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <atomic>

// Forward declaration to avoid circular dependency
namespace rpc
{
    class service;
}

namespace rpc
{
    class pass_through;

    /**
     * @brief Transport connection status lifecycle
     *
     * Status Progression:
     * CONNECTING → CONNECTED → DISCONNECTING (optional) → DISCONNECTED (normal flow)
     * Any state → DISCONNECTED (error/shutdown)
     *
     * DISCONNECTED is a terminal state - no further traffic allowed.
     */
    enum class transport_status
    {
        CONNECTING,    // Initial state, establishing connection
        CONNECTED,     // Fully operational
        DISCONNECTING, // Beginning to shut down, a close signal is being sent or received
        DISCONNECTED // Terminal state, close signal has been acknowledged, or there is a terminal failure, no further traffic allowed
    };

    /**
     * @brief Key for identifying passthrough routes between zone pairs
     *
     * Used to register and lookup passthrough handlers that route RPC calls
     * through intermediary zones to reach non-adjacent destinations.
     */
    struct pass_through_key
    {
        destination_zone zone1;
        destination_zone zone2;

        bool operator==(const pass_through_key& other) const noexcept
        {
            return zone1 == other.zone1 && zone2 == other.zone2;
        }
    };
}

namespace std
{
    template<> struct hash<rpc::pass_through_key>
    {
        auto operator()(const rpc::pass_through_key& item) const noexcept
        {
            return (std::size_t)item.zone1.get_subnet() + (std::size_t)item.zone2.get_subnet();
        }
    };
}

namespace rpc
{

    /**
     * @brief Base class for all transport implementations
     *
     * A transport connects two adjacent zones and handles bidirectional RPC
     * communication. It implements the i_marshaller interface to route calls
     * between zones.
     *
     * Key Responsibilities:
     * - Marshal/unmarshal RPC calls between zones
     * - Track reference counts (inbound stubs, outbound proxies)
     * - Route calls to local service or remote transport
     * - Manage passthrough routing for non-adjacent zones
     * - Handle connection lifecycle and status transitions
     *
     * Inbound vs Outbound Methods:
     * - inbound_*: Process calls arriving from remote zone (route to local service or passthrough)
     * - outbound_*: Send calls to remote zone (implemented by derived classes)
     *
     * Reference Counting:
     * - zone_counts_: Tracks proxies in local zone pointing to remote objects and stubs in local zone referenced by remote proxies
     * - Used to determine when transport can safely disconnect
     *
     * Back-Channel Data Flow:
     * Back-channel vectors carry reference counting operations:
     * - in_back_channel: Reference operations arriving with the call
     * - out_back_channel: Reference operations to send with the response
     *
     * Passthrough Routing:
     * Transports can route calls to non-adjacent zones through intermediary
     * zones using passthrough handlers registered for zone pairs.
     *
     * Thread Safety:
     * - Status changes protected by atomic operations
     * - Passthrough map protected by destinations_mutex_
     * - Reference counts use atomic variables
     *
     * Ownership Model:
     * - Service proxies own transports (strong member_ptr)
     * - Passthroughs own both forward and reverse transports
     * - Services hold weak references (lookup registry)
     *
     * See documents/architecture/06-transports-and-passthroughs.md for details.
     */
    /**
     * @brief Combined reference count tracking for a remote zone
     *
     * Tracks both directions of references between this transport's zone and a remote zone:
     * - proxy_count: Proxies in local zone referencing objects in the remote zone
     * - stub_count: Stubs in local zone referenced by proxies in the remote zone
     *
     * When both counts drop to zero, the transport to that zone can be removed.
     */
    struct remote_service_count
    {
        std::atomic<uint64_t> proxy_count{0};
        std::atomic<uint64_t> stub_count{0};
        std::atomic<uint64_t> outbound_passthrough_count{0}; // Passthroughs using this transport to reach dest
        std::atomic<uint64_t> inbound_passthrough_count{0};  // Passthroughs using this transport from dest
    };

    class transport : public i_marshaller, public std::enable_shared_from_this<transport>
    {
    private:
        std::string name_;

        // Zone identity
        zone zone_id_;

        // Zone on the other side of the transport
        zone adjacent_zone_id_;

        // Weak reference to local service (lookup only, doesn't keep alive)
        std::weak_ptr<service> service_;

        mutable std::shared_mutex destinations_mutex_;

        // Passthrough routing map for non-adjacent zones
        std::unordered_map<pass_through_key, std::weak_ptr<pass_through>> pass_thoughs_;

        // Combined reference count tracking per zone
        // Maps zone IDs to counts of both outbound proxies and inbound stubs
        std::unordered_map<zone, remote_service_count> zone_counts_;

        std::atomic<int64_t> destination_count_ = 0;

        std::atomic<transport_status> status_{transport_status::CONNECTING};

    protected:
        // Constructor for derived transport classes
        transport(
            std::string name,
            std::shared_ptr<service> service);
        transport(
            std::string name,
            zone zone_id);

        // lock free version - adds handler for zone pair and increments passthrough counts

        // Caller must hold destinations_mutex_
        // outbound_dest: zone reachable via this transport (increments outbound_passthrough_count)
        // inbound_source: zone routing through this transport (increments inbound_passthrough_count)
        bool inner_add_passthrough(
            std::weak_ptr<pass_through> handler,
            destination_zone outbound_dest,
            destination_zone inbound_source);

        // Helper to route incoming messages to registered handlers
        // Gets handler for specific zone pair
        std::shared_ptr<pass_through> inner_get_passthrough(
            destination_zone zone1,
            destination_zone zone2) const;

        std::shared_ptr<transport> inner_get_transport_from_passthroughs(destination_zone destination_zone_id) const;

        void inner_increment_outbound_proxy_count(destination_zone dest);
        bool inner_decrement_outbound_proxy_count(destination_zone dest);

        void inner_increment_inbound_stub_count(caller_zone dest);
        bool inner_decrement_inbound_stub_count(caller_zone dest);
        bool inner_decrement_inbound_stub_count_by(
            caller_zone dest,
            uint64_t count);

        /**
         * @brief Called when destination_count_ drops to zero
         *
         * Derived classes override to initiate graceful shutdown.
         * Default implementation is a no-op.
         * Only called when status is CONNECTED to avoid redundant transitions.
         */
        virtual void on_destination_count_zero() { }

    public:
        ~transport() override;

        std::string get_name() const { return name_; }

        std::shared_ptr<service> get_service() const { return service_.lock(); }
        void set_service(std::shared_ptr<service> service);

        // Destination management for zone pairs
        // For local service, use add_passthrough(local_zone, local_zone, service)
        std::shared_ptr<pass_through> get_passthrough(
            destination_zone zone1,
            destination_zone zone2) const;

        void remove_passthrough(
            destination_zone outbound_dest,
            destination_zone inbound_source);

        /**
         * @brief Increment reference count for proxies pointing to destination zone
         * @param dest The destination zone being referenced
         *
         * Called when a new proxy is created that references an object in dest.
         * Prevents transport disconnect while proxies exist.
         *
         * Thread-Safety: Uses atomic operations
         */
        void increment_outbound_proxy_count(destination_zone dest);

        /**
         * @brief Decrement reference count for proxies pointing to destination zone
         * @param dest The destination zone being dereferenced
         *
         * Called when a proxy to dest is destroyed.
         *
         * Thread-Safety: Uses atomic operations
         */
        void decrement_outbound_proxy_count(destination_zone dest);

        /**
         * @brief Increment reference count for stubs referenced by caller zone
         * @param dest The caller zone holding the reference
         *
         * Called when a stub is referenced by a proxy in dest.
         *
         * Thread-Safety: Uses atomic operations
         */
        void increment_inbound_stub_count(caller_zone dest);

        /**
         * @brief Decrement reference count for stubs referenced by caller zone
         * @param dest The caller zone releasing the reference
         *
         * Called when a proxy in dest releases its reference to a local stub.
         *
         * Thread-Safety: Uses atomic operations
         */
        void decrement_inbound_stub_count(caller_zone dest);

        /**
         * @brief Bulk-decrement reference count for stubs referenced by caller zone
         * @param dest  The caller zone releasing the references
         * @param count Number of references to release in a single lock acquisition
         *
         * Equivalent to calling decrement_inbound_stub_count() count times but
         * acquires destinations_mutex_ only once.
         *
         * Thread-Safety: Uses destinations_mutex_
         */
        void decrement_inbound_stub_count_by(
            caller_zone dest,
            uint64_t count);

        /**
         * @brief Get total count of destinations reachable through this transport
         * @return Number of destinations
         */
        int64_t get_destination_count() { return destination_count_.load(); }

        static std::shared_ptr<pass_through> create_pass_through(
            std::shared_ptr<transport> forward,
            const std::shared_ptr<transport>& reverse,
            const std::shared_ptr<service>& service,
            destination_zone forward_dest,
            destination_zone reverse_dest);

        /**
         * @brief Get current transport status
         * @return Current status (CONNECTING, CONNECTED, DISCONNECTING, or DISCONNECTED)
         *
         * Thread-Safety: Uses atomic load
         */
        transport_status get_status() const;

        /**
         * @brief Set transport status
         * @param new_status New status to set
         *
         * Setting DISCONNECTED triggers cleanup of passthroughs and notifies all
         * destinations. Derived classes override to add custom behavior (e.g.,
         * hierarchical transports propagate disconnection to parent/child).
         *
         * Thread-Safety: Uses atomic operations
         */
        virtual void set_status(transport_status new_status);

        /**
         * @brief Get the local zone ID
         * @return Zone ID of the local zone
         */
        zone get_zone_id() const { return zone_id_; }

        /**
         * @brief Get the adjacent zone ID
         * @return Zone ID of the zone on the other side of this transport
         */
        zone get_adjacent_zone_id() const { return adjacent_zone_id_; }

        /**
         * @brief Set the adjacent zone ID
         * @param zone_id Zone ID of the zone on the other side of this transport
         */
        void set_adjacent_zone_id(zone new_adjacent_zone_id);

        CORO_TASK(void) notify_all_destinations_of_disconnect();

        /**
         * @brief Initiate connection handshake with remote zone
         * @param stub once the adjacent zone id is known if this is not null call add_ref on the stub with the adjacent zone id
         * @param input_descr Descriptor of interface to send to remote zone
         * @return connect_result carrying error code and descriptor received from the remote zone
         *
         * Delegates to inner_connect() which derived classes implement.
         *
         * Thread-Safety: Implementation-specific (varies by derived class)
         */
        CORO_TASK(connect_result)
        connect(
            std::shared_ptr<rpc::object_stub> stub,
            connection_settings input_descr);

        /**
         * @brief Initiate a transport about to receive a connection request
         * @return error::OK() on success, error code on failure
         *
         * Delegates to inner_accept() which derived classes implement.
         *
         * Thread-Safety: Implementation-specific (varies by derived class)
         */
        CORO_TASK(int) accept();

        /////////////////////////////////
        // INBOUND METHODS - Process calls arriving from remote zone
        /////////////////////////////////
        // These methods receive calls from the remote zone and route them to:
        // - Local service (if destination_zone matches zone_id_)
        // - Passthrough handler (if routing to non-adjacent zone)
        //
        // Inbound methods handle back-channel reference counting operations and
        // update inbound_stub_count_ tracking.

        /**
         * @brief Process incoming RPC call with return value
         * @param params Owned parameter bundle (avoids reference params in coroutine frames)
         * @return send_result containing error code, output buffer, and back-channel entries
         *
         * Routes to local service if destination matches zone_id_, otherwise
         * routes through passthrough to reach non-adjacent zone.
         */
        CORO_TASK(send_result) inbound_send(send_params params);

        CORO_TASK(void) inbound_post(post_params params);

        CORO_TASK(standard_result) inbound_try_cast(try_cast_params params);

        CORO_TASK(standard_result) inbound_add_ref(add_ref_params params);

        CORO_TASK(standard_result) inbound_release(release_params params);

        CORO_TASK(void) inbound_object_released(object_released_params params);

        CORO_TASK(void) inbound_transport_down(transport_down_params params);

        /////////////////////////////////
        // OUTBOUND METHODS (i_marshaller implementation) - Send calls to remote zone
        /////////////////////////////////
        // These methods are called by the local service to send RPC calls.
        // They implement the i_marshaller interface (marked final) and delegate
        // to virtual outbound_* methods that derived classes implement.
        //
        // Outbound methods update outbound_proxy_count_ tracking and handle
        // back-channel reference counting operations.

        /**
         * @brief Send RPC call with return value to remote zone
         *
         * Implements i_marshaller::send(). Delegates to outbound_send() which
         * derived classes implement to handle transport-specific serialization
         * and transmission.
         *
         * @see inbound_send for parameter descriptions
         */
        CORO_TASK(send_result) send(send_params params) final;
        CORO_TASK(void) post(post_params params) final;
        CORO_TASK(standard_result) try_cast(try_cast_params params) final;
        CORO_TASK(standard_result) add_ref(add_ref_params params) final;
        CORO_TASK(standard_result) release(release_params params) final;
        CORO_TASK(void) object_released(object_released_params params) final;
        CORO_TASK(void) transport_down(transport_down_params params) final;

        // Requests a new zone ID from the root zone.
        // Non-hierarchical transports forward to the local service allocator.
        // Child-side hierarchical transports override outbound_get_new_zone_id to
        // forward the request up to the parent zone instead.
        CORO_TASK(new_zone_id_result) get_new_zone_id(get_new_zone_id_params params) final;

        virtual CORO_TASK(new_zone_id_result) outbound_get_new_zone_id(get_new_zone_id_params params);

        /////////////////////////////////
        // VIRTUAL METHODS - Derived classes implement transport-specific behavior
        /////////////////////////////////

        /**
         * @brief Establish connection with remote zone
         * @param stub once the adjacent zone id is known if this is not null call add_ref on the stub with the adjacent zone id
         * @param input_descr Descriptor of interface to send to remote zone
         * @return connect_result carrying error code and descriptor received from the remote zone
         *
         * Derived classes implement the transport-specific connection handshake.
         * Examples:
         * - TCP: Network socket connection
         * - SPSC: Queue initialization
         * - Local: Direct function call to child zone creation
         *
         * Thread-Safety: Implementation-specific
         */
        virtual CORO_TASK(connect_result) inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            connection_settings input_descr) = 0;

        virtual CORO_TASK(int) inner_accept() = 0;

        /**
         * @brief Send RPC call to remote zone (transport-specific implementation)
         *
         * Derived classes implement serialization and transmission. Common patterns:
         * - Serialize parameters with encoding format
         * - Send over transport medium (network, queue, function call)
         * - Wait for response
         * - Deserialize return value and back-channel data
         *
         * @see inbound_send for parameter descriptions
         */
        virtual CORO_TASK(send_result) outbound_send(send_params params) = 0;
        virtual CORO_TASK(void) outbound_post(post_params params) = 0;
        virtual CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) = 0;
        virtual CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) = 0;
        virtual CORO_TASK(standard_result) outbound_release(release_params params) = 0;
        virtual CORO_TASK(void) outbound_object_released(object_released_params params) = 0;
        virtual CORO_TASK(void) outbound_transport_down(transport_down_params params) = 0;
    };

} // namespace rpc
