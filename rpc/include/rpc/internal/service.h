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
#include <type_traits>

#include <rpc/internal/error_codes.h>
#include <rpc/internal/assert.h>
#include <rpc/internal/types.h>
#include <rpc/internal/version.h>
#include <rpc/internal/marshaller.h>
#include <rpc/internal/zone_id_allocator.h>
#include <rpc/internal/pass_through.h>
#include <rpc/internal/bindings_fwd.h>
#include <rpc/internal/remote_pointer.h>
#include <rpc/internal/coroutine_support.h>
#include <rpc/internal/service_proxy.h>
#include <rpc/internal/stub.h>

#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/i_telemetry_service.h>
#endif

#ifdef CANOPY_BUILD_COROUTINE
#include <coro/scheduler.hpp>
#endif

namespace rpc
{
    class object_stub;
    class service;
    class child_service;
    class service_proxy;
    class casting_interface;
    class transport;

    // The callback type all transports use when a remote zone initiates a connection.
    // connection_settings and remote_object are internal protocol details hidden
    // from user code — use make_new_zone_connection_handler<Remote, Local> to create one.
    struct connection_handler_result
    {
        int error_code;
        rpc::remote_object output_descriptor;
    };

    using connection_handler = std::function<CORO_TASK(connection_handler_result)(
        rpc::connection_settings, std::shared_ptr<rpc::service>, std::shared_ptr<rpc::transport>)>;

    // A factory callable that receives a name, service, and connection handler,
    // and returns a configured transport. Different transport implementations
    // provide factory helpers (e.g. rpc::stream_transport::transport_factory).
    // The factory is a coroutine to support transports that need async setup
    // (e.g. allocating a server-assigned zone ID before constructing the transport).
    using transport_factory
        = std::function<CORO_TASK(std::shared_ptr<transport>)(std::string, std::shared_ptr<service>, connection_handler)>;

    const object dummy_object_id = {std::numeric_limits<uint64_t>::max()};

    struct service_config
    {
        zone initial_zone{DEFAULT_PREFIX};
    };

    struct remote_object_result
    {
        int error_code;
        rpc::remote_object descriptor;

        remote_object_result() = default;
        remote_object_result(int error_code, rpc::remote_object descriptor)
            : error_code(error_code)
            , descriptor(descriptor)
        {
        }
    };

    template<class T> struct service_connect_result
    {
        int error_code;
        rpc::shared_ptr<T> output_interface;

        service_connect_result() = default;
        service_connect_result(int error_code, rpc::shared_ptr<T> output_interface)
            : error_code(error_code)
            , output_interface(std::move(output_interface))
        {
        }
    };

    /**
     * @brief Callback interface for object lifecycle notifications
     *
     * Services can register event listeners to receive notifications when
     * objects are released or zone events occur.
     */
    class service_event : public std::enable_shared_from_this<service_event>
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
    private:
        const zone zone_id_;
        std::string name_;

        mutable std::atomic<uint64_t> object_id_generator_ = 0;
        std::atomic<encoding> default_encoding_ = CANOPY_DEFAULT_ENCODING;

        // map object_id's to stubs_
        mutable std::mutex stub_control_;
        std::unordered_map<object, std::weak_ptr<object_stub>> stubs_;

        mutable std::mutex service_events_control_;
        std::set<std::weak_ptr<service_event>, std::owner_less<std::weak_ptr<service_event>>> service_events_;

#ifdef CANOPY_BUILD_COROUTINE
        std::shared_ptr<coro::scheduler> io_scheduler_;
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

        // get a transport or passthrough through which connect between callers and destination
        // if a passthrough is needed, it will be created and added to the transports_ map
        int get_or_create_link_between_source_and_destination(caller_zone caller_zone_id,
            destination_zone destination_zone_id,
            const std::shared_ptr<rpc::transport>& destination_transport,
            std::shared_ptr<rpc::i_marshaller>& marshaller);

        template<class T, template<class> class PtrType>
        CORO_TASK(remote_object_result)
        remote_add_ref(uint64_t protocol_version, caller_zone caller_zone_id, PtrType<T> iface);

    protected:
        struct child_service_tag
        {
        };

