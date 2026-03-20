/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>

// RPC headers
#include <rpc/rpc.h>

#ifdef _IN_ENCLAVE
#  include <fmt/format-inl.h>
#else
#  include <fmt/format.h>
#endif
namespace rpc
{
    ////////////////////////////////////////////////////////////////////////////
    // service

    thread_local service* current_service_ = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    service* service::get_current_service()
    {
        return current_service_;
    }
    void service::set_current_service(service* svc)
    {
        current_service_ = svc;
    }

    object service::generate_new_object_id() const
    {
        auto count = ++object_id_generator_;
        return {count};
    }

#ifdef CANOPY_BUILD_COROUTINE
    service::service(const char* name, zone zone_id, const std::shared_ptr<coro::scheduler>& scheduler)
        : zone_id_(zone_id)
        , name_(name)
        , io_scheduler_(scheduler)
    {
        RPC_ASSERT(zone_id_.get_subnet() != 0);
    }
    service::service(const char* name, zone zone_id, const std::shared_ptr<coro::scheduler>& scheduler, child_service_tag)
        : zone_id_(zone_id)
        , name_(name)
        , io_scheduler_(scheduler)
    {
        RPC_ASSERT(zone_id_.get_subnet() != 0);
        // No telemetry call for child services
    }

#else
    service::service(const char* name, zone zone_id)
        : zone_id_(zone_id)
        , name_(name)
    {
        RPC_ASSERT(zone_id_.get_subnet() != 0);
    }
    service::service(const char* name, zone zone_id, child_service_tag)
        : zone_id_(zone_id)
        , name_(name)
    {
        RPC_ASSERT(zone_id_.get_subnet() != 0);
        // No telemetry call for child services
    }

#endif

    ////////////////////////////////////////////////////////////////////////////
    // root_service

#ifdef CANOPY_BUILD_COROUTINE
    root_service::root_service(const char* name, zone zone_id, const std::shared_ptr<coro::scheduler>& scheduler)
        : service(name, zone_id, scheduler)
        , zone_allocator_(zone_id.get_address())
    {
#  ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_service_creation(name, zone_id, destination_zone());
#  endif
    }

    root_service::root_service(
        const char* name, const service_config& config, const std::shared_ptr<coro::scheduler>& scheduler)
        : root_service(name, config.initial_zone, scheduler)
    {
    }
#else
    root_service::root_service(const char* name, zone zone_id)
        : service(name, zone_id)
        , zone_allocator_(zone_id.get_address())
    {
#  ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_service_creation(name, zone_id, destination_zone());
#  endif
    }

    root_service::root_service(const char* name, const service_config& config)
        : root_service(name, config.initial_zone)
    {
    }
#endif

    CORO_TASK(new_zone_id_result)
    root_service::get_new_zone_id(get_new_zone_id_params params)
    {
        std::ignore = params;
        zone_address addr;
        if (auto ret = zone_allocator_.allocate_zone(addr); ret != rpc::error::OK())
            CO_RETURN new_zone_id_result{ret, {}, {}};
        CO_RETURN new_zone_id_result{rpc::error::OK(), zone{addr}, {}};
    }

    service::~service()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_service_deletion(zone_id_);
#endif

        // Child services use reference counting through service proxies to manage proper cleanup ordering.
        // Parent services maintain references to child services to prevent premature destruction.
        // The cleanup mechanism in service_proxy handles the proper ordering.

        object_id_generator_ = 0;
        // Verify all object stubs have been properly released before service destruction
        bool is_empty = check_is_empty();
        (void)is_empty;
        RPC_ASSERT(is_empty);

        {
            std::lock_guard l(stub_control_);
            stubs_.clear();
        }
        service_proxies_.clear();

