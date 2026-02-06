/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/**
 * @file service.h
 * @brief Core zone management and object lifetime tracking for Canopy RPC
 *
 * This file defines the service class, which represents an execution zone in
 * the Canopy RPC system. A zone is an isolated boundary for object lifetime
 * management, providing the foundation for cross-context communication (in-process,
 * inter-process, remote machines, SGX enclaves).
 *
 * Key Architectural Concepts:
 * - Zone: An execution context with its own object registry and service management
 * - Service: Manages all object lifetimes and RPC communications within a zone
 * - Transport: Connects two adjacent zones for bidirectional communication
 * - Service Proxy: Represents a remote zone and routes calls through transports
 * - Stub: Server-side RPC endpoint that wraps local objects for remote access
 * - Proxy: Client-side RPC endpoint that represents remote objects locally
 *
 * See documents/architecture/01-overview.md for complete architecture details.
 */

#pragma once

#include <string>
#include <memory>
#include <list>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <limits>
#include <functional>

#include <rpc/internal/error_codes.h>
#include <rpc/internal/assert.h>
#include <rpc/internal/types.h>
#include <rpc/internal/version.h>
#include <rpc/internal/marshaller.h>
#include <rpc/internal/remote_pointer.h>
#include <rpc/internal/coroutine_support.h>
#include <rpc/internal/service_proxy.h>

#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/i_telemetry_service.h>
#endif

#ifdef CANOPY_BUILD_COROUTINE
#include <coro/io_scheduler.hpp>
#endif

namespace rpc
{
    class i_interface_stub;
    class object_stub;
    class service;
    class child_service;
    class service_proxy;
    class casting_interface;
    class transport;
    class transport_to_parent;

    const object dummy_object_id = {std::numeric_limits<uint64_t>::max()};

    /**
     * @brief Callback interface for object lifecycle notifications
     *
     * Services can register event listeners to receive notifications when
     * objects are released or zone events occur.
     */
    class service_event
    {
    public:
        virtual ~service_event() = default;

        /**
         * @brief Called when an object is released in a remote zone
         * @param object_id The ID of the released object
         * @param destination The zone where the object was released
         */
        virtual CORO_TASK(void) on_object_released(object object_id, destination_zone destination) = 0;
    };

    /**
     * @brief Core zone manager responsible for all object lifetimes and RPC communications
     *
     * The service class is the central component of a Canopy RPC zone. It manages:
     * - Object lifetime tracking via stubs (server-side wrappers)
     * - Remote zone connections via service_proxies
     * - Transport registration and routing
     * - Reference counting across zone boundaries
     * - Marshalling and unmarshalling of RPC calls
     *
     * Zone Lifecycle Management:
     * - Services are kept alive by local objects (stubs) living in the zone
     * - Child services hold strong references to parent transports
     * - Transports are owned by service_proxies and passthroughs
     * - Services hold only weak references to transports (registry for lookup)
     *
     * Thread Safety:
     * - All public methods are thread-safe unless documented otherwise
     * - Stub registration uses stub_control_ mutex
     * - Transport registration uses service_proxy_control_ mutex
     * - get_current_service() returns thread-local service pointer
     *
     * Ownership Model:
     * - Services own stubs (via shared_ptr in stubs_ map)
     * - Stubs hold strong reference to service (keeps service alive)
     * - Service proxies own transports (strong member_ptr)
     * - Services hold weak references to proxies and transports
     *
     * Hidden Service Principle:
     * Each object only interacts with the current service via get_current_service().
     * Objects don't directly reference their owning service, maintaining clean
     * separation between object lifetime and zone lifetime.
     *
     * See documents/architecture/03-services.md for detailed service lifecycle.
     */
    class service : public i_marshaller, public std::enable_shared_from_this<rpc::service>
    {
    protected:
        static std::atomic<uint64_t> zone_id_generator_;

        zone zone_id_ = {0};
        std::string name_;

        mutable std::atomic<uint64_t> object_id_generator_ = 0;
        std::atomic<encoding> default_encoding_ = CANOPY_DEFAULT_ENCODING;

        // map object_id's to stubs_
        mutable std::mutex stub_control_;
        std::unordered_map<object, std::weak_ptr<object_stub>> stubs_;

        // factory
        std::unordered_map<rpc::interface_ordinal,
            std::shared_ptr<std::function<std::shared_ptr<rpc::i_interface_stub>(const std::shared_ptr<rpc::i_interface_stub>&)>>>
            stub_factories_;

        // map wrapped objects pointers to stubs_
        std::unordered_map<void*, std::weak_ptr<object_stub>> wrapped_object_to_stub_;