        friend transport;

#ifdef CANOPY_BUILD_COROUTINE
        explicit service(const char* name, zone zone_id, const std::shared_ptr<coro::scheduler>& scheduler);
        explicit service(
            const char* name, zone zone_id, const std::shared_ptr<coro::scheduler>& scheduler, child_service_tag);
#else
        explicit service(const char* name, zone zone_id);
        explicit service(const char* name, zone zone_id, child_service_tag);
#endif

    public:
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
        ~service() override;

#ifdef CANOPY_BUILD_COROUTINE
        template<typename Callable> auto schedule(Callable&& callable)
        {
            // Forwards the lambda (or any other callable) to the real scheduler
            return io_scheduler_->schedule(std::forward<Callable>(callable));
        }

        coro::scheduler::schedule_operation schedule() { return io_scheduler_->schedule(); }

        bool spawn(coro::task<void>&& callable)
        {
            // Forwards the lambda (or any other callable) to the real scheduler
            return io_scheduler_->spawn_detached(std::forward<coro::task<void>>(callable));
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
         * Used for SPSC, TCP, and other transports.
         *
         * Thread-Safety: Protected by internal mutexes
         */
        template<class in_param_type, class out_param_type>
        CORO_TASK(service_connect_result<out_param_type>)
        connect_to_zone(
            const char* name, std::shared_ptr<transport> child_transport, rpc::shared_ptr<in_param_type> input_interface);

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
        CORO_TASK(remote_object_result)
        attach_remote_zone(const char* name,
            std::shared_ptr<transport> peer_transport,
            rpc::connection_settings input_descr,
            std::function<CORO_TASK(service_connect_result<CHILD_INTERFACE>)(
                rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::service>)> fn);

        template<class Remote, class Local>
        CORO_TASK(std::shared_ptr<transport>)
        make_acceptor(std::string name,
            transport_factory factory,
            std::function<CORO_TASK(service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<service>)> fn);

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
        CORO_TASK(send_result) send(send_params params) override;
        /**
         * @brief Post a one-way RPC call (fire-and-forget)
         * @param params Bundled post parameters
         *
         * Unlike send(), post() does not wait for a return value.
         *
         * Thread-Safety: Protected by service_proxy_control_ mutex for transport lookup
         */
        CORO_TASK(void) post(post_params params) override;
        CORO_TASK(standard_result) try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) release(release_params params) override;
        CORO_TASK(void) object_released(object_released_params params) override;
        CORO_TASK(void) transport_down(transport_down_params params) override;
        CORO_TASK(new_zone_id_result) get_new_zone_id(get_new_zone_id_params params) override = 0;

        ///////////////////////////////////////////////////////////////////////////////
        // OUTBOUND COMMUNICATION FUNCTIONS - Allow derived classes to add functionality
        ///////////////////////////////////////////////////////////////////////////////
        // These functions provide a way for derived classes to intercept and add extra functionality
        // to outbound calls, such as processing back_channel data. This allows derived versions of
        // the service class to add extra functionality to the overridden version, such as add or
        // process back_channel data.
        ///////////////////////////////////////////////////////////////////////////////

        virtual CORO_TASK(send_result) outbound_send(send_params params, std::shared_ptr<transport> transport);
        virtual CORO_TASK(void) outbound_post(post_params params, std::shared_ptr<transport> transport);
        virtual CORO_TASK(standard_result) outbound_try_cast(try_cast_params params, std::shared_ptr<transport> transport);
        virtual CORO_TASK(standard_result) outbound_add_ref(add_ref_params params, std::shared_ptr<transport> transport);
        virtual CORO_TASK(standard_result) outbound_release(release_params params, std::shared_ptr<transport> transport);

    public:
        CORO_TASK(uint64_t)
        release_local_stub(std::shared_ptr<object_stub> stub, bool is_optimistic, caller_zone caller_zone_id);

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

        CORO_TASK(void) notify_transport_down(std::shared_ptr<transport> transport, destination_zone remote_zone);

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

        template<class T, template<class> class PtrType>
        CORO_TASK(remote_object_bind_result)
        bind_in_proxy(uint64_t protocol_version, PtrType<T> iface, caller_zone caller_zone_id);