        if (on_shutdown_)
        {
            on_shutdown_->set();
        }
    }

    object service::get_object_id(const rpc::shared_ptr<casting_interface>& ptr) const
    {
        if (ptr == nullptr)
            return {};
        if (ptr->__rpc_is_local())
        {
            auto stub = ptr->__rpc_get_stub();
            if (stub)
            {
                return stub->get_id();
            }
            else
            {
                return {};
            }
        }
        else
        {
            return casting_interface::get_object_id(*ptr);
        }
    }

    bool service::check_is_empty() const
    {
        // Acquire both locks up-front in a consistent order to avoid deadlock.
        // stub_control_ always precedes service_proxy_control_ across this codebase.
        std::scoped_lock l(stub_control_, service_proxy_control_);
        bool success = true;
        for (const auto& item : stubs_)
        {
            auto stub = item.second.lock();
            if (!stub)
            {
                RPC_WARNING("stub zone_id {}, object stub {} has been released but not deregistered in the service "
                            "suspected unclean shutdown",
                    std::to_string(zone_id_),
                    std::to_string(item.first));
            }
            else
            {
                RPC_WARNING("stub zone_id {}, object stub {} has not been released, there is a strong pointer "
                            "maintaining a positive reference count suspected unclean shutdown",
                    std::to_string(zone_id_),
                    std::to_string(item.first));
            }
            success = false;
        }

        for (const auto& item : service_proxies_)
        {
            auto svcproxy = item.second.lock();
            if (!svcproxy)
            {
                RPC_WARNING("service proxy zone_id {}, destination_zone_id {}, has been released "
                            "but not deregistered in the service",
                    std::to_string(zone_id_),
                    std::to_string(item.first));
            }
            else
            {
                RPC_WARNING("service proxy zone_id {}, destination_zone_id {} "
                            "has not been released in the service suspected unclean shutdown",
                    std::to_string(zone_id_),
                    std::to_string(item.first));

                for (const auto& proxy : svcproxy->get_proxies())
                {
                    auto op = proxy.second.lock();
                    if (op)
                    {
                        RPC_WARNING(" has object_proxy {}", std::to_string(op->get_object_id()));
                    }
                    else
                    {
                        RPC_WARNING(" has null object_proxy");
                    }
                    success = false;
                }
            }
            success = false;
        }

        // Check for live transports
        // Note: For child_service, the parent_transport is expected to still be alive during shutdown
        // as it's where the thread goes when shutting down this zone
        const auto* child_svc = dynamic_cast<const child_service*>(this);
        std::shared_ptr<transport> expected_parent_transport;
        destination_zone expected_parent_zone_id;
        if (child_svc)
        {
            expected_parent_transport = child_svc->get_parent_transport();
            expected_parent_zone_id = child_svc->get_parent_zone_id();
        }

        for (const auto& item : transports_)
        {
            auto transport_ptr = item.second.lock();
            if (!transport_ptr)
            {
                RPC_WARNING("transport zone_id {}, destination_zone_id {} has been released "
                            "but not deregistered in the service",
                    std::to_string(zone_id_),
                    std::to_string(item.first));
                success = false;
            }
            else
            {
                // For child_service, allow the parent transport to still exist during shutdown
                if (child_svc && expected_parent_transport == transport_ptr && item.first == expected_parent_zone_id)
                {
                    RPC_DEBUG("transport zone_id {}, parent_zone_id {} still active during child_service shutdown "
                              "(expected behavior)",
                        std::to_string(zone_id_),
                        std::to_string(item.first));
                    continue;
                }

                RPC_WARNING("transport zone_id {}, destination_zone_id {} (adjacent_zone={}) "
                            "has not been released suspected unclean shutdown",
                    std::to_string(zone_id_),
                    std::to_string(item.first),
                    std::to_string(transport_ptr->get_adjacent_zone_id()));
                success = false;
            }
        }

        return success;
    }

    CORO_TASK(send_result)
    service::send(send_params params)
    {
        auto object_id = params.remote_object_id.get_object_id();

        // overriden versions of this functions my have more to do with these parameters
        std::ignore = params.tag;
        std::ignore = params.in_back_channel;

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_send(
                zone_id_, params.remote_object_id, params.caller_zone_id, params.interface_id, params.method_id);
        }
#endif
        if (!params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            RPC_ERROR("Routing error: send() reached wrong zone - should have been routed via passthrough");
            RPC_ASSERT(false);
            CO_RETURN send_result{error::TRANSPORT_ERROR(), {}, {}};
        }
        current_service_tracker tracker(this);
        if (params.protocol_version < rpc::LOWEST_SUPPORTED_VERSION
            || params.protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in send", params.protocol_version);
            CO_RETURN send_result{rpc::error::INVALID_VERSION(), {}, {}};
        }

        std::weak_ptr<object_stub> weak_stub = get_object(object_id);
        auto stub = weak_stub.lock();
        if (stub == nullptr)
        {
            RPC_INFO("Object gone - stub has already been released");
            CO_RETURN send_result{rpc::error::OBJECT_GONE(), {}, {}};
        }

        CO_RETURN CO_AWAIT stub->call(std::move(params));
    }

    CORO_TASK(void)
    service::post(post_params params)
    {
        auto object_id = params.remote_object_id.get_object_id();

        // overriden versions of this functions my have more to do with these parameters
        std::ignore = params.tag;
        std::ignore = params.in_back_channel;

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_post(
                zone_id_, params.remote_object_id, params.caller_zone_id, params.interface_id, params.method_id);
        }