        mutable std::mutex service_events_control_;
        std::set<std::weak_ptr<service_event>, std::owner_less<std::weak_ptr<service_event>>> service_events_;

#ifdef CANOPY_BUILD_COROUTINE
        std::shared_ptr<coro::io_scheduler> io_scheduler_;
#endif

        std::shared_ptr<rpc::event> on_shutdown_;

        mutable std::mutex service_proxy_control_;
        // owned by service proxies
        std::unordered_map<destination_zone, std::weak_ptr<service_proxy>> service_proxies_;

        // transports owned by:
        // service proxies
        // pass through objects
        // child services to parent transports
        std::unordered_map<destination_zone, std::weak_ptr<transport>> transports_;

        void inner_add_transport(destination_zone adjacent_zone_id, const std::shared_ptr<transport>& transport_ptr);
        void inner_remove_transport(destination_zone destination_zone_id);
        std::shared_ptr<rpc::transport> inner_get_transport(destination_zone destination_zone_id) const;

    protected:
        struct child_service_tag
        {
        };

        friend transport;

        // Friend declarations for binding template functions
        template<class T>
        friend CORO_TASK(int) stub_bind_out_param(
            const std::shared_ptr<rpc::service>&, uint64_t, caller_zone, const shared_ptr<T>&, interface_descriptor&);

        template<class T>
        friend CORO_TASK(int) stub_bind_in_param(
            uint64_t, const std::shared_ptr<rpc::service>&, caller_zone, const interface_descriptor&, shared_ptr<T>&);

    public:
#ifdef CANOPY_BUILD_COROUTINE
        explicit service(const char* name, zone zone_id, const std::shared_ptr<coro::io_scheduler>& scheduler);
        explicit service(
            const char* name, zone zone_id, const std::shared_ptr<coro::io_scheduler>& scheduler, child_service_tag);
#else
        explicit service(const char* name, zone zone_id);
        explicit service(const char* name, zone zone_id, child_service_tag);
#endif
        /**
         * @brief Service destructor - ensures clean zone shutdown
         *
         * By the time the service destructor is called:
         * - All transports MUST be disconnected (status set to DISCONNECTED)
         * - All transports MUST be unregistered from the transports_ map
         * - All service proxies must be released (enforced in destructor)
         * - All stubs should be released (checked via check_is_empty())
         *
         * Exception for child_service:
         * The parent_transport is intentionally kept alive DURING the child_service
         * destructor to execute the safe disconnection protocol. The child_service
         * destructor calls parent_transport->set_status(DISCONNECTED), which triggers
         * the circular reference cleanup between parent and child transports.
         *
         * See documents/architecture/03-services.md for service lifecycle details.
         * See documents/transports/hierarchical.md for hierarchical transport pattern.
         */
        virtual ~service();

        /**
         * @brief Generate a globally unique zone identifier
         * @return A new zone ID that is unique across all zones in the system
         *
         * Thread-Safety: Safe to call from multiple threads
         */
        static zone generate_new_zone_id();

#ifdef CANOPY_BUILD_COROUTINE
        template<typename Callable> auto schedule(Callable&& callable)
        {
            // Forwards the lambda (or any other callable) to the real scheduler
            return io_scheduler_->schedule(std::forward<Callable>(callable));
        }

        bool spawn(coro::task<void>&& callable)
        {
            // Forwards the lambda (or any other callable) to the real scheduler
            return io_scheduler_->spawn(std::forward<coro::task<void>>(callable));
        }
        auto get_scheduler() const { return io_scheduler_; }

        void set_shutdown_event(const std::shared_ptr<rpc::event>& e) { on_shutdown_ = e; }
#endif

        /**
         * @brief Get the current service for this thread
         * @return Pointer to the service currently processing RPC calls on this thread
         *
         * The current service is thread-local and set automatically during RPC call
         * processing. This allows objects to access their owning service without
         * storing explicit references (Hidden Service Principle).
         *
         * Thread-Safety: Returns thread-local value, safe to call from any thread
         *
         * Usage: Only call this function when servicing an RPC call. The value is
         * only guaranteed to be valid during stub method execution.
         */
        static service* get_current_service();

        /**
         * @brief Set the current service for this thread
         * @param svc The service to set as current for this thread
         *
         * This is called automatically by the RPC framework during call processing.
         * User code should rarely need to call this directly.
         *
         * Thread-Safety: Sets thread-local value, safe to call from any thread
         */
        static void set_current_service(service* svc);