        template<class T>
        CORO_TASK(remote_object_bind_result)
        bind_in_proxy(uint64_t protocol_version, optimistic_ptr<T> iface, caller_zone caller_zone_id);

        CORO_TASK(remote_object_bind_result)
        get_descriptor_from_interface_stub(
            caller_zone caller_zone_id, rpc::shared_ptr<rpc::casting_interface> iface, bool optimistic);

        // Specialized version for binding out parameters (used by stub_bind_out_param)
        template<class T, template<class> class PtrType>
        CORO_TASK(remote_object_result)
        stub_add_ref(uint64_t protocol_version, caller_zone caller_zone_id, PtrType<T> iface);

        /////////////////////////////////
        // PRIVATE FUNCTIONS
        /////////////////////////////////
        void inner_add_zone_proxy(const std::shared_ptr<rpc::service_proxy>& service_proxy);
        void cleanup_service_proxy(const std::shared_ptr<rpc::service_proxy>& other_zone);

        CORO_TASK(void)
        clean_up_on_failed_connection(std::shared_ptr<rpc::service_proxy> destination_zone,
            rpc::shared_ptr<rpc::casting_interface> input_interface);

        /////////////////////////////////
        // FRIENDS FUNCTIONS
        /////////////////////////////////

        friend service_proxy;

        template<class T, template<class> class PtrType>
        friend CORO_TASK(interface_bind_result<PtrType<T>>) rpc::proxy_bind_out_param(
            std::shared_ptr<rpc::service_proxy> sp, rpc::remote_object encap);

        template<class T, template<class> class PtrType>
        friend CORO_TASK(interface_bind_result<PtrType<T>>) rpc::stub_bind_in_param(uint64_t protocol_version,
            std::shared_ptr<rpc::service> serv,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object encap);

        template<class T>
        friend CORO_TASK(remote_object_bind_result) rpc::create_interface_stub(
            rpc::service* serv, rpc::shared_ptr<T> iface, caller_zone caller_zone_id);

        template<class T, template<class> class PtrType>
        friend CORO_TASK(remote_object_bind_result) rpc::stub_bind_out_param(
            std::shared_ptr<rpc::service> zone, uint64_t protocol_version, rpc::caller_zone caller_zone_id, PtrType<T> iface);