#endif

        if (!params.remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            RPC_ERROR("Routing error: post() reached wrong zone - should have been routed via passthrough");
            RPC_ASSERT(false);
            CO_RETURN;
        }

        current_service_tracker tracker(this);

        // Log that post was received
        RPC_TRACE("service::post received for destination_zone={} object_id={}",
            params.remote_object_id.get_subnet(),
            object_id.get_val());

        if (params.protocol_version < rpc::LOWEST_SUPPORTED_VERSION
            || params.protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in post", params.protocol_version);
            CO_RETURN;
        }

        std::weak_ptr<object_stub> weak_stub = get_object(object_id);
        auto stub = weak_stub.lock();
        if (stub == nullptr)
        {
            RPC_INFO("Object gone - stub has already been released");
            CO_RETURN;
        }

        // For local post, we just call the stub without waiting for a response
        // Note: back-channel is not applicable for post operations (fire-and-forget)
        // Convert post_params to send_params for stub dispatch (identical fields)
        send_params send{FLD(protocol_version) params.protocol_version,
            FLD(encoding_type) params.encoding_type,
            FLD(tag) params.tag,
            FLD(caller_zone_id) params.caller_zone_id,
            FLD(remote_object_id) params.remote_object_id,
            FLD(interface_id) params.interface_id,
            FLD(method_id) params.method_id,
            FLD(in_data) std::move(params.in_data),
            FLD(in_back_channel) std::move(params.in_back_channel)};
        CO_AWAIT stub->call(std::move(send));

        // Log that post was delivered to local stub
        RPC_TRACE("service::post delivered to local stub for object_id={} in zone={}",
            object_id.get_val(),
            zone_id_.get_subnet());

        CO_RETURN;
    }

    CORO_TASK(void)
    service::clean_up_on_failed_connection(
        std::shared_ptr<rpc::service_proxy> destination_zone, rpc::shared_ptr<rpc::casting_interface> input_interface)
    {
        if (destination_zone && input_interface)
        {
            auto object_id = casting_interface::get_object_id(*input_interface);
            auto ret = CO_AWAIT destination_zone->sp_release(object_id, release_options::normal);
            if (ret == error::OK())
            {
                // destination_zone->release_external_ref();
            }
            else
            {
                RPC_ERROR("destination_zone->sp_release failed with code {}", ret);
            }
        }
    }

    // this is a key function that returns an interface descriptor
    // for wrapping an implementation to a local object inside a stub where needed
    // or if the interface is a proxy to add ref it
    CORO_TASK(remote_object_bind_result)
    service::get_descriptor_from_interface_stub(
        caller_zone caller_zone_id, rpc::shared_ptr<rpc::casting_interface> iface, bool optimistic)
    {
        remote_object_bind_result result{error::OK(), nullptr, {}};
        if (!iface)
        {
            result.error_code = error::INVALID_DATA();
            CO_RETURN result;
        }
        if (!iface->__rpc_is_local())
        {
            // we should not be getting the interface from remote objects
            result.error_code = error::OBJECT_NOT_FOUND();
            CO_RETURN result;
        }
        {
            std::lock_guard g(stub_control_);
            result.stub = iface->__rpc_get_stub();
            if (!result.stub)
            {
                {
                    auto id = generate_new_object_id();
                    result.stub = std::make_shared<object_stub>(id, shared_from_this(), iface);
                    stubs_[id] = result.stub;
                    result.stub->keep_self_alive();
                    iface->__rpc_set_stub(result.stub);
                }
            }
        }
        auto ret = CO_AWAIT result.stub->add_ref(optimistic, false, caller_zone_id);
        if (ret != rpc::error::OK())
        {
            result.error_code = ret;
            CO_RETURN result;
        }
        result.descriptor = zone_id_.with_object(result.stub->get_id());
        CO_RETURN result;
    }

    std::weak_ptr<object_stub> service::get_object(object object_id) const
    {
        std::lock_guard l(stub_control_);
        auto item = stubs_.find(object_id);
        if (item == stubs_.end())
        {
            // Stub has been deleted - can happen with optimistic_ptr when shared_ptr is released
            return std::weak_ptr<object_stub>();
        }

        return item->second;
    }
    CORO_TASK(standard_result)
    service::try_cast(try_cast_params params)
    {
        auto protocol_version = params.protocol_version;
        auto remote_object_id = params.remote_object_id;
        auto interface_id = params.interface_id;
        auto object_id = remote_object_id.get_object_id();

        std::ignore = params.caller_zone_id;
        std::ignore = params.in_back_channel;

        if (!remote_object_id.get_address().same_zone(zone_id_.get_address()))
        {
            RPC_ERROR("Routing error: try_cast() reached wrong zone - should have been routed via passthrough");
            RPC_ASSERT(false);
            CO_RETURN standard_result{error::TRANSPORT_ERROR(), {}};
        }
        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in try_cast", protocol_version);
            CO_RETURN standard_result{rpc::error::INVALID_VERSION(), {}};
        }
        std::weak_ptr<object_stub> weak_stub = get_object(object_id);
        auto stub = weak_stub.lock();
        if (!stub)
        {
            RPC_ERROR("Invalid data - stub is null in try_cast");
            CO_RETURN standard_result{error::INVALID_DATA(), {}};
        }
        CO_RETURN standard_result{stub->try_cast(interface_id), {}};
    }

    CORO_TASK(standard_result)
    service::add_ref(add_ref_params params)
    {
        auto protocol_version = params.protocol_version;
        auto remote_object_id = params.remote_object_id;
        auto caller_zone_id = params.caller_zone_id;
        auto requesting_zone_id = params.requesting_zone_id;
        auto build_out_param_channel = params.build_out_param_channel;
        auto object_id = remote_object_id.get_object_id();

        bool optimistic = !!(build_out_param_channel & add_ref_options::optimistic);
        bool build_caller_channel = !!(build_out_param_channel & add_ref_options::build_caller_route);
        bool build_dest_channel = !!(build_out_param_channel & add_ref_options::build_destination_route)
                                  || build_out_param_channel == add_ref_options::normal
                                  || build_out_param_channel == add_ref_options::optimistic;

        RPC_ASSERT(!!build_caller_channel || !!build_dest_channel);

        current_service_tracker tracker(this);
        if (build_caller_channel)
        {
            if (zone_id_ != caller_zone_id)
            {
                auto caller_transport = get_transport(caller_zone_id);
                add_ref_params caller_params;
                caller_params.protocol_version = protocol_version;
                caller_params.remote_object_id = remote_object_id;
                caller_params.caller_zone_id = caller_zone_id;
                caller_params.requesting_zone_id = zone_id_;
                caller_params.build_out_param_channel
                    = add_ref_options::build_caller_route
                      | (optimistic ? add_ref_options::optimistic : add_ref_options::normal);
                caller_params.in_back_channel = params.in_back_channel;
                auto caller_result = CO_AWAIT caller_transport->add_ref(std::move(caller_params));
                if (caller_result.error_code != rpc::error::OK())
                {
                    RPC_ERROR("Caller channel add_ref failed with code {}", caller_result.error_code);
                    CO_RETURN caller_result;
                }
            }
            else
            {
                std::lock_guard g(service_proxy_control_);
                auto destination_transport = inner_get_transport(remote_object_id.as_zone());
                if (destination_transport == nullptr)
                {
                    destination_transport = inner_get_transport(requesting_zone_id);
                    if (destination_transport == nullptr)
                    {
                        RPC_ERROR("Destination transport not found for zone {}", remote_object_id.get_subnet());
                        CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
                    }
                    inner_add_transport(remote_object_id.as_zone(), destination_transport);
                }
            }
        }

        if (build_dest_channel)
        {
            if (!remote_object_id.get_address().same_zone(zone_id_.get_address()))
            {
                auto dest_transport = get_transport(remote_object_id.as_zone());
                add_ref_params dest_params;
                dest_params.protocol_version = protocol_version;
                dest_params.remote_object_id = remote_object_id;
                dest_params.caller_zone_id = caller_zone_id;
                dest_params.requesting_zone_id = requesting_zone_id;
                dest_params.build_out_param_channel = build_out_param_channel & (~add_ref_options::build_caller_route);
                dest_params.in_back_channel = params.in_back_channel;
                CO_RETURN CO_AWAIT dest_transport->add_ref(std::move(dest_params));
            }

            // service has the implementation
            if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
            {
                RPC_ERROR("Unsupported service version {} in add_ref", protocol_version);
                CO_RETURN standard_result{rpc::error::INVALID_VERSION(), {}};
            }

            if (object_id == dummy_object_id)
            {
                CO_RETURN standard_result{rpc::error::OK(), {}};
            }

            std::weak_ptr<object_stub> weak_stub = get_object(object_id);
            auto stub = weak_stub.lock();
            if (!stub)
            {
                RPC_ERROR("Stub found in registry but already expired in add_ref: object_id={}", object_id.get_val());
                RPC_ASSERT(false);
                CO_RETURN standard_result{rpc::error::OBJECT_NOT_FOUND(), {}};
            }

            {
                std::lock_guard g(service_proxy_control_);
                auto dest_transport = inner_get_transport(caller_zone_id);
                if (!dest_transport)
                {
                    dest_transport = inner_get_transport(requesting_zone_id);
                    inner_add_transport(caller_zone_id, dest_transport);
                }
            }

            auto ret = CO_AWAIT stub->add_ref(
                !!(build_out_param_channel & add_ref_options::optimistic), false, caller_zone_id);
            if (ret != rpc::error::OK())
            {
                CO_RETURN standard_result{ret, {}};
            }
        }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_add_ref(
                zone_id_, remote_object_id, caller_zone_id, requesting_zone_id, build_out_param_channel);
        }