        /**
         * @brief Generate a new object ID unique within this zone
         * @return A new object identifier
         *
         * Thread-Safety: Safe to call from multiple threads (uses atomic increment)
         */
        object generate_new_object_id() const;

        std::string get_name() const { return name_; }
        zone get_zone_id() const { return zone_id_; }

        /**
         * @brief Get the default encoding format for this service
         * @return The encoding format used for new service proxies
         *
         * This encoding is used when creating new service_proxy instances.
         * Thread-Safety: Safe to call from multiple threads (atomic access)
         */
        encoding get_default_encoding() const { return default_encoding_.load(); }

        /**
         * @brief Set the default encoding format for this service
         * @param enc The encoding format to use for new service proxies
         *
         * Changes the default encoding for future service_proxy creations.
         * Does not affect existing proxies.
         * Thread-Safety: Safe to call from multiple threads (atomic access)
         */
        void set_default_encoding(encoding enc) { default_encoding_.store(enc); }

        /**
         * @brief Check if the zone has no active objects
         * @return true if all stubs have been released, false otherwise
         *
         * This is used during service destruction to verify clean shutdown.
         * An assertion will fire if objects remain when the service destructs.
         */
        virtual bool check_is_empty() const;

        /////////////////////////////////
        // NOTIFICATION LOGIC
        /////////////////////////////////

        /**
         * @brief Register a callback for object lifecycle events
         * @param event Weak pointer to service_event implementation
         *
         * Registered events will be notified when objects are released.
         * Uses weak_ptr to avoid keeping event listeners alive.
         *
         * Thread-Safety: Protected by service_events_control_ mutex
         */
        void add_service_event(const std::weak_ptr<service_event>& event);

        /**
         * @brief Unregister an object lifecycle event callback
         * @param event Weak pointer to the service_event to remove
         *
         * Thread-Safety: Protected by service_events_control_ mutex
         */
        void remove_service_event(const std::weak_ptr<service_event>& event);

        /**
         * @brief Notify all registered event listeners that an object was released
         * @param object_id The ID of the released object
         * @param destination The zone where the release occurred
         *
         * Thread-Safety: Protected by service_events_control_ mutex
         */
        CORO_TASK(void) notify_object_gone_event(object object_id, destination_zone destination);

        /**
         * @brief Get the object ID for a local interface pointer
         * @param ptr Shared pointer to a local object (must be local, not remote)
         * @return The object ID associated with this pointer
         *
         * Parameter passed by value to ensure the object stays alive during lookup.
         * This implements an implicit lock on the lifetime of ptr.
         *
         * Thread-Safety: Protected by stub_control_ mutex
         */
        object get_object_id(const shared_ptr<casting_interface>& ptr) const;

        /**
         * @brief Connect to a remote zone via a transport
         * @tparam in_param_type Type of interface to send to remote zone
         * @tparam out_param_type Type of interface to receive from remote zone
         * @param name Descriptive name for the connection
         * @param child_transport Transport connecting to the remote zone
         * @param input_interface Interface to marshal and send to remote zone
         * @param output_interface[out] Interface received from remote zone
         * @return error::OK() on success, error code on failure
         *
         * This method:
         * 1. Marshals the input_interface (if provided) and creates a stub
         * 2. Calls connect() on the transport
         * 3. Creates a service_proxy for the remote zone
         * 4. Demarshals the output_interface (if provided)
         *
         * Used for SPSC, TCP, and other non-hierarchical transports.
         *
         * Thread-Safety: Protected by internal mutexes
         */
        template<class in_param_type, class out_param_type>
        CORO_TASK(int)
        connect_to_zone(const char* name,
            std::shared_ptr<transport> child_transport,
            const rpc::shared_ptr<in_param_type>& input_interface,
            rpc::shared_ptr<out_param_type>& output_interface);

        /**
         * @brief Attach a remote zone during peer-to-peer connection
         * @tparam PARENT_INTERFACE Type of interface received from peer
         * @tparam CHILD_INTERFACE Type of interface to send to peer
         * @param name Descriptive name for the connection
         * @param peer_transport Transport to the peer zone
         * @param input_descr Descriptor of interface received from peer
         * @param output_descr[out] Descriptor of interface to send to peer
         * @param fn Callback to create local interface after demarshalling peer interface
         * @return error::OK() on success, error code on failure
         *
         * This is the server-side counterpart to connect_to_zone(). It:
         * 1. Demarshals the parent interface from input_descr
         * 2. Creates service_proxy for the peer zone
         * 3. Calls the user-provided function to create the child interface
         * 4. Marshals the child interface into output_descr
         *
         * Called by the remote peer during connection establishment.
         *
         * Thread-Safety: Protected by internal mutexes
         */
        template<class PARENT_INTERFACE, class CHILD_INTERFACE>
        CORO_TASK(int)
        attach_remote_zone(const char* name,
            std::shared_ptr<transport> peer_transport,
            rpc::interface_descriptor input_descr,
            rpc::interface_descriptor& output_descr,
            std::function<CORO_TASK(int)(const rpc::shared_ptr<PARENT_INTERFACE>&,
                rpc::shared_ptr<CHILD_INTERFACE>&,
                const std::shared_ptr<rpc::service>&)> fn);

