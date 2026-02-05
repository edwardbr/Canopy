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
        DISCONNECTING, // Beginning to shut down a close signal is being sent or recieved
        DISCONNECTED // Terminal state close signal has been acknowleged, or there is a terminal failure, no further traffic allowed
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
            return (std::size_t)item.zone1.get_val() + (std::size_t)item.zone2.get_val();
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
        transport(std::string name, std::shared_ptr<service> service, zone adjacent_zone_id);
        transport(std::string name, zone zone_id, zone adjacent_zone_id);

        // lock free version of same function - adds handler for zone pair
        bool inner_add_passthrough(destination_zone zone1, destination_zone zone2, std::weak_ptr<pass_through> handler);

        // Helper to route incoming messages to registered handlers
        // Gets handler for specific zone pair
        std::shared_ptr<pass_through> inner_get_passthrough(destination_zone zone1, destination_zone zone2) const;

        std::shared_ptr<transport> inner_get_transport_from_passthroughs(destination_zone destination_zone_id) const;

        void inner_increment_outbound_proxy_count(destination_zone dest);
        void inner_decrement_outbound_proxy_count(destination_zone dest);

        void inner_increment_inbound_stub_count(caller_zone dest);
        void inner_decrement_inbound_stub_count(caller_zone dest);

        void set_adjacent_zone_id(zone new_adjacent_zone_id) { adjacent_zone_id_ = new_adjacent_zone_id; }
        CORO_TASK(void) notify_all_destinations_of_disconnect();

    public:
        virtual ~transport();

        std::string get_name() const { return name_; }

        std::shared_ptr<service> get_service() const { return service_.lock(); }
        void set_service(std::shared_ptr<service> service);

        // Destination management for zone pairs
        // For local service, use add_passthrough(local_zone, local_zone, service)
        std::shared_ptr<pass_through> get_passthrough(destination_zone zone1, destination_zone zone2) const;

        void remove_passthrough(destination_zone zone1, destination_zone zone2);

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
         * @brief Get total count of destinations reachable through this transport
         * @return Number of destinations
         */
        int64_t get_destination_count() { return destination_count_.load(); }

        static std::shared_ptr<pass_through> create_pass_through(std::shared_ptr<transport> forward,
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
         * @brief Initiate connection handshake with remote zone
         * @param input_descr Descriptor of interface to send to remote zone
         * @param output_descr[out] Descriptor of interface received from remote zone
         * @return error::OK() on success, error code on failure
         *
         * Delegates to inner_connect() which derived classes implement.
         *
         * Thread-Safety: Implementation-specific (varies by derived class)
         */
        CORO_TASK(int) connect(interface_descriptor input_descr, interface_descriptor& output_descr);

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
         * @param protocol_version RPC protocol version
         * @param encoding Serialization format
         * @param tag Operation tag for tracing
         * @param caller_zone_id Zone making the call
         * @param destination_zone_id Target zone (may be local or passthrough)
         * @param object_id Target object ID
         * @param interface_id Target interface ordinal
         * @param method_id Method to invoke
         * @param in_data Serialized input parameters
         * @param out_buf_[out] Buffer for serialized return value
         * @param in_back_channel Input back-channel data for reference counting
         * @param out_back_channel[out] Output back-channel data for reference counting
         * @return error::OK() on success, error code on failure
         *
         * Routes to local service if destination matches zone_id_, otherwise
         * routes through passthrough to reach non-adjacent zone.
         */
        CORO_TASK(int)
        inbound_send(uint64_t protocol_version,
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
            std::vector<back_channel_entry>& out_back_channel);

        CORO_TASK(void)
        inbound_post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            const std::vector<back_channel_entry>& in_back_channel);

        CORO_TASK(int)
        inbound_try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel);

        CORO_TASK(int)
        inbound_add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            known_direction_zone known_direction_zone_id,
            add_ref_options build_out_param_channel,

            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel);

        CORO_TASK(int)
        inbound_release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            release_options options,

            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel);

        CORO_TASK(void)
        inbound_object_released(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            const std::vector<back_channel_entry>& in_back_channel);

        CORO_TASK(void)
        inbound_transport_down(uint64_t protocol_version,
            destination_zone destination_zone_id,
            caller_zone caller_zone_id,
            const std::vector<back_channel_entry>& in_back_channel);

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
        CORO_TASK(int)
        send(uint64_t protocol_version,
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
            std::vector<back_channel_entry>& out_back_channel) final;

        CORO_TASK(void)
        post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            const std::vector<back_channel_entry>& in_back_channel) final;

        CORO_TASK(int)
        try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel) final;

        CORO_TASK(int)
        add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            known_direction_zone known_direction_zone_id,
            add_ref_options build_out_param_channel,
            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel) final;

        CORO_TASK(int)
        release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            release_options options,
            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel) final;

        CORO_TASK(void)
        object_released(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            const std::vector<back_channel_entry>& in_back_channel) final;

        CORO_TASK(void)
        transport_down(uint64_t protocol_version,
            destination_zone destination_zone_id,
            caller_zone caller_zone_id,
            const std::vector<back_channel_entry>& in_back_channel) final;

        /////////////////////////////////
        // VIRTUAL METHODS - Derived classes implement transport-specific behavior
        /////////////////////////////////

        /**
         * @brief Establish connection with remote zone
         * @param input_descr Descriptor of interface to send to remote zone
         * @param output_descr[out] Descriptor of interface received from remote zone
         * @return error::OK() on success, error code on failure
         *
         * Derived classes implement the transport-specific connection handshake.
         * Examples:
         * - TCP: Network socket connection
         * - SPSC: Queue initialization
         * - Local: Direct function call to child zone creation
         *
         * Thread-Safety: Implementation-specific
         */
        virtual CORO_TASK(int) inner_connect(interface_descriptor input_descr, interface_descriptor& output_descr) = 0;

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
        virtual CORO_TASK(int) outbound_send(uint64_t protocol_version,
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
            = 0;

        virtual CORO_TASK(void) outbound_post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            const std::vector<back_channel_entry>& in_back_channel)
            = 0;

        virtual CORO_TASK(int) outbound_try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel)
            = 0;

        virtual CORO_TASK(int) outbound_add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            known_direction_zone known_direction_zone_id,
            add_ref_options build_out_param_channel,

            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel)
            = 0;

        virtual CORO_TASK(int) outbound_release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            release_options options,

            const std::vector<back_channel_entry>& in_back_channel,
            std::vector<back_channel_entry>& out_back_channel)
            = 0;

        virtual CORO_TASK(void) outbound_object_released(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            const std::vector<back_channel_entry>& in_back_channel)
            = 0;

        virtual CORO_TASK(void) outbound_transport_down(uint64_t protocol_version,
            destination_zone destination_zone_id,
            caller_zone caller_zone_id,
            const std::vector<back_channel_entry>& in_back_channel)
            = 0;
    };

    class transport_to_parent : public transport
    {
        std::shared_ptr<transport> parent_;

    public:
        transport_to_parent(std::string name, std::shared_ptr<service> service, std::shared_ptr<transport> parent)
            : transport(name, service, parent->get_zone_id())
            , parent_(parent)
        {
        }
    };

} // namespace rpc