#endif
        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(uint64_t)
    service::release_local_stub(std::shared_ptr<rpc::object_stub> stub, bool is_optimistic, caller_zone caller_zone_id)
    {
        uint64_t count = stub->release(is_optimistic, caller_zone_id);
        if (!count && !is_optimistic)
        {
            auto object_id = stub->get_id();
            // When shared count drops to zero, notify all transports that have optimistic references.
            // Snapshot is taken before erasing from service maps so no zone is missed.
            auto optimistic_refs = stub->get_zones_with_optimistic_refs();

            {
                // a scoped lock - erase stub from maps
                std::lock_guard l(stub_control_);
                stubs_.erase(object_id);
            } // Release stub_control_ lock before calling object_released

            stub->dont_keep_alive();
            // stub = nullptr;

            // Now notify all transports that had optimistic references
            // IMPORTANT: This must be done AFTER releasing stub_control_ mutex to avoid deadlock
            // In synchronous mode or with local transport, object_released can chain back
            // to code that tries to acquire stub_control_ again
            for (const auto& caller_zone_id : optimistic_refs)
            {
                auto transport = get_transport(caller_zone_id);
                if (transport)
                {
                    // Call object_released on the transport to notify the remote zone
                    // This is a fire-and-forget operation
                    object_released_params or_params;
                    or_params.protocol_version = rpc::get_version();
                    or_params.remote_object_id = zone_id_.with_object(object_id); // destination with embedded object_id
                    or_params.caller_zone_id = caller_zone_id;
                    CO_AWAIT transport->object_released(std::move(or_params));
                }
            }
        }
        CO_RETURN count;
    }

    void service::cleanup_service_proxy(const std::shared_ptr<rpc::service_proxy>& other_zone)
    {
        std::ignore = other_zone;
    }

    CORO_TASK(standard_result)
    service::release(release_params params)
    {
        auto protocol_version = params.protocol_version;
        auto remote_object_id = params.remote_object_id;
        auto caller_zone_id = params.caller_zone_id;
        auto options = params.options;
        auto object_id = remote_object_id.get_object_id();

        std::ignore = params.in_back_channel;

        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in release", protocol_version);
            CO_RETURN standard_result{rpc::error::INVALID_VERSION(), {}};
        }

        {
            std::shared_ptr<rpc::object_stub> stub;
            {
                // a scoped lock
                std::lock_guard l(stub_control_);
                auto item = stubs_.find(object_id);
                if (item == stubs_.end())
                {
                    // Stub has been deleted - can happen with optimistic_ptr when shared_ptr is released
                    CO_RETURN standard_result{rpc::error::OBJECT_NOT_FOUND(), {}};
                }

                stub = item->second.lock();
            }

            if (!stub)
            {
                RPC_ERROR("Stub found in registry but already expired in release: object_id={}", object_id.get_val());
                RPC_ASSERT(false);
                CO_RETURN standard_result{rpc::error::OBJECT_NOT_FOUND(), {}};
            }

            CO_AWAIT release_local_stub(stub, !!(release_options::optimistic & options), caller_zone_id);
        }

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_release(zone_id_, remote_object_id, caller_zone_id, options);
        }