        // protected:
        /////////////////////////////////
        // i_marshaller INTERFACE IMPLEMENTATION
        /////////////////////////////////
        // These methods implement the i_marshaller interface, routing RPC operations
        // to the appropriate transport. They lookup the destination zone's service_proxy
        // and delegate to the virtual outbound_* methods for extensibility.

        /**
         * @brief Send an RPC call with return value
         * @param protocol_version RPC protocol version
         * @param encoding Serialization format (yas_binary, json, etc)
         * @param tag Operation tag for tracing
         * @param caller_zone_id Zone making the call
         * @param destination_zone_id Target zone
         * @param object_id Target object ID
         * @param interface_id Target interface ordinal
         * @param method_id Method to invoke
         * @param in_data Serialized input parameters
         * @param out_buf_[out] Buffer for serialized return value
         * @param in_back_channel Input back-channel data for reference counting
         * @param out_back_channel[out] Output back-channel data for reference counting
         * @return error::OK() on success, error code on failure
         *
         * Looks up transport via service_proxy and delegates to outbound_send().
         *
         * Thread-Safety: Protected by service_proxy_control_ mutex for transport lookup
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
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;
        /**
         * @brief Post a one-way RPC call (fire-and-forget)
         * @param protocol_version RPC protocol version
         * @param encoding Serialization format
         * @param tag Operation tag for tracing
         * @param caller_zone_id Zone making the call
         * @param destination_zone_id Target zone
         * @param object_id Target object ID
         * @param interface_id Target interface ordinal
         * @param method_id Method to invoke
         * @param in_data Serialized input parameters
         * @param in_back_channel Input back-channel data for reference counting
         *
         * Unlike send(), post() does not wait for a return value.
         *
         * Thread-Safety: Protected by service_proxy_control_ mutex for transport lookup
         */
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
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;
        CORO_TASK(int)
        try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;
        CORO_TASK(int)
        add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            known_direction_zone known_direction_zone_id,
            add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;
        CORO_TASK(int)
        release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        // New methods from i_marshaller interface
        CORO_TASK(void)
        object_released(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(void)
        transport_down(uint64_t protocol_version,
            destination_zone destination_zone_id,
            caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        ///////////////////////////////////////////////////////////////////////////////
        // OUTBOUND COMMUNICATION FUNCTIONS - Allow derived classes to add functionality
        ///////////////////////////////////////////////////////////////////////////////
        // These functions provide a way for derived classes to intercept and add extra functionality
        // to outbound calls, such as processing back_channel data. This allows derived versions of
        // the service class to add extra functionality to the overridden version, such as add or
        // process back_channel data.
        ///////////////////////////////////////////////////////////////////////////////

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
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel,
            const std::shared_ptr<transport>& transport);

        virtual CORO_TASK(void) outbound_post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            const std::shared_ptr<transport>& transport);

        virtual CORO_TASK(int) outbound_try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            object object_id,
            interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel,
            const std::shared_ptr<transport>& transport);

        virtual CORO_TASK(int) outbound_add_ref(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            known_direction_zone known_direction_zone_id,
            add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel,
            const std::shared_ptr<transport>& transport);

        virtual CORO_TASK(int) outbound_release(uint64_t protocol_version,
            destination_zone destination_zone_id,
            object object_id,
            caller_zone caller_zone_id,
            release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel,
            const std::shared_ptr<transport>& transport);

    public:
        /////////////////////////////////
        // STUB REGISTRATION AND FACTORY LOGIC
        /////////////////////////////////

        /**
         * @brief Register a factory for creating interface stubs
         * @param id_getter Function to get interface ordinal for a protocol version
         * @param factory Function to create stub from another stub (for casting)
         *
         * This registers a factory that can create stubs for a specific interface type.
         * Used during stub creation and interface casting operations.
         *
         * IMPORTANT: This function is NOT thread-safe. Call it during service initialization
         * before the service is used for normal RPC operations.
         */
        void add_interface_stub_factory(std::function<interface_ordinal(uint8_t)> id_getter,
            std::shared_ptr<std::function<std::shared_ptr<rpc::i_interface_stub>(const std::shared_ptr<rpc::i_interface_stub>&)>>
                factory);