        template<class T, template<class> class PtrType>
        friend CORO_TASK(remote_object_bind_result) rpc::proxy_bind_in_param(
            std::shared_ptr<rpc::object_proxy> object_p, uint64_t protocol_version, PtrType<T> iface);
    };

    /**
     * @brief Root service for zones that own zone ID allocation
     *
     * root_service is the concrete base for all top-level (non-child) zones. It holds
     * the zone_id_allocator and satisfies the pure-virtual get_new_zone_id() requirements left open by the abstract service class.
     *
     * Use root_service wherever a zone is the authoritative owner of its own ID space
     * (i.e. it is NOT a child zone created by a hierarchical transport). Child zones
     * use child_service, which forwards get_new_zone_id() requests up to the root
     * via the parent transport.
     *
     * See documents/architecture/03-services.md for service lifecycle details.
     */
    class root_service : public service
    {
        zone_id_allocator zone_allocator_;

    public:
#ifdef CANOPY_BUILD_COROUTINE
        explicit root_service(const char* name, zone zone_id, const std::shared_ptr<coro::scheduler>& scheduler);
        explicit root_service(
            const char* name, const service_config& config, const std::shared_ptr<coro::scheduler>& scheduler);
#else
        explicit root_service(const char* name, zone zone_id);
        explicit root_service(const char* name, const service_config& config);
#endif

        ~root_service() override = default;

        /**
         * @brief i_marshaller implementation: allocate a new zone ID for the caller
         *
         * Thread-Safety: Safe to call from multiple threads
         */
        CORO_TASK(new_zone_id_result) get_new_zone_id(get_new_zone_id_params params) override;
    };

    /**
     * @brief Helper class to keep a transport alive even if it's not being immediately used, needed for components that
     * expect the transport to be there
     */
    struct transport_keep_alive
    {
        std::shared_ptr<transport> transport_;
        destination_zone zone_id_;

        transport_keep_alive() = default;
        transport_keep_alive(const std::shared_ptr<transport>& transport, destination_zone zone_id)
            : transport_(transport)
            , zone_id_(zone_id)
        {
            transport_->increment_outbound_proxy_count(zone_id);
        }

        transport_keep_alive(const transport_keep_alive&) = delete;
        transport_keep_alive& operator=(const transport_keep_alive&) = delete;

        transport_keep_alive(transport_keep_alive&& other) noexcept
            : transport_(std::move(other.transport_))
            , zone_id_(other.zone_id_)
        {
        }

        transport_keep_alive& operator=(transport_keep_alive&& other) noexcept
        {
            if (this != &other)
            {
                if (transport_)
                    transport_->decrement_outbound_proxy_count(zone_id_);
                transport_ = std::move(other.transport_);
                zone_id_ = other.zone_id_;
            }
            return *this;
        }

        ~transport_keep_alive()
        {
            if (transport_)
                transport_->decrement_outbound_proxy_count(zone_id_);
        }
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
            const std::shared_ptr<coro::scheduler>& io_scheduler)
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

        ~child_service() override;

        // Forwards the request up to the parent zone via the parent transport.
        CORO_TASK(new_zone_id_result) get_new_zone_id(get_new_zone_id_params params) override;

        template<class PARENT_INTERFACE, class CHILD_INTERFACE>
        static CORO_TASK(remote_object_result) create_child_zone(const char* name,
            std::shared_ptr<transport> parent_transport,
            rpc::connection_settings input_descr,
            std::function<CORO_TASK(service_connect_result<CHILD_INTERFACE>)(
                rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::child_service>)> fn
#ifdef CANOPY_BUILD_COROUTINE
            ,
            std::shared_ptr<coro::scheduler> io_scheduler
#endif
        )
        {
            remote_object_result result{rpc::error::OK(), {}};
            if (input_descr.inbound_interface_id != PARENT_INTERFACE::get_id(rpc::get_version()))
            {
                RPC_ERROR("inbound_interface_id does not match");
                result.error_code = rpc::error::INVALID_INTERFACE_ID();
                CO_RETURN result;
            }
            if (input_descr.outbound_interface_id != CHILD_INTERFACE::get_id(rpc::get_version()))
            {
                RPC_ERROR("outbound_interface_id does not match");
                result.error_code = rpc::error::INVALID_INTERFACE_ID();
                CO_RETURN result;
            }
            auto zone_id = parent_transport->get_zone_id();
            auto adjacent_zone_id = parent_transport->get_adjacent_zone_id();

            auto child_svc = std::shared_ptr<rpc::child_service>(new rpc::child_service(name,
                zone_id,
                adjacent_zone_id
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

            child_svc->add_transport(input_descr.remote_object_id.as_zone(), parent_transport);
            transport_keep_alive ka(parent_transport, input_descr.remote_object_id.as_zone());
            transport_keep_alive adjacent_ka;
            if (input_descr.remote_object_id != parent_transport->get_adjacent_zone_id())
            {
                child_svc->add_transport(parent_transport->get_adjacent_zone_id(), parent_transport);
                adjacent_ka = transport_keep_alive(parent_transport, parent_transport->get_adjacent_zone_id());
            }

            rpc::shared_ptr<PARENT_INTERFACE> parent_ptr;
            if (input_descr.get_object_id() != 0)
            {
                auto parent_service_proxy = rpc::service_proxy::create(
                    "parent", child_svc, parent_transport, input_descr.remote_object_id.as_zone());

                child_svc->add_zone_proxy(parent_service_proxy);

                bool new_proxy_added = true;

                auto proxy_result = CO_AWAIT parent_service_proxy->get_or_create_object_proxy(input_descr.get_object_id(),
                    service_proxy::object_proxy_creation_rule::ADD_REF_IF_NEW,
                    new_proxy_added,
                    {parent_transport->get_adjacent_zone_id().get_address()},
                    false);
                if (proxy_result.error_code != error::OK())
                {
                    RPC_ERROR("get_or_create_object_proxy failed");
                    result.error_code = proxy_result.error_code;
                    CO_RETURN result;
                }
                auto op = std::move(proxy_result.object_proxy);
                RPC_ASSERT(op != nullptr);
                if (!op)
                {
                    RPC_ERROR("Object not found - object proxy is null");
                    result.error_code = rpc::error::OBJECT_NOT_FOUND();
                    CO_RETURN result;
                }
                auto query_result = CO_AWAIT op->template query_interface<PARENT_INTERFACE>(false);
                if (query_result.error_code != rpc::error::OK())
                {
                    result.error_code = query_result.error_code;
                    CO_RETURN result;
                }
                parent_ptr = std::move(query_result.iface);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                    telemetry_service->on_transport_inbound_add_ref(zone_id,
                        adjacent_zone_id,
                        zone_id.with_object(input_descr.get_object_id()),
                        adjacent_zone_id,
                        adjacent_zone_id,
                        rpc::add_ref_options::normal);
#endif
            }
            auto child_result = CO_AWAIT fn(parent_ptr, child_svc);
            if (child_result.error_code != rpc::error::OK())
            {
                result.error_code = child_result.error_code;
                CO_RETURN result;
            }
            auto child_ptr = std::move(child_result.output_interface);
            if (child_ptr)
            {
                auto bind_result = CO_AWAIT rpc::stub_bind_out_param(
                    child_svc, rpc::get_version(), parent_transport->get_adjacent_zone_id(), child_ptr);
                result.error_code = bind_result.error_code;
                result.descriptor = bind_result.descriptor;

                if (result.error_code == rpc::error::OK())
                {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                    if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                        telemetry_service->on_transport_outbound_add_ref(zone_id,
                            adjacent_zone_id,
                            zone_id.with_object(result.descriptor.get_object_id()),
                            adjacent_zone_id,
                            zone_id,
                            rpc::add_ref_options::build_caller_route);
#endif
                }
                CO_RETURN result;
            }
            CO_RETURN result;
        };
    };

    template<class in_param_type, class out_param_type>
    CORO_TASK(service_connect_result<out_param_type>)
    service::connect_to_zone(
        const char* name, std::shared_ptr<transport> child_transport, rpc::shared_ptr<in_param_type> input_interface)
    {
        service_connect_result<out_param_type> result{rpc::error::OK(), {}};
        // Marshal input interface if provided
        rpc::connection_settings input_descr;
        // Connect via transport (calls remote zone's entry point)
        rpc::remote_object output_descr;

        int err_code = rpc::error::OK();

        std::shared_ptr<rpc::object_stub> input_stub;
        bool stub_created = false;

        if (input_interface)
        {
            // this is to check that an interface is belonging to another zone and not the operating zone
            if (!input_interface->__rpc_is_local()
                && casting_interface::get_destination_zone(*input_interface) != get_zone_id())
            {
                input_descr.remote_object_id = casting_interface::get_destination_zone(*input_interface)
                                                   .with_object(casting_interface::get_object_id(*input_interface));
            }
            else
            {
                {
                    std::lock_guard g(stub_control_);
                    input_stub = input_interface->__rpc_get_stub(); // get stub after lock
                    if (!input_stub)
                    {
                        auto id = generate_new_object_id();
                        input_stub = std::make_shared<object_stub>(id, shared_from_this(), input_interface);
                        stubs_[id] = input_stub;
                        input_stub->keep_self_alive();
                        input_interface->__rpc_set_stub(input_stub);
                        stub_created = true;
                    }
                }
                if (err_code != error::OK())
                {
                    result.error_code = err_code;
                    CO_RETURN result;
                }
                input_descr.remote_object_id = zone_id_.with_object(input_stub->get_id());
            }
        }
        else
        {
            input_descr.remote_object_id = child_transport->get_zone_id().get_address();
        }
        input_descr.inbound_interface_id = in_param_type::get_id(rpc::get_version());
        input_descr.outbound_interface_id = out_param_type::get_id(rpc::get_version());

        // once the transport is connected the adjacent zone id is known it must add_ref the stub against the transport
        // if the input interface is remote then the addref is not needed.
        err_code = CO_AWAIT child_transport->connect(input_stub, input_descr, output_descr);
        if (err_code != rpc::error::OK())
        {
            if (input_stub)
            {
                CO_AWAIT release_local_stub(input_stub, false, child_transport->get_adjacent_zone_id());

                std::lock_guard g(stub_control_);
                // this is a hard race condition but we need to check if the stub was created and if it is not
                // referenced, there is a microscopic risk that somehow the reference to the stub was shared around in
                // other threads and they have not got around to add_refing it.  The stub will remain valid until their
                // callstacks hold onto the shared_ptr of the object that the stub is holding, but then the object may
                // be deleted soon afterwards.
                if (stub_created && input_stub->get_shared_count() == 0 && input_stub->get_optimistic_count() == 0)
                {
                    // take the cyonide pill die when people leave the room
                    input_stub->dont_keep_alive();
                }
            }
            CO_AWAIT child_transport->notify_all_destinations_of_disconnect();
            result.error_code = err_code;
            CO_RETURN result;
        }

        // Demarshal output interface if provided
        if (output_descr.get_object_id() != 0 && output_descr.is_set())
        {
            // Create service_proxy for this connection
            auto new_service_proxy = rpc::service_proxy::create(
                name, shared_from_this(), child_transport, child_transport->get_adjacent_zone_id());

            // add the proxy to the service
            add_zone_proxy(new_service_proxy);

            auto bind_result
                = CO_AWAIT rpc::proxy_bind_out_param<out_param_type, rpc::shared_ptr>(new_service_proxy, output_descr);
            err_code = bind_result.error_code;
            result.output_interface = std::move(bind_result.iface);
        }

        // we release the stub here because we did an add_ref in inner_connect.
        if (input_stub)
        {
            CO_AWAIT release_local_stub(input_stub, false, child_transport->get_adjacent_zone_id());
        }

        result.error_code = err_code;
        CO_RETURN result;
    }

    // Attach remote zone - for peer-to-peer connections
    // Takes single transport since this is called by the remote peer during connection
    template<class PARENT_INTERFACE, class CHILD_INTERFACE>
    CORO_TASK(remote_object_result)
    service::attach_remote_zone(const char* name,
        std::shared_ptr<transport> peer_transport,
        rpc::connection_settings input_descr,
        std::function<CORO_TASK(service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::service>)> fn)
    {
        remote_object_result result{rpc::error::OK(), {}};
        if (input_descr.inbound_interface_id != PARENT_INTERFACE::get_id(rpc::get_version()))
        {
            RPC_ERROR("inbound_interface_id does not match");
            result.error_code = rpc::error::INVALID_INTERFACE_ID();
            CO_RETURN result;
        }
        if (input_descr.outbound_interface_id != CHILD_INTERFACE::get_id(rpc::get_version()))
        {
            RPC_ERROR("outbound_interface_id does not match");
            result.error_code = rpc::error::INVALID_INTERFACE_ID();
            CO_RETURN result;
        }

        auto adjacent_zone_id = peer_transport->get_adjacent_zone_id();

        rpc::shared_ptr<PARENT_INTERFACE> parent_ptr;
        add_transport(input_descr.remote_object_id.as_zone(), peer_transport);
        transport_keep_alive ka(peer_transport, input_descr.remote_object_id.as_zone());
        transport_keep_alive adjacent_ka;
        if (input_descr.remote_object_id != adjacent_zone_id)
        {
            add_transport(adjacent_zone_id, peer_transport);
            adjacent_ka = transport_keep_alive(peer_transport, adjacent_zone_id);
        }

        if (input_descr.get_object_id() != 0)
        {
            auto parent_service_proxy = rpc::service_proxy::create(
                name, shared_from_this(), peer_transport, input_descr.remote_object_id.as_zone());

            add_zone_proxy(parent_service_proxy);

            bool new_proxy_added = true;

            auto proxy_result = CO_AWAIT parent_service_proxy->get_or_create_object_proxy(input_descr.get_object_id(),
                service_proxy::object_proxy_creation_rule::ADD_REF_IF_NEW,
                new_proxy_added,
                {adjacent_zone_id.get_address()},
                false);
            if (proxy_result.error_code != error::OK())
            {
                RPC_ERROR("get_or_create_object_proxy failed");
                result.error_code = proxy_result.error_code;
                CO_RETURN result;
            }
            auto op = std::move(proxy_result.object_proxy);
            RPC_ASSERT(op != nullptr);
            if (!op)
            {
                RPC_ERROR("Object not found - object proxy is null");
                result.error_code = rpc::error::OBJECT_NOT_FOUND();
                CO_RETURN result;
            }
            auto query_result = CO_AWAIT op->template query_interface<PARENT_INTERFACE>(false);
            if (query_result.error_code != rpc::error::OK())
            {
                result.error_code = query_result.error_code;
                CO_RETURN result;
            }
            parent_ptr = std::move(query_result.iface);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                telemetry_service->on_transport_inbound_add_ref(zone_id_,
                    adjacent_zone_id,
                    zone_id_.with_object(input_descr.get_object_id()),
                    adjacent_zone_id,
                    adjacent_zone_id,
                    rpc::add_ref_options::normal);
#endif
        }

        // Call local entry point to create child interface
        auto child_result = CO_AWAIT fn(parent_ptr, shared_from_this());
        if (child_result.error_code != rpc::error::OK())
        {
            result.error_code = child_result.error_code;
            CO_RETURN result;
        }
        auto child_ptr = std::move(child_result.output_interface);

        if (child_ptr)
        {
            auto bind_result
                = CO_AWAIT rpc::stub_bind_out_param(shared_from_this(), rpc::get_version(), adjacent_zone_id, child_ptr);
            result.error_code = bind_result.error_code;
            result.descriptor = bind_result.descriptor;

            if (result.error_code == rpc::error::OK())
            {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                    telemetry_service->on_transport_outbound_add_ref(zone_id_,
                        adjacent_zone_id,
                        zone_id_.with_object(result.descriptor.get_object_id()),
                        adjacent_zone_id,
                        zone_id_,
                        rpc::add_ref_options::build_caller_route);
#endif
            }
            CO_RETURN result;
        }

        CO_RETURN result;
    }

    template<class T, template<class> class PtrType>
    CORO_TASK(remote_object_result)
    service::remote_add_ref(uint64_t protocol_version, caller_zone caller_zone_id, PtrType<T> iface)
    {
        remote_object_result result{error::OK(), {}};
        constexpr bool optimistic = __rpc_pointer_traits::is_optimistic_v<PtrType<T>>;

        if (iface->__rpc_is_local())
        {
            result.error_code = error::INCOMPATIBLE_SERVICE();
            CO_RETURN result;
        }

        RPC_ASSERT(iface.get() != nullptr);

        RPC_ASSERT(!iface->__rpc_is_local());

        // Inline prepare_out_param logic here for out parameter binding.
        auto object_proxy = iface->__rpc_get_object_proxy();
        RPC_ASSERT(object_proxy != nullptr);
        auto object_id = object_proxy->get_object_id();

        auto object_service_proxy = object_proxy->get_service_proxy();
        RPC_ASSERT(object_service_proxy->zone_id_ == zone_id_);
        auto destination_zone_id = object_service_proxy->get_destination_zone_id();
        auto destination_transport = object_service_proxy->get_transport();

        std::shared_ptr<rpc::i_marshaller> marshaller;
        int err_code = get_or_create_link_between_source_and_destination(
            caller_zone_id, destination_zone_id, destination_transport, marshaller);
        if (err_code != error::OK())
        {
            result.error_code = err_code;
            CO_RETURN result;
        }

        auto known_direction = zone_id_;
        RPC_DEBUG("remote_add_ref: zone={}, dest_zone={}, caller_zone={}, "
                  "known_direction={}, destination_transport={}, obj_adj_zone={}",
            zone_id_.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            known_direction.get_subnet(),
            destination_transport != nullptr,
            destination_transport ? destination_transport->get_adjacent_zone_id().get_subnet() : 0);

        auto dest_with_obj = destination_zone_id.with_object(object_id);
        add_ref_params ar_params;
        ar_params.protocol_version = protocol_version;
        ar_params.remote_object_id = dest_with_obj;
        ar_params.caller_zone_id = caller_zone_id;
        ar_params.requesting_zone_id = known_direction;
        ar_params.build_out_param_channel = rpc::add_ref_options::build_destination_route
                                            | rpc::add_ref_options::build_caller_route
                                            | (optimistic ? add_ref_options::optimistic : add_ref_options::normal);
        ar_params.in_back_channel = empty_back_channel();
        auto ar_result = CO_AWAIT marshaller->add_ref(std::move(ar_params));
        err_code = ar_result.error_code;
        if (err_code != rpc::error::OK())
        {
            RPC_ERROR("remote_add_ref add_ref failed with code {}", err_code);
            result.error_code = err_code;
            CO_RETURN result;
        }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_proxy_add_ref(zone_id_,
                destination_zone_id.with_object(object_id),
                caller_zone_id,
                known_direction,
                rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route
                    | (optimistic ? add_ref_options::optimistic : add_ref_options::normal));
        }
#endif

        result.descriptor = destination_zone_id.with_object(object_id);
        CO_RETURN result;
    }

    template<class T, template<class> class PtrType>
    CORO_TASK(remote_object_result)
    service::stub_add_ref([[maybe_unused]] uint64_t protocol_version, caller_zone caller_zone_id, PtrType<T> iface)
    {
        remote_object_result result{error::OK(), {}};
        static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "stub_add_ref only supports rpc::shared_ptr and rpc::optimistic_ptr");

        constexpr bool optimistic = __rpc_pointer_traits::is_optimistic_v<PtrType<T>>;

        RPC_ASSERT(iface->__rpc_is_local());

        // For local interfaces or when caller_zone_id is not set, create a local stub.
        auto stub = iface->__rpc_get_stub();
        if (stub)
        {
            if constexpr (optimistic)
            {
                if (stub->get_shared_count() == 0)
                {
                    result.error_code = error::OBJECT_GONE();
                    CO_RETURN result;
                }
            }
        }
        else
        {
            if constexpr (optimistic)
            {
                result.error_code = error::OBJECT_GONE();
                CO_RETURN result;
            }
            else
            {
                std::lock_guard g(stub_control_);
                auto id = generate_new_object_id();
                stub = std::make_shared<object_stub>(id, shared_from_this(), iface);
                stubs_[id] = stub;
                stub->keep_self_alive();
                iface->__rpc_set_stub(stub);
            }
        }

        auto ret = CO_AWAIT stub->add_ref(optimistic, true, caller_zone_id); // outcall=true
        if (ret != rpc::error::OK())
        {
            result.error_code = ret;
            CO_RETURN result;
        }
        result.descriptor = zone_id_.with_object(stub->get_id());
        CO_RETURN result;
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

    // Converts a typed factory into a connection_handler, hiding the protocol
    // machinery (connection_settings, remote_object, attach_remote_zone).
    //
    // The factory signature matches the attach_remote_zone inner callback so that
    // error codes from in-factory RPC calls (e.g. set_host, set_callback) propagate
    // correctly back to the connection handshake.
    template<class Remote, class Local>
    connection_handler make_new_zone_connection_handler(const char* name,
        std::function<CORO_TASK(service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<rpc::service>)> factory)
    {
        // The handler owns its coroutine closure and the captures live with that handler.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        return [name_str = std::string(name), fn = std::move(factory)](rpc::connection_settings input,
                   std::shared_ptr<rpc::service> svc,
                   std::shared_ptr<rpc::transport> tp) -> CORO_TASK(connection_handler_result)
        {
            // forward to the service to bind the transport to its registerd transports proxies and stubs
            auto result = CO_AWAIT svc->attach_remote_zone<Remote, Local>(name_str.c_str(), tp, input, fn);
            CO_RETURN connection_handler_result{result.error_code, std::move(result.descriptor)};
        };
    }

    template<class Remote, class Local>
    CORO_TASK(std::shared_ptr<transport>)
    service::make_acceptor(std::string name,
        transport_factory factory,
        std::function<CORO_TASK(service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<service>)> fn)
    {
        auto handler = make_new_zone_connection_handler<Remote, Local>(name.c_str(), std::move(fn));
        CO_RETURN CO_AWAIT factory(std::move(name), shared_from_this(), std::move(handler));
    }
}