#endif
        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(void)
    service::object_released(object_released_params params)
    {
        auto protocol_version = params.protocol_version;
        auto remote_object_id = params.remote_object_id;
        auto caller_zone_id = params.caller_zone_id;
        auto object_id = remote_object_id.get_object_id();

        std::ignore = params.in_back_channel;
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_object_released(zone_id_, remote_object_id, caller_zone_id);
        }
#endif
        if (caller_zone_id != zone_id_)
        {
            RPC_ERROR("Routing error: object_released() reached wrong zone - should have been routed via passthrough");
            RPC_ASSERT(false);
            CO_RETURN;
        }
        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in object_released", protocol_version);
            CO_RETURN;
        }

        // Notify that an object has been released
        CO_AWAIT notify_object_gone_event(object_id, remote_object_id.as_zone());
    }

    CORO_TASK(void)
    service::transport_down(transport_down_params params)
    {
        auto protocol_version = params.protocol_version;
        auto destination_zone_id = params.destination_zone_id;
        auto caller_zone_id = params.caller_zone_id;

        std::ignore = params.in_back_channel;
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_transport_down(zone_id_, destination_zone_id, caller_zone_id);
        }
#endif

        if (!destination_zone_id.get_address().same_zone(zone_id_.get_address()))
        {
            RPC_ERROR("Routing error: transport_down() reached wrong zone - should have been routed via passthrough");
            RPC_ASSERT(false);
            CO_RETURN;
        }
        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in transport_down", protocol_version);
            CO_RETURN;
        }

        RPC_INFO("Transport down notification received from caller_zone={} to destination_zone={}",
            caller_zone_id.get_subnet(),
            destination_zone_id.get_subnet());

        // Collect all stubs that have references from the failed zone
        std::vector<std::pair<object, std::shared_ptr<object_stub>>> stubs_to_cleanup;
        std::vector<object> objects_to_notify;
        {
            std::lock_guard l(stub_control_);
            for (auto& [obj_id, weak_stub] : stubs_)
            {
                auto stub = weak_stub.lock();
                if (stub && stub->has_references_from_zone(caller_zone_id))
                {
                    stubs_to_cleanup.emplace_back(obj_id, stub);
                }
            }

            RPC_INFO("transport_down: Found {} stubs with references from zone {}",
                stubs_to_cleanup.size(),
                caller_zone_id.get_subnet());

            // Release all references from the failed zone and trigger object_released events
            for (auto& [obj_id, stub] : stubs_to_cleanup)
            {
                bool should_delete = stub->release_all_from_zone(caller_zone_id);

                if (should_delete)
                {
                    // Shared count reached zero - stub should be deleted
                    RPC_DEBUG("transport_down: Object {} ref count dropped to zero, cleaning up", obj_id.get_val());

                    // Remove from maps
                    stubs_.erase(obj_id);

                    // Track for notification
                    objects_to_notify.push_back(obj_id);

                    stub->dont_keep_alive();
                }
            }
        } // Release stub_control_ before calling notify

        // Notify service events about deleted objects (outside the lock)
        for (const auto& obj_id : objects_to_notify)
        {
            CO_AWAIT notify_object_gone_event(obj_id, destination_zone_id);
        }

        RPC_INFO("transport_down: Cleanup complete, {} objects deleted", objects_to_notify.size());
    }

    void service::inner_add_zone_proxy(const std::shared_ptr<rpc::service_proxy>& service_proxy)
    {
        // this is for internal use only has no lock
        // service_proxy->add_external_ref();
        auto destination_zone_id = service_proxy->get_destination_zone_id();
        // auto caller_zone_id = service_proxy->get_caller_zone_id();
        RPC_ASSERT(destination_zone_id != zone_id_);
        RPC_ASSERT(service_proxies_.find(destination_zone_id) == service_proxies_.end());
        service_proxies_[destination_zone_id] = service_proxy;

        RPC_ASSERT(transports_.find(destination_zone_id) != transports_.end());
        // transports_[destination_zone_id] = service_proxy->get_transport();
        RPC_DEBUG("inner_add_zone_proxy service zone: {} destination_zone={} adjacent_zone={}",
            std::to_string(zone_id_),
            std::to_string(service_proxy->destination_zone_id_),
            std::to_string(service_proxy->get_transport()->get_adjacent_zone_id()));
    }

    void service::add_zone_proxy(const std::shared_ptr<rpc::service_proxy>& service_proxy)
    {
        RPC_ASSERT(service_proxy->get_destination_zone_id() != zone_id_);
        std::lock_guard g(service_proxy_control_);
        inner_add_zone_proxy(service_proxy);
    }

    void service::inner_add_transport(destination_zone destination_zone_id, const std::shared_ptr<transport>& transport_ptr)
    {
        RPC_ASSERT(destination_zone_id.get_subnet());
        RPC_ASSERT(transports_.find(destination_zone_id) == transports_.end());
        transports_[destination_zone_id] = transport_ptr;
        RPC_DEBUG("inner_add_transport service zone: {} destination_zone={} adjacent_zone={}",
            std::to_string(zone_id_),
            std::to_string(destination_zone_id),
            std::to_string(transport_ptr->get_adjacent_zone_id()));
    }

    void service::inner_remove_transport(destination_zone destination_zone_id)
    {
        RPC_ASSERT(destination_zone_id.get_subnet());
        auto it = transports_.find(destination_zone_id);
        if (it != transports_.end())
        {
            auto dest = it->second.lock();
            if (!dest)
            {
                RPC_ERROR("inner_remove_transport: Transport for zone={} is in registry but already expired",
                    destination_zone_id.get_subnet());
                RPC_ASSERT(false);
            }
            transports_.erase(it);
            RPC_DEBUG("remove_transport service zone: {} destination_zone_id={}",
                std::to_string(zone_id_),
                std::to_string(destination_zone_id));
        }
    }
    std::shared_ptr<rpc::transport> service::inner_get_transport(destination_zone destination_zone_id) const
    {
        RPC_ASSERT(destination_zone_id.get_subnet());
        // Try to find a direct transport to the destination zone
        auto item = transports_.find(destination_zone_id);
        if (item != transports_.end())
        {
            auto transport = item->second.lock();
            if (transport)
            {
                return transport;
            }
        }
        return nullptr;
    }

    void service::add_transport(destination_zone destination_zone_id, const std::shared_ptr<transport>& transport_ptr)
    {
        std::lock_guard g(service_proxy_control_);
        inner_add_transport(destination_zone_id, transport_ptr);
    }

    void service::remove_transport(destination_zone adjacent_zone_id)
    {
        std::lock_guard g(service_proxy_control_);
        inner_remove_transport(adjacent_zone_id);
    }

    std::shared_ptr<rpc::transport> service::get_transport(destination_zone destination_zone_id) const
    {
        std::lock_guard g(service_proxy_control_);
        return inner_get_transport(destination_zone_id);
    }

    std::shared_ptr<rpc::service_proxy> service::get_zone_proxy(
        caller_zone caller_zone_id, // when you know who is calling you
        destination_zone destination_zone_id,
        bool& new_proxy_added)
    {
        new_proxy_added = false;
        std::lock_guard g(service_proxy_control_);

        RPC_DEBUG("get_zone_proxy: svc_zone={}, dest={}, caller_zone={}, num_transports={}",
            zone_id_.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.is_set() ? caller_zone_id.get_subnet() : 0,
            transports_.size());

        {
            auto item = service_proxies_.find(destination_zone_id);
            if (item != service_proxies_.end())
            {
                RPC_DEBUG("get_zone_proxy: Found existing proxy in service_proxies_", 0);
                return item->second.lock();
            }
        }

        {
            auto item = transports_.find(destination_zone_id);
            if (item != transports_.end())
            {
                auto transport = item->second.lock();
                if (transport)
                {
                    auto proxy = service_proxy::create(fmt::format("SP#{}", destination_zone_id.get_subnet()),
                        shared_from_this(),
                        transport,
                        destination_zone_id);
                    inner_add_zone_proxy(proxy);
                    new_proxy_added = true;
                    return proxy;
                }
            }
        }

        if (caller_zone_id.is_set())
        {
            auto item = transports_.find(caller_zone_id);
            if (item != transports_.end())
            {
                auto transport = item->second.lock();
                if (transport)
                {
                    auto proxy = service_proxy::create(fmt::format("SP#{}", destination_zone_id.get_subnet()),
                        shared_from_this(),
                        transport,
                        destination_zone_id);
                    inner_add_transport(destination_zone_id, transport);
                    inner_add_zone_proxy(proxy);
                    new_proxy_added = true;
                    return proxy;
                }
            }
        }

        RPC_ERROR("get_zone_proxy: Could not find route! svc_zone={}, dest={}, caller_zone={}",
            zone_id_.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.is_set() ? caller_zone_id.get_subnet() : 0);

        return nullptr;
    }

    void service::remove_zone_proxy(destination_zone destination_zone_id)
    {
        RPC_DEBUG("remove_zone_proxy service zone: {} destination_zone={}",
            std::to_string(zone_id_),
            std::to_string(destination_zone_id));

        std::lock_guard g(service_proxy_control_);
        auto item = service_proxies_.find(destination_zone_id);
        if (item == service_proxies_.end())
        {
            RPC_ERROR("remove_zone_proxy: destination_zone {} not found in service {}",
                std::to_string(destination_zone_id),
                std::to_string(zone_id_));
            return;
        }
        service_proxies_.erase(item);
    }

    void service::add_service_event(const std::weak_ptr<service_event>& event)
    {
        std::lock_guard g(service_events_control_);
        service_events_.insert(event);
    }
    void service::remove_service_event(const std::weak_ptr<service_event>& event)
    {
        std::lock_guard g(service_events_control_);
        service_events_.erase(event);
    }
    CORO_TASK(void) service::notify_object_gone_event(object object_id, destination_zone destination)
    {
        std::set<std::weak_ptr<service_event>, std::owner_less<std::weak_ptr<service_event>>> service_events_copy;
        {
            std::lock_guard g(service_events_control_);
            if (!service_events_.empty())
            {
                service_events_copy = service_events_;
            }
        }
        for (const auto& se : service_events_copy)
        {
            auto se_handler = se.lock();
            if (se_handler)
                CO_AWAIT se_handler->on_object_released(object_id, destination);
        }
        CO_RETURN;
    }

    child_service::~child_service()
    {
        // Disconnect parent transport to break circular reference
        auto parent = get_parent_transport();
        if (parent)
        {
            parent->set_status(transport_status::DISCONNECTED);
        }
    }

    CORO_TASK(new_zone_id_result)
    child_service::get_new_zone_id(get_new_zone_id_params params)
    {
        auto parent = get_parent_transport();
        if (!parent)
            CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        CO_RETURN CO_AWAIT parent->get_new_zone_id(std::move(params));
    }

    ///////////////////////////////////////////////////////////////////////////////
    // Default implementations of outbound functions
    ///////////////////////////////////////////////////////////////////////////////

    CORO_TASK(send_result)
    service::outbound_send(send_params params, std::shared_ptr<transport> transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->send(std::move(params));
    }

    CORO_TASK(void)
    service::outbound_post(post_params params, std::shared_ptr<transport> transport)
    {
        // Default implementation - directly call the transport
        CO_AWAIT transport->post(std::move(params));
    }

    CORO_TASK(standard_result)
    service::outbound_try_cast(try_cast_params params, std::shared_ptr<transport> transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->try_cast(std::move(params));
    }

    CORO_TASK(standard_result)
    service::outbound_add_ref(add_ref_params params, std::shared_ptr<transport> transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->add_ref(std::move(params));
    }

    CORO_TASK(standard_result)
    service::outbound_release(release_params params, std::shared_ptr<transport> transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->release(std::move(params));
    }

    // Efficient targeted cleanup for a specific remote zone
    // This version is called when the transport knows exactly which zone connection failed
    CORO_TASK(void)
    service::notify_transport_down([[maybe_unused]] std::shared_ptr<transport> transport, destination_zone remote_zone)
    {
        std::vector<remote_object> objects_to_notify;

        {
            std::lock_guard l(stub_control_);

            // Clean up service proxy for this specific zone
            {
                std::lock_guard g(service_proxy_control_);
                auto zit = service_proxies_.find(remote_zone);
                if (zit != service_proxies_.end())
                {
                    auto sp = zit->second.lock();
                    if (sp)
                    {
                        sp->set_transport(nullptr);
                    }
                    service_proxies_.erase(zit);
                }
            }

            // Clean up stubs referenced by this specific zone
            for (auto& [obj_id, weak_stub] : stubs_)
            {
                auto stub = weak_stub.lock();
                if (!stub)
                    continue;

                bool should_delete = stub->release_all_from_zone(remote_zone);

                if (should_delete)
                {
                    // Shared count reached zero - stub should be deleted
                    RPC_DEBUG("transport_down: Object {} ref count from zone {} dropped to zero, cleaning up",
                        obj_id.get_val(),
                        remote_zone.get_subnet());

                    objects_to_notify.push_back(remote_zone.with_object(obj_id));
                    stub->dont_keep_alive();
                }
            }

            // Remove deleted stubs from map inside the lock (separate pass avoids iterator invalidation)
            for (const auto& obj : objects_to_notify)
                stubs_.erase(obj.get_object_id());

            // Remove the transport entry for the disconnected zone.
            // When notify_transport_down is called (from transport cleanup), the transport's
            // destination_count may still be non-zero because release_all_from_zone does not
            // decrement transport counts. We must explicitly remove the stale entry here to
            // keep transports_ consistent so the service destructor's check_is_empty() passes.
            {
                std::lock_guard g(service_proxy_control_);
                transports_.erase(remote_zone);
            }
        } // stub_control_ released here

        // Notify service events about deleted objects (outside the lock)
        for (const auto& obj : objects_to_notify)
            CO_AWAIT notify_object_gone_event(obj.get_object_id(), obj.as_zone());
    }

    int service::get_or_create_link_between_source_and_destination(caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        const std::shared_ptr<rpc::transport>& destination_transport,
        std::shared_ptr<rpc::i_marshaller>& marshaller)
    {
        RPC_ASSERT(caller_zone_id.is_set());
        RPC_ASSERT(destination_zone_id.is_set());

        if (caller_zone_id == destination_zone_id)
        {
            marshaller = destination_transport;
            if (!marshaller)
            {
                return error::TRANSPORT_ERROR();
            }
        }
        else
        {
            marshaller = destination_transport->get_passthrough(destination_zone_id, caller_zone_id);
            if (!marshaller)
            {
                std::shared_ptr<rpc::transport> caller_transport;
                {
                    std::lock_guard g(service_proxy_control_);
                    auto found = transports_.find(caller_zone_id);
                    if (found == transports_.end())
                    {
                        RPC_ERROR("No service proxy found for caller zone {}", caller_zone_id.get_subnet());
                        return error::ZONE_NOT_FOUND();
                    }
                    else
                    {
                        caller_transport = found->second.lock();
                        if (!caller_transport)
                        {
                            RPC_ERROR("Failed to obtain valid caller service_proxy");
                            return error::SERVICE_PROXY_LOST_CONNECTION();
                        }
                    }
                }

                if (destination_transport == caller_transport)
                {
                    marshaller = destination_transport;
                }
                else
                {
                    marshaller = transport::create_pass_through(
                        destination_transport, caller_transport, shared_from_this(), destination_zone_id, caller_zone_id);
                }
            }
        }
        return error::OK();
    }
}