        template<class T>
        std::function<std::shared_ptr<rpc::i_interface_stub>(const std::shared_ptr<object_stub>& stub)>
        get_interface_stub_factory(const shared_ptr<T>& iface);

        int create_interface_stub(rpc::interface_ordinal interface_id,
            std::function<interface_ordinal(uint8_t)> original_interface_id,
            const std::shared_ptr<rpc::i_interface_stub>& original,
            std::shared_ptr<rpc::i_interface_stub>& new_stub);

        uint64_t release_local_stub(
            const std::shared_ptr<object_stub>& stub, bool is_optimistic, caller_zone caller_zone_id);

        /**
         * @brief Register a transport to an adjacent zone
         * @param adjacent_zone_id The zone ID this transport connects to
         * @param transport_ptr The transport to register
         *
         * Transports are owned by service_proxies and passthroughs. The service
         * maintains only weak references for lookup purposes.
         *
         * Thread-Safety: Protected by service_proxy_control_ mutex
         */
        void add_transport(destination_zone adjacent_zone_id, const std::shared_ptr<transport>& transport_ptr);

        /**
         * @brief Unregister a transport to an adjacent zone
         * @param adjacent_zone_id The zone ID of the transport to remove
         *
         * Thread-Safety: Protected by service_proxy_control_ mutex
         */
        void remove_transport(destination_zone adjacent_zone_id);

        /**
         * @brief Get transport to a destination zone (may route through intermediaries)
         * @param destination_zone_id The target zone
         * @return Shared pointer to transport, or nullptr if not found
         *
         * Looks up transport in the registry. Passthroughs can route to non-adjacent zones.
         *
         * Thread-Safety: Protected by service_proxy_control_ mutex
         */
        std::shared_ptr<rpc::transport> get_transport(destination_zone destination_zone_id) const;

        CORO_TASK(void)
        notify_transport_down(const std::shared_ptr<transport>& transport, destination_zone remote_zone);

    protected:
        virtual void add_zone_proxy(const std::shared_ptr<rpc::service_proxy>& zone);

    private:
        virtual std::shared_ptr<rpc::service_proxy> get_zone_proxy(
            caller_zone caller_zone_id, // when you know who is calling you
            destination_zone destination_zone_id,
            bool& new_proxy_added);
        virtual void remove_zone_proxy(destination_zone destination_zone_id);

        /////////////////////////////////
        // BINDING LOGIC
        /////////////////////////////////

        std::weak_ptr<object_stub> get_object(object object_id) const;

        template<class T>
        CORO_TASK(int)
        bind_in_proxy(uint64_t protocol_version,
            const shared_ptr<T>& iface,
            std::shared_ptr<rpc::object_stub>& stub,
            caller_zone caller_zone_id,
            interface_descriptor& descriptor);

        CORO_TASK(int)
        get_descriptor_from_interface_stub(caller_zone caller_zone_id,
            rpc::casting_interface* pointer,
            std::function<std::shared_ptr<rpc::i_interface_stub>(std::shared_ptr<object_stub>)> fn,
            std::shared_ptr<object_stub>& stub,
            interface_descriptor& descriptor);

        // Specialized version for binding out parameters (used by stub_bind_out_param)
        CORO_TASK(int)
        add_ref_local_or_remote_return_descriptor(uint64_t protocol_version,
            caller_zone caller_zone_id,
            rpc::casting_interface* pointer,
            std::function<std::shared_ptr<rpc::i_interface_stub>(std::shared_ptr<object_stub>)> fn,
            std::shared_ptr<object_stub>& stub,
            interface_descriptor& descriptor);

        /////////////////////////////////
        // PRIVATE FUNCTIONS
        /////////////////////////////////

        rpc::shared_ptr<casting_interface> get_castable_interface(object object_id, interface_ordinal interface_id);

        void inner_add_zone_proxy(const std::shared_ptr<rpc::service_proxy>& service_proxy);
        void cleanup_service_proxy(const std::shared_ptr<rpc::service_proxy>& other_zone);

        CORO_TASK(void)
        clean_up_on_failed_connection(const std::shared_ptr<rpc::service_proxy>& destination_zone,
            rpc::shared_ptr<rpc::casting_interface> input_interface);

        /////////////////////////////////
        // FRIENDS FUNCTIONS
        /////////////////////////////////

        friend service_proxy;

        template<class T>
        friend CORO_TASK(int) rpc::proxy_bind_out_param(const std::shared_ptr<rpc::service_proxy>& sp,
            const rpc::interface_descriptor& encap,
            rpc::shared_ptr<T>& val);

        template<class T>
        friend CORO_TASK(int) rpc::stub_bind_in_param(uint64_t protocol_version,
            const std::shared_ptr<rpc::service>& serv,
            rpc::caller_zone caller_zone_id,
            const rpc::interface_descriptor& encap,
            rpc::shared_ptr<T>& iface);

        template<class T>
        friend CORO_TASK(int) rpc::create_interface_stub(rpc::service& serv,
            const rpc::shared_ptr<T>& iface,
            caller_zone caller_zone_id,
            rpc::interface_descriptor& descriptor);

        template<class T>
        friend CORO_TASK(int) rpc::stub_bind_out_param(const std::shared_ptr<rpc::service>& zone,
            uint64_t protocol_version,
            rpc::caller_zone caller_zone_id,
            const rpc::shared_ptr<T>& iface,
            rpc::interface_descriptor& descriptor);

        template<class T>
        friend CORO_TASK(int) rpc::proxy_bind_in_param(std::shared_ptr<rpc::object_proxy> object_p,
            uint64_t protocol_version,
            const rpc::shared_ptr<T>& iface,
            std::shared_ptr<rpc::object_stub>& stub,
            rpc::interface_descriptor& descriptor);
    };

    /**
     * @brief Service for child zones in hierarchical zone relationships
     *
     * Child services represent zones created by parent zones in hierarchical
     * transports (local, SGX enclave, DLL). They differ from regular services by:
     * - Holding a strong reference to the parent transport
     * - Maintaining the parent zone's lifetime while the child exists
     *
     * Hierarchical Transport Circular Dependency Pattern:
     * Parent Zone: child_transport → member_ptr<parent_transport> (to child)
     * Child Zone:  parent_transport → member_ptr<child_transport> (to parent)
     *
     * Stack-Based Lifetime Protection:
     * When calls cross zone boundaries, stack-based shared_ptr protects transport
     * lifetime. This prevents use-after-free during active calls even if circular
     * references are broken.
     *
     * Safe Disconnection Protocol:
     * 1. child_service destructor calls parent_transport->set_status(DISCONNECTED)
     * 2. parent_transport propagates status to parent zone
     * 3. child_transport::on_child_disconnected() breaks its circular reference
     * 4. Both transports break their references, resolving the circular dependency
     * 5. Stack protection ensures no use-after-free during active calls
     *
     * See documents/architecture/07-zone-hierarchies.md and
     * documents/transports/hierarchical.md for complete details.
     */
    class child_service : public service
    {
        mutable std::mutex parent_protect;
        // CRITICAL: Strong reference to parent transport - keeps parent zone alive
        // This ensures parent remains reachable while child exists
        std::shared_ptr<transport> parent_transport_;
        destination_zone parent_zone_id_;

    public:
        /**
         * @brief Set the parent transport (MUST be called during child zone creation)
         * @param parent_transport Transport to the parent zone
         *
         * This establishes the child's strong reference to the parent, completing
         * the circular dependency pattern for hierarchical transports.
         *
         * Thread-Safety: Protected by parent_protect mutex
         */
        void set_parent_transport(const std::shared_ptr<transport>& parent_transport)
        {
            std::lock_guard lock(parent_protect);
            parent_transport_ = parent_transport;
        }

        /**
         * @brief Get the parent transport
         * @return Shared pointer to parent transport
         *
         * Thread-Safety: Protected by parent_protect mutex
         */
        std::shared_ptr<transport> get_parent_transport() const
        {
            std::lock_guard lock(parent_protect);
            return parent_transport_;
        }

        /**
         * @brief Get the parent zone ID
         * @return Zone ID of the parent zone
         *
         * Thread-Safety: Safe to call from multiple threads (const member)
         */
        destination_zone get_parent_zone_id() const { return parent_zone_id_; }

    public:
#ifdef CANOPY_BUILD_COROUTINE
        explicit child_service(const char* name,
            zone zone_id,
            destination_zone parent_zone_id,
            const std::shared_ptr<coro::io_scheduler>& io_scheduler)
            : service(name, zone_id, io_scheduler, child_service_tag{})
            , parent_zone_id_(parent_zone_id)
        {
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_service_creation(name, zone_id, parent_zone_id);
#endif
        }
#else
        explicit child_service(const char* name, zone zone_id, destination_zone parent_zone_id)
            : service(name, zone_id, child_service_tag{})
            , parent_zone_id_(parent_zone_id)
        {
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_service_creation(name, zone_id, parent_zone_id);
#endif
        }
#endif

        virtual ~child_service();

        template<class PARENT_INTERFACE, class CHILD_INTERFACE>
        static CORO_TASK(int) create_child_zone(const char* name,
            std::shared_ptr<transport> parent_transport,
            rpc::interface_descriptor input_descr,
            rpc::interface_descriptor& output_descr,
            std::function<CORO_TASK(int)(const rpc::shared_ptr<PARENT_INTERFACE>&,
                rpc::shared_ptr<CHILD_INTERFACE>&,
                const std::shared_ptr<rpc::child_service>&)>&& fn
#ifdef CANOPY_BUILD_COROUTINE
            ,
            const std::shared_ptr<coro::io_scheduler>& io_scheduler
#endif
        )
        {
            auto zone_id = parent_transport->get_zone_id();
            auto adjacent_zone_id = parent_transport->get_adjacent_zone_id();

            auto child_svc = std::shared_ptr<rpc::child_service>(new rpc::child_service(name,
                zone_id,
                adjacent_zone_id.as_destination()
#ifdef CANOPY_BUILD_COROUTINE
                    ,
                io_scheduler
#endif
                ));

            // Link the child to the parent via transport
            parent_transport->set_service(child_svc);

            // CRITICAL: Child service must keep parent transport alive
            // This ensures parent zone remains reachable while child exists
            child_svc->set_parent_transport(parent_transport);

            child_svc->add_transport(input_descr.destination_zone_id, parent_transport);

            rpc::shared_ptr<PARENT_INTERFACE> parent_ptr;
            if (input_descr.object_id != 0)
            {
                auto parent_service_proxy
                    = rpc::service_proxy::create("parent", child_svc, parent_transport, input_descr.destination_zone_id);

                child_svc->add_zone_proxy(parent_service_proxy);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                    telemetry_service->on_transport_inbound_add_ref(zone_id,
                        adjacent_zone_id,
                        zone_id.as_destination(),
                        adjacent_zone_id.as_caller(),
                        input_descr.object_id,
                        adjacent_zone_id,
                        rpc::add_ref_options::normal);
#endif

                auto err_code = CO_AWAIT rpc::demarshall_interface_proxy(
                    rpc::get_version(), parent_service_proxy, input_descr, parent_ptr);
                if (err_code != rpc::error::OK())
                {
                    CO_RETURN err_code;
                }
            }
            rpc::shared_ptr<CHILD_INTERFACE> child_ptr;
            {
                auto err_code = CO_AWAIT fn(parent_ptr, child_ptr, child_svc);
                if (err_code != rpc::error::OK())
                {
                    CO_RETURN err_code;
                }
            }
            if (child_ptr)
            {
                RPC_ASSERT(
                    child_ptr->is_local()
                    && "we cannot support remote pointers to subordinate zones as it has not been registered yet");
                auto err_code = CO_AWAIT rpc::create_interface_stub(
                    *child_svc, child_ptr, parent_transport->get_adjacent_zone_id().as_caller(), output_descr);

                if (err_code == rpc::error::OK())
                {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                    if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                        telemetry_service->on_transport_outbound_add_ref(zone_id,
                            adjacent_zone_id,
                            zone_id.as_destination(),
                            adjacent_zone_id.as_caller(),
                            output_descr.object_id,
                            zone_id,
                            rpc::add_ref_options::build_caller_route);
#endif
                }
                CO_RETURN err_code;
            }
            CO_RETURN rpc::error::OK();
        };
    };

    template<class in_param_type, class out_param_type>
    CORO_TASK(int)
    service::connect_to_zone(const char* name,
        std::shared_ptr<transport> child_transport,
        const rpc::shared_ptr<in_param_type>& input_interface,
        rpc::shared_ptr<out_param_type>& output_interface)
    {
        // Marshal input interface if provided
        rpc::interface_descriptor input_descr{{0}, {0}};
        // Connect via transport (calls remote zone's entry point)
        rpc::interface_descriptor output_descr{{0}, {0}};

        int err_code = rpc::error::OK();

        bool transport_added = false;

        if (input_interface)
        {
            add_transport(child_transport->get_adjacent_zone_id().as_destination(), child_transport);
            transport_added = true;
            std::shared_ptr<object_stub> stub;
            auto factory = get_interface_stub_factory(input_interface);
            err_code = CO_AWAIT get_descriptor_from_interface_stub(
                child_transport->get_adjacent_zone_id().as_caller(), input_interface.get(), factory, stub, input_descr);

            if (err_code != error::OK())
            {
                remove_transport(child_transport->get_adjacent_zone_id().as_destination());
                CO_RETURN err_code;
            }
        }
        else
        {
            input_descr.destination_zone_id = child_transport->get_zone_id().get_val();
        }
        err_code = CO_AWAIT child_transport->connect(input_descr, output_descr);
        if (err_code != rpc::error::OK())
        {
            if (transport_added)
            {
                // Clean up on failure
                remove_transport(child_transport->get_adjacent_zone_id().as_destination());
            }
            CO_RETURN err_code;
        }

        // Demarshal output interface if provided
        if (output_descr.object_id != 0 && output_descr.destination_zone_id != 0)
        {
            if (!transport_added)
            {
                add_transport(child_transport->get_adjacent_zone_id().as_destination(), child_transport);
            }

            // Create service_proxy for this connection
            auto new_service_proxy = rpc::service_proxy::create(
                name, shared_from_this(), child_transport, child_transport->get_adjacent_zone_id().as_destination());

            // add the proxy to the service
            add_zone_proxy(new_service_proxy);

            err_code = CO_AWAIT rpc::demarshall_interface_proxy(
                rpc::get_version(), new_service_proxy, output_descr, output_interface);
        }

        CO_RETURN err_code;
    }

    // Attach remote zone - for peer-to-peer connections
    // Takes single transport since this is called by the remote peer during connection
    template<class PARENT_INTERFACE, class CHILD_INTERFACE>
    CORO_TASK(int)
    service::attach_remote_zone(const char* name,
        std::shared_ptr<transport> peer_transport,
        rpc::interface_descriptor input_descr,
        rpc::interface_descriptor& output_descr,
        std::function<CORO_TASK(int)(
            const rpc::shared_ptr<PARENT_INTERFACE>&, rpc::shared_ptr<CHILD_INTERFACE>&, const std::shared_ptr<rpc::service>&)> fn)
    {
        // Demarshal parent interface if provided
        rpc::shared_ptr<PARENT_INTERFACE> parent_ptr;
        if (input_descr.object_id != 0)
        {
            // Create service_proxy for peer connection
            auto peer_service_proxy
                = rpc::service_proxy::create(name, shared_from_this(), peer_transport, input_descr.destination_zone_id);
            add_zone_proxy(peer_service_proxy);

            auto err_code = CO_AWAIT rpc::demarshall_interface_proxy(
                rpc::get_version(), peer_service_proxy, input_descr, parent_ptr);
            if (err_code != rpc::error::OK())
            {
                CO_RETURN err_code;
            }
        }
        else
        {
            add_transport(peer_transport->get_adjacent_zone_id().as_destination(), peer_transport);
        }

        auto err_code = CO_AWAIT peer_transport->accept();
        if (err_code != rpc::error::OK())
        {
            if (input_descr != interface_descriptor())
            {
                // perhaps we should stop the transport first?
                remove_transport(peer_transport->get_adjacent_zone_id().as_destination());
            }
            CO_RETURN err_code;
        }

        // Call local entry point to create child interface
        rpc::shared_ptr<CHILD_INTERFACE> child_ptr;
        err_code = CO_AWAIT fn(parent_ptr, child_ptr, shared_from_this());
        if (err_code != rpc::error::OK())
        {
            if (input_descr != interface_descriptor())
            {
                // perhaps we should stop the transport first?
                remove_transport(peer_transport->get_adjacent_zone_id().as_destination());
            }
            CO_RETURN err_code;
        }

        // Marshal child interface to return to peer
        if (child_ptr)
        {
            RPC_ASSERT(child_ptr->is_local() && "Cannot support remote pointers from subordinate zones");
            err_code = CO_AWAIT rpc::create_interface_stub(
                *this, child_ptr, peer_transport->get_adjacent_zone_id().as_caller(), output_descr);
            if (err_code != rpc::error::OK())
            {
                if (input_descr != interface_descriptor())
                {
                    // perhaps we should stop the transport first?
                    remove_transport(peer_transport->get_adjacent_zone_id().as_destination());
                }
                CO_RETURN err_code;
            }
        }

        CO_RETURN rpc::error::OK();
    }

    // protect the current service local pointer
    struct current_service_tracker
    {
        service* old_service_ = nullptr;
        current_service_tracker(service* current_service)
        {
            old_service_ = service::get_current_service();
            service::set_current_service(current_service);
        }
        ~current_service_tracker() { service::set_current_service(old_service_); }
    };
}
